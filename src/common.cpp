#include "common.h"

#include <algorithm>
#include <cwctype>

namespace dps {

// ---- time -------------------------------------------------------------------

int64_t QpcNow() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

int64_t QpcFrequency() {
    static const int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? value.QuadPart : 1;
    }();
    return frequency;
}

double QpcToSeconds(int64_t ticks) { return static_cast<double>(ticks) / static_cast<double>(QpcFrequency()); }

int64_t SecondsToQpc(double seconds) { return static_cast<int64_t>(seconds * static_cast<double>(QpcFrequency())); }

double SecondsSince(int64_t start) { return QpcToSeconds(QpcNow() - start); }

namespace {

constexpr int64_t kUnixEpochAsFileTime = 116444736000000000LL;  // 100 ns ticks from 1601 to 1970

FILETIME UnixMillisToFileTime(int64_t unixMillis) {
    ULARGE_INTEGER value;
    value.QuadPart = static_cast<ULONGLONG>(unixMillis * 10000LL + kUnixEpochAsFileTime);
    FILETIME ft;
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    return ft;
}

std::string FormatSystemTime(const SYSTEMTIME& st, const char* format) {
    std::string out;
    for (const char* p = format; *p; ++p) {
        if (*p != '%' || !p[1]) {
            out += *p;
            continue;
        }
        ++p;
        switch (*p) {
            case 'Y': out += StrFormat("%04u", st.wYear); break;
            case 'm': out += StrFormat("%02u", st.wMonth); break;
            case 'd': out += StrFormat("%02u", st.wDay); break;
            case 'H': out += StrFormat("%02u", st.wHour); break;
            case 'M': out += StrFormat("%02u", st.wMinute); break;
            case 'S': out += StrFormat("%02u", st.wSecond); break;
            case 'L': out += StrFormat("%03u", st.wMilliseconds); break;
            case '%': out += '%'; break;
            default: out += '%'; out += *p; break;
        }
    }
    return out;
}

}  // namespace

int64_t UnixMillisNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER value;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    return (static_cast<int64_t>(value.QuadPart) - kUnixEpochAsFileTime) / 10000LL;
}

std::string IsoUtc(int64_t unixMillis) {
    FILETIME ft = UnixMillisToFileTime(unixMillis);
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st))
        return "1970-01-01T00:00:00.000Z";
    return FormatSystemTime(st, "%Y-%m-%dT%H:%M:%S.%LZ");
}

std::string LocalTimeString(int64_t unixMillis, const char* format) {
    FILETIME utc = UnixMillisToFileTime(unixMillis);
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st))
        return "1970-01-01";
    return FormatSystemTime(st, format);
}

// ---- strings ----------------------------------------------------------------

std::string VStrFormat(const char* fmt, va_list args) {
    char stack[512];
    va_list copy;
    va_copy(copy, args);
    const int needed = vsnprintf(stack, sizeof(stack), fmt, copy);
    va_end(copy);
    if (needed < 0)
        return {};
    if (static_cast<size_t>(needed) < sizeof(stack))
        return std::string(stack, static_cast<size_t>(needed));

    std::string out(static_cast<size_t>(needed) + 1, '\0');
    vsnprintf(out.data(), out.size(), fmt, args);
    out.resize(static_cast<size_t>(needed));
    return out;
}

std::string StrFormat(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string out = VStrFormat(fmt, args);
    va_end(args);
    return out;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
        return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0)
        return {};
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
    return out;
}

std::string ThousandsSep(long long value) {
    const bool negative = value < 0;
    unsigned long long magnitude = negative ? 0ULL - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value);
    std::string digits = std::to_string(magnitude);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + 1);
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return negative ? "-" + out : out;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ContainsInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty())
        return true;
    // Byte-wise ASCII folding is enough for the markers we search for; other UTF-8
    // bytes compare exactly.
    auto fold = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [&](char a, char b) { return fold(a) == fold(b); });
    return it != haystack.end();
}

// ---- files ------------------------------------------------------------------

std::wstring GameDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return L".";
        if (length < path.size()) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring DataDirectory() {
    static const std::wstring dir = JoinPath(GameDirectory(), L"nativePC\\plugins\\MhwDpsMeter");
    return dir;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty())
        return b;
    if (a.back() == L'\\' || a.back() == L'/')
        return a + b;
    return a + L"\\" + b;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty())
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2)
        EnsureDirectory(path.substr(0, slash));
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart < (256LL << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD read = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 24));
            ok = ReadFile(file, out.data() + done, chunk, &read, nullptr) && read > 0;
            done += read;
        }
        out.resize(done);
    }
    CloseHandle(file);
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, std::string_view contents) {
    const std::wstring tmp = path + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    bool ok = true;
    size_t done = 0;
    while (ok && done < contents.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(contents.size() - done, 1u << 24));
        ok = WriteFile(file, contents.data() + done, chunk, &written, nullptr) && written > 0;
        done += written;
    }
    CloseHandle(file);

    if (ok)
        ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileW(tmp.c_str());
    return ok;
}

// ---- threads ----------------------------------------------------------------

namespace {

struct ThreadStart {
    void (*fn)(void*);
    void* arg;
};

DWORD WINAPI ThreadTrampoline(LPVOID param) {
    ThreadStart start = *static_cast<ThreadStart*>(param);
    delete static_cast<ThreadStart*>(param);
    start.fn(start.arg);
    return 0;
}

}  // namespace

bool RunDetached(void (*fn)(void*), void* arg) {
    auto* start = new ThreadStart{fn, arg};
    HANDLE thread = CreateThread(nullptr, 0, ThreadTrampoline, start, 0, nullptr);
    if (!thread) {
        delete start;
        return false;
    }
    CloseHandle(thread);
    return true;
}

}  // namespace dps
