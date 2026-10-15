#include "log.h"

#include <atomic>
#include <deque>

#include "common.h"

namespace dps::log {
namespace {

constexpr size_t kRecentLines = 200;

Lock g_lock;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_path;
Level g_minLevel = Level::Info;
std::deque<std::string> g_recent;

const char* LevelName(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?";
}

}  // namespace

void Init(const std::wstring& path) {
    LockGuard guard(g_lock);
    if (g_file != INVALID_HANDLE_VALUE)
        return;

    g_path = path;
    const std::wstring previous = path + L".prev";
    MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
}

void SetMinLevel(Level level) {
    LockGuard guard(g_lock);
    g_minLevel = level;
}

void Write(Level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const std::string message = VStrFormat(fmt, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    std::string line = StrFormat("%02u:%02u:%02u.%03u [%s] [%5lu] %s", now.wHour, now.wMinute, now.wSecond,
                                 now.wMilliseconds, LevelName(level), GetCurrentThreadId(), message.c_str());

    LockGuard guard(g_lock);
    if (level < g_minLevel)
        return;

    g_recent.push_back(line);
    while (g_recent.size() > kRecentLines)
        g_recent.pop_front();

    line += "\r\n";
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    OutputDebugStringA(line.c_str());
}

std::vector<std::string> Recent(size_t count) {
    LockGuard guard(g_lock);
    const size_t start = g_recent.size() > count ? g_recent.size() - count : 0;
    return {g_recent.begin() + static_cast<std::ptrdiff_t>(start), g_recent.end()};
}

std::wstring Path() {
    LockGuard guard(g_lock);
    return g_path;
}

}  // namespace dps::log

void DpsImGuiAssertFailed(const char* expression, const char* file, int line) {
    static std::atomic<int> reported{0};
    if (reported.fetch_add(1) < 20)
        LOG_ERROR("ImGui assertion failed: %s (%s:%d)", expression, file, line);
}
