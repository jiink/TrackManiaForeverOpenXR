#pragma once

#include <string_view>

namespace tmoxr::log {
void Initialize();
void Write(std::string_view level, std::string_view message);
void Info(std::string_view message);
void Warn(std::string_view message);
void Error(std::string_view message);
} // namespace tmoxr::log
