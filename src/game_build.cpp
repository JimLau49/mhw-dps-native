#include "game_build.h"

#include <cstring>
#include <set>

#include "common.h"

namespace dps::build {
namespace {

constexpr wchar_t kTitlePrefix[] = L"MONSTER HUNTER: WORLD(";

int ParseTitle(const wchar_t* title) {
    const wchar_t* start = wcsstr(title, kTitlePrefix);
    if (!start)
        return 0;
    start += wcslen(kTitlePrefix);
    int value = 0;
    int digits = 0;
    while (*start >= L'0' && *start <= L'9' && digits < 8) {
        value = value * 10 + (*start - L'0');
        ++start;
        ++digits;
    }
    return (*start == L')' && digits >= 5) ? value : 0;
}

struct WindowSearch {
    DWORD pid;
    HWND window;
    int build;
};

/// InternalGetWindowText reads the title without sending WM_GETTEXT, so a busy game
/// thread cannot block us (GetWindowText would wait for it).
int WindowTitle(HWND hwnd, wchar_t* title, int capacity) {
    using InternalGetWindowTextFn = int(WINAPI*)(HWND, LPWSTR, int);
    static const auto internal = reinterpret_cast<InternalGetWindowTextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "InternalGetWindowText"));
    return internal ? internal(hwnd, title, capacity) : GetWindowTextW(hwnd, title, capacity);
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<WindowSearch*>(param);
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != search->pid)
        return TRUE;

    wchar_t title[256];
    const int length = WindowTitle(hwnd, title, static_cast<int>(std::size(title)));
    if (length <= 0)
        return TRUE;
    title[std::size(title) - 1] = L'\0';
    if (!wcsstr(title, kTitlePrefix))
        return TRUE;

    search->window = hwnd;
    search->build = ParseTitle(title);
    return FALSE;
}

}  // namespace

int FromRdata(const mem::ModuleInfo& exe) {
    const mem::Section* rdata = exe.Find(".rdata");
    if (!rdata || rdata->size < 8)
        return 0;

    // Standalone "4xxxxx": NUL, '4', five digits, NUL. Chunked guarded reads keep a
    // partially unmapped section from faulting.
    std::set<int> candidates;
    std::vector<uint8_t> chunk(1 << 20);
    constexpr size_t kOverlap = 8;
    for (size_t offset = 0; offset < rdata->size;) {
        const size_t length = std::min(chunk.size(), rdata->size - offset);
        if (mem::ReadBytes(rdata->start + offset, chunk.data(), length)) {
            for (size_t i = 0; i + 8 <= length; ++i) {
                if (chunk[i] != 0 || chunk[i + 1] != '4' || chunk[i + 7] != 0)
                    continue;
                bool digits = true;
                for (size_t k = 2; k < 7; ++k)
                    digits = digits && chunk[i + k] >= '0' && chunk[i + k] <= '9';
                if (digits)
                    candidates.insert(std::atoi(reinterpret_cast<const char*>(&chunk[i + 1])));
            }
        }
        if (length < chunk.size())
            break;
        offset += length - kOverlap;
    }
    return candidates.size() == 1 ? *candidates.begin() : 0;
}

int FromWindowTitle(HWND* window) {
    WindowSearch search{GetCurrentProcessId(), nullptr, 0};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&search));
    if (window)
        *window = search.window;
    return search.build;
}

HWND FindGameWindow() {
    HWND window = nullptr;
    FromWindowTitle(&window);
    return window;
}

int FromStrackerLoader() {
    HMODULE loader = GetModuleHandleW(L"loader.dll");
    if (!loader)
        return 0;
    // const char* loader::GameVersion
    auto* slot = reinterpret_cast<const char* const*>(GetProcAddress(loader, "?GameVersion@loader@@3PEBDEB"));
    uintptr_t text = 0;
    if (!slot || !mem::ReadPtr(reinterpret_cast<uintptr_t>(slot), text))
        return 0;
    std::string version;
    if (!mem::ReadCString(text, 32, version))
        return 0;
    // "421810" or e.g. "15.23.00 (421810)": take the first run of 5-7 digits.
    for (size_t i = 0; i < version.size(); ++i) {
        size_t j = i;
        while (j < version.size() && version[j] >= '0' && version[j] <= '9')
            ++j;
        if (j - i >= 5 && j - i <= 7)
            return std::atoi(version.substr(i, j - i).c_str());
        if (j > i)
            i = j;
    }
    return 0;
}

}  // namespace dps::build
