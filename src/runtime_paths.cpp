#include "runtime_paths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <system_error>
#include <vector>

namespace tmoxr {
namespace {

std::filesystem::path CurrentModulePath() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&CurrentModulePath), &module)) {
        return {};
    }

    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    return std::filesystem::path(std::wstring(path.data(), length));
}

} // namespace

std::filesystem::path ModuleDirectory() {
    return CurrentModulePath().parent_path();
}

std::filesystem::path ModuleFilePath(const wchar_t* fileName) {
    const auto directory = ModuleDirectory();
    return directory.empty() ? std::filesystem::path(fileName)
                             : directory / fileName;
}

std::filesystem::path UserDataDirectory() {
    PWSTR documentsPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(
            FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documentsPath)) &&
        documentsPath) {
        const std::filesystem::path directory =
            std::filesystem::path(documentsPath) / L"TrackMania";
        CoTaskMemFree(documentsPath);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (!error) return directory;
    } else if (documentsPath) {
        CoTaskMemFree(documentsPath);
    }
    return ModuleDirectory();
}

std::filesystem::path UserDataFilePath(const wchar_t* fileName) {
    const auto directory = UserDataDirectory();
    return directory.empty() ? std::filesystem::path(fileName)
                             : directory / fileName;
}

} // namespace tmoxr
