#include "large_address_aware.h"

#include <Windows.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace {

bool IsFileLargeAddressAware(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos{};
    DWORD read = 0;
    bool okay = ReadFile(file, &dos, sizeof(dos), &read, nullptr) &&
        read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    LARGE_INTEGER position{};
    position.QuadPart = dos.e_lfanew;
    IMAGE_NT_HEADERS32 nt{};
    okay = okay && SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
        ReadFile(file, &nt, sizeof(nt), &read, nullptr) && read == sizeof(nt) &&
        nt.Signature == IMAGE_NT_SIGNATURE;
    CloseHandle(file);
    return okay &&
        (nt.FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

bool OnlyLargeAddressFlagChanged(const std::wstring& original,
                                 const std::wstring& patched) {
    auto readFile = [](const std::wstring& path, std::vector<unsigned char>& bytes) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        const bool sized = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
            size.QuadPart <= 64ll * 1024ll * 1024ll;
        if (!sized) {
            CloseHandle(file);
            return false;
        }
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        const bool okay = bytes.empty() ||
            (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                      &read, nullptr) && read == bytes.size());
        CloseHandle(file);
        return okay;
    };

    std::vector<unsigned char> before;
    std::vector<unsigned char> after;
    if (!readFile(original, before) || !readFile(patched, after) ||
        before.size() != after.size()) {
        return false;
    }
    size_t differences = 0;
    unsigned char changedBits = 0;
    for (size_t index = 0; index < before.size(); ++index) {
        if (before[index] == after[index]) continue;
        ++differences;
        changedBits = static_cast<unsigned char>(before[index] ^ after[index]);
    }
    return differences == 1 && changedBits == IMAGE_FILE_LARGE_ADDRESS_AWARE;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: large_address_aware_smoke.exe <32-bit exe>\n");
        return 1;
    }

    wchar_t temporaryDirectory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporaryDirectory)) return 2;
    wchar_t temporaryFile[MAX_PATH]{};
    if (!GetTempFileNameW(temporaryDirectory, L"tfx", 0, temporaryFile)) return 3;
    DeleteFileW(temporaryFile);
    const std::wstring testPath = std::wstring(temporaryFile) + L".exe";
    if (!CopyFileW(argv[1], testPath.c_str(), FALSE)) return 4;

    // Ensure the test exercises the transition even if the supplied executable
    // was already LAA: clear only the copied file's flag first.
    HANDLE copy = CreateFileW(testPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (copy == INVALID_HANDLE_VALUE) return 5;
    IMAGE_DOS_HEADER dos{};
    DWORD transferred = 0;
    ReadFile(copy, &dos, sizeof(dos), &transferred, nullptr);
    LARGE_INTEGER position{};
    position.QuadPart = dos.e_lfanew + sizeof(DWORD) +
        offsetof(IMAGE_FILE_HEADER, Characteristics);
    SetFilePointerEx(copy, position, nullptr, FILE_BEGIN);
    WORD characteristics = 0;
    ReadFile(copy, &characteristics, sizeof(characteristics), &transferred, nullptr);
    characteristics = static_cast<WORD>(
        characteristics & ~IMAGE_FILE_LARGE_ADDRESS_AWARE);
    SetFilePointerEx(copy, position, nullptr, FILE_BEGIN);
    WriteFile(copy, &characteristics, sizeof(characteristics), &transferred, nullptr);
    CloseHandle(copy);

    const auto result = tmoxr::laa::PatchExecutable(testPath);
    const bool passed = result.status == tmoxr::laa::PatchStatus::Patched &&
        IsFileLargeAddressAware(testPath) &&
        GetFileAttributesW(result.backupPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
        OnlyLargeAddressFlagChanged(result.backupPath, testPath);

    DeleteFileW(testPath.c_str());
    DeleteFileW(result.backupPath.c_str());
    std::printf("status=%d, patched=%d\n",
                static_cast<int>(result.status), passed ? 1 : 0);
    return passed ? 0 : 6;
}
