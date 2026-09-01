#include <Windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3d9.lib")

FARPROC ImportedFunction(const char* functionName) {
    auto* const module = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto* const dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    auto* const nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(module + dos->e_lfanew);
    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        module + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const char* const library = reinterpret_cast<const char*>(
            module + descriptor->Name);
        if (_stricmp(library, "d3d9.dll") != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA32*>(module +
            (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk
                                            : descriptor->FirstThunk));
        auto* functions = reinterpret_cast<IMAGE_THUNK_DATA32*>(
            module + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++functions) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) continue;
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                module + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name),
                            functionName) == 0) {
                return reinterpret_cast<FARPROC>(functions->u1.Function);
            }
        }
    }
    return nullptr;
}

int main() {
    HMODULE tmoxr = LoadLibraryW(L"TMFOXR.dll");
    if (!tmoxr) {
        std::fprintf(stderr, "LoadLibraryW(TMFOXR.dll) failed: %lu\n",
                     GetLastError());
        return 1;
    }
    using InjectionStatusFn = DWORD(WINAPI*)();
    const auto injectionStatus = reinterpret_cast<InjectionStatusFn>(
        GetProcAddress(tmoxr, "TMFOXR_GetInjectionStatus"));
    D3DPERF_SetOptions(0);
    if (IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION)) d3d->Release();

    const bool create9Redirected =
        ImportedFunction("Direct3DCreate9") ==
        GetProcAddress(tmoxr, "Direct3DCreate9");
    const bool perfRedirected =
        ImportedFunction("D3DPERF_SetOptions") ==
        GetProcAddress(tmoxr, "D3DPERF_SetOptions");
    const bool success = create9Redirected && perfRedirected;
    std::printf("status=0x%08lx, Direct3DCreate9 redirected=%d, D3DPERF_SetOptions redirected=%d\n",
                injectionStatus ? injectionStatus() : 0,
                create9Redirected, perfRedirected);
    FreeLibrary(tmoxr);
    return success ? 0 : 2;
}
