#include "large_address_aware.h"

#include <cstddef>
#include <cstdint>

namespace tmoxr::laa {
namespace {

constexpr wchar_t kBackupSuffix[] = L".TMFOXR-backup";

bool ReadAt(HANDLE file, uint64_t offset, void* destination, DWORD size,
            DWORD& error) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        error = GetLastError();
        return false;
    }
    DWORD read = 0;
    if (!ReadFile(file, destination, size, &read, nullptr) || read != size) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) error = ERROR_HANDLE_EOF;
        return false;
    }
    return true;
}

bool WriteAt(HANDLE file, uint64_t offset, const void* source, DWORD size,
             DWORD& error) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        error = GetLastError();
        return false;
    }
    DWORD written = 0;
    if (!WriteFile(file, source, size, &written, nullptr) || written != size) {
        error = GetLastError();
        if (error == ERROR_SUCCESS) error = ERROR_WRITE_FAULT;
        return false;
    }
    if (!FlushFileBuffers(file)) {
        error = GetLastError();
        return false;
    }
    return true;
}

bool LocateCharacteristics(HANDLE file, uint64_t& offset,
                           WORD& characteristics, DWORD& error) {
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize)) {
        error = GetLastError();
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    if (!ReadAt(file, 0, &dos, sizeof(dos), error)) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        error = ERROR_BAD_EXE_FORMAT;
        return false;
    }

    const uint64_t ntOffset = static_cast<uint64_t>(dos.e_lfanew);
    const uint64_t minimumEnd = ntOffset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
        sizeof(WORD);
    if (minimumEnd > static_cast<uint64_t>(fileSize.QuadPart)) {
        error = ERROR_BAD_EXE_FORMAT;
        return false;
    }

    DWORD signature = 0;
    if (!ReadAt(file, ntOffset, &signature, sizeof(signature), error)) return false;
    if (signature != IMAGE_NT_SIGNATURE) {
        error = ERROR_BAD_EXE_FORMAT;
        return false;
    }

    IMAGE_FILE_HEADER fileHeader{};
    if (!ReadAt(file, ntOffset + sizeof(signature), &fileHeader,
                sizeof(fileHeader), error)) {
        return false;
    }
    if (fileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        fileHeader.SizeOfOptionalHeader < sizeof(WORD)) {
        error = ERROR_BAD_EXE_FORMAT;
        return false;
    }

    WORD optionalMagic = 0;
    if (!ReadAt(file, ntOffset + sizeof(signature) + sizeof(fileHeader),
                &optionalMagic, sizeof(optionalMagic), error)) {
        return false;
    }
    if (optionalMagic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        error = ERROR_BAD_EXE_FORMAT;
        return false;
    }

    offset = ntOffset + sizeof(signature) +
        offsetof(IMAGE_FILE_HEADER, Characteristics);
    characteristics = fileHeader.Characteristics;
    return true;
}

} // namespace

bool IsCurrentProcessLargeAddressAware() {
    const auto* module = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!module) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(module + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
        nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
        (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

bool IsExecutableLargeAddressAware(const std::wstring& executablePath,
                                   DWORD* windowsError) {
    if (windowsError) *windowsError = ERROR_SUCCESS;
    HANDLE file = CreateFileW(
        executablePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (windowsError) *windowsError = GetLastError();
        return false;
    }
    uint64_t characteristicsOffset = 0;
    WORD characteristics = 0;
    DWORD error = ERROR_SUCCESS;
    const bool valid = LocateCharacteristics(
        file, characteristicsOffset, characteristics, error);
    CloseHandle(file);
    if (windowsError) *windowsError = error;
    return valid &&
        (characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

PatchResult PatchExecutable(const std::wstring& executablePath) {
    PatchResult result{};
    result.backupPath = executablePath + kBackupSuffix;

    HANDLE inspection = CreateFileW(
        executablePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (inspection == INVALID_HANDLE_VALUE) {
        result.status = PatchStatus::OpenFailed;
        result.windowsError = GetLastError();
        return result;
    }

    uint64_t characteristicsOffset = 0;
    WORD characteristics = 0;
    DWORD error = ERROR_SUCCESS;
    const bool valid = LocateCharacteristics(
        inspection, characteristicsOffset, characteristics, error);
    CloseHandle(inspection);
    if (!valid) {
        result.status = error == ERROR_BAD_EXE_FORMAT
            ? PatchStatus::InvalidExecutable : PatchStatus::ReadFailed;
        result.windowsError = error;
        return result;
    }
    if ((characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0) {
        result.status = PatchStatus::AlreadyEnabled;
        return result;
    }

    if (!CopyFileW(executablePath.c_str(), result.backupPath.c_str(), TRUE)) {
        error = GetLastError();
        if (error != ERROR_FILE_EXISTS) {
            result.status = PatchStatus::BackupFailed;
            result.windowsError = error;
            return result;
        }
    }

    HANDLE file = CreateFileW(
        executablePath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.status = PatchStatus::OpenFailed;
        result.windowsError = GetLastError();
        return result;
    }

    characteristics = static_cast<WORD>(
        characteristics | IMAGE_FILE_LARGE_ADDRESS_AWARE);
    bool wrote = WriteAt(
        file, characteristicsOffset, &characteristics,
        sizeof(characteristics), error);
    if (wrote) {
        WORD verified = 0;
        uint64_t verifiedOffset = 0;
        wrote = LocateCharacteristics(
            file, verifiedOffset, verified, error) &&
            verifiedOffset == characteristicsOffset &&
            (verified & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
        if (!wrote && error == ERROR_SUCCESS) error = ERROR_WRITE_FAULT;
    }
    CloseHandle(file);

    result.status = wrote ? PatchStatus::Patched : PatchStatus::WriteFailed;
    result.windowsError = wrote ? ERROR_SUCCESS : error;
    return result;
}

} // namespace tmoxr::laa
