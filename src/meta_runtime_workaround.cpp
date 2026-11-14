#include "meta_runtime_workaround.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace tmoxr {
namespace {

FARPROC WINAPI GetProcAddressWithoutThreadDescription(HMODULE module, LPCSTR name) {
    // Names below 0x10000 are ordinals, not strings.
    if (reinterpret_cast<uintptr_t>(name) > 0xFFFF &&
        std::strcmp(name, "SetThreadDescription") == 0) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    return GetProcAddress(module, name);
}

bool IsMetaRuntimeModule(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return false;
    std::wstring lowered(path.data(), length);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return lowered.find(L"\\oculus-runtime\\") != std::wstring::npos;
}

unsigned RedirectGetProcAddressImports(HMODULE module) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return 0;

    const auto replacement = reinterpret_cast<ULONG_PTR>(&GetProcAddressWithoutThreadDescription);
    unsigned redirected = 0;
    for (const auto* descriptor =
             reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
         descriptor->Name; ++descriptor) {
        // The name table still holds the import names after the loader has
        // overwritten the address table; without one the slot cannot be identified.
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(
            const_cast<uint8_t*>(base) + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(import->Name, "GetProcAddress") != 0) continue;
            if (slots->u1.Function == replacement) continue;
            DWORD previousProtection = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function),
                    PAGE_READWRITE, &previousProtection)) continue;
            slots->u1.Function = replacement;
            VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function),
                previousProtection, &previousProtection);
            ++redirected;
        }
    }
    return redirected;
}

} // namespace

unsigned HideThreadDescriptionFromMetaRuntime() {
#ifdef _WIN64
    return 0;
#else
    std::vector<HMODULE> modules(1024);
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(),
            static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) return 0;
    modules.resize(std::min<size_t>(modules.size(), bytes / sizeof(HMODULE)));
    unsigned redirected = 0;
    for (const HMODULE module : modules) {
        if (IsMetaRuntimeModule(module)) redirected += RedirectGetProcAddressImports(module);
    }
    return redirected;
#endif
}

} // namespace tmoxr
