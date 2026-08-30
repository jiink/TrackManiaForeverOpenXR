#include "log.h"
#include "runtime_paths.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace tmoxr::log {
namespace {
std::mutex g_mutex;
std::ofstream g_file;

std::filesystem::path LogPath() {
    return ModuleFilePath(L"TMOXR.log");
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}
} // namespace

void Initialize() {
    std::scoped_lock lock(g_mutex);
    if (g_file.is_open()) return;
    // One launch, one diagnostic file. Keeping prior sessions made it too easy
    // to mistake an old experimental message for the currently deployed build.
    g_file.open(LogPath(), std::ios::out | std::ios::trunc);
    if (g_file.is_open()) {
        g_file << "========== TrackMania OpenXR bridge started ==========" << std::endl;
    }
}

void Write(std::string_view level, std::string_view message) {
    Initialize();
    std::scoped_lock lock(g_mutex);
    const std::string line = "[" + Timestamp() + "][" + std::string(level) + "][T" +
        std::to_string(GetCurrentThreadId()) + "] " + std::string(message);
    OutputDebugStringA((line + "\n").c_str());
    if (g_file.is_open()) {
        g_file << line << std::endl;
        g_file.flush();
    }
}

void Info(std::string_view message) { Write("INFO", message); }
void Warn(std::string_view message) { Write("WARN", message); }
void Error(std::string_view message) { Write("ERROR", message); }
} // namespace tmoxr::log
