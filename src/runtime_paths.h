#pragma once

#include <filesystem>

namespace tmoxr {

std::filesystem::path ModuleDirectory();
std::filesystem::path ModuleFilePath(const wchar_t* fileName);

} // namespace tmoxr
