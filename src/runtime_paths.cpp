#include "runtime_paths.h"

#include <Windows.h>

#include <vector>

namespace tmoxr {

std::filesystem::path ModuleDirectory() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module)) {
        return {};
    }

    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

std::filesystem::path ModuleFilePath(const wchar_t* fileName) {
    const auto directory = ModuleDirectory();
    return directory.empty() ? std::filesystem::path(fileName)
                             : directory / fileName;
}

} // namespace tmoxr
