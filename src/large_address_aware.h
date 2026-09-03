#pragma once

#include <Windows.h>

#include <string>

namespace tmoxr::laa {

enum class PatchStatus {
    AlreadyEnabled,
    Patched,
    InvalidExecutable,
    BackupFailed,
    OpenFailed,
    ReadFailed,
    WriteFailed,
};

struct PatchResult {
    PatchStatus status = PatchStatus::InvalidExecutable;
    DWORD windowsError = ERROR_SUCCESS;
    std::wstring backupPath;
};

bool IsCurrentProcessLargeAddressAware();
bool IsExecutableLargeAddressAware(const std::wstring& executablePath,
                                   DWORD* windowsError = nullptr);
PatchResult PatchExecutable(const std::wstring& executablePath);

} // namespace tmoxr::laa
