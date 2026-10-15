#pragma once

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#ifndef DPS_VERSION
#define DPS_VERSION "0.0.0"
#endif
#ifndef DPS_BUILD_STAMP
#define DPS_BUILD_STAMP "dev"
#endif

namespace dps {

constexpr int kPartySlots = 4;

/// "0.1.0+2026-10-15 12:00Z": version plus build time, so a stale DLL is obvious in-game.
constexpr const char* kBuildStamp = DPS_VERSION "+" DPS_BUILD_STAMP;

// ---- time -------------------------------------------------------------------

/// QueryPerformanceCounter ticks.
int64_t QpcNow();
int64_t QpcFrequency();
double QpcToSeconds(int64_t ticks);
int64_t SecondsToQpc(double seconds);
/// Seconds from start to now (QPC).
double SecondsSince(int64_t start);

/// Milliseconds since the Unix epoch (wall clock, UTC).
int64_t UnixMillisNow();
/// "2026-10-15T12:34:56.789Z"
std::string IsoUtc(int64_t unixMillis);
/// Local wall time formatted with strftime-style specifiers ("%Y-%m-%d_%H%M%S").
std::string LocalTimeString(int64_t unixMillis, const char* format);

// ---- strings ----------------------------------------------------------------

std::string StrFormat(const char* fmt, ...) __attribute__((format(gnu_printf, 1, 2)));
std::string VStrFormat(const char* fmt, va_list args);
std::wstring Utf8ToWide(std::string_view text);
std::string WideToUtf8(std::wstring_view text);
/// "1,234,567"
std::string ThousandsSep(long long value);
bool StartsWith(std::string_view text, std::string_view prefix);
bool ContainsInsensitive(std::string_view haystack, std::string_view needle);

// ---- files ------------------------------------------------------------------

/// Folder that holds MonsterHunterWorld.exe.
std::wstring GameDirectory();
/// <game>/nativePC/plugins/MhwDpsMeter — settings, logs and address overrides.
std::wstring DataDirectory();
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
bool EnsureDirectory(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool ReadWholeFile(const std::wstring& path, std::string& out);
/// Writes path.tmp then renames over path, so readers never see a torn file.
bool WriteFileAtomic(const std::wstring& path, std::string_view contents);

// ---- locking ----------------------------------------------------------------

/// SRWLOCK wrapper. Never held across calls into game code.
class Lock {
public:
    Lock() { InitializeSRWLock(&lock_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    void lock() { AcquireSRWLockExclusive(&lock_); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&lock_) != 0; }
    void unlock() { ReleaseSRWLockExclusive(&lock_); }

private:
    SRWLOCK lock_;
};

class LockGuard {
public:
    explicit LockGuard(Lock& lock) : lock_(lock) { lock_.lock(); }
    ~LockGuard() { lock_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lock& lock_;
};

/// Runs fn on a new detached thread. Returns false if the thread could not be created.
bool RunDetached(void (*fn)(void*), void* arg);

}  // namespace dps
