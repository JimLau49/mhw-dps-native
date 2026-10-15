#include "memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>

#include "common.h"

// Guarded copy. A fault anywhere between dps_safe_copy_begin and
// dps_safe_copy_fault is redirected by the vectored handler below to
// dps_safe_copy_fault, which returns 0. Win64: rcx = dst, rdx = src, r8 = size;
// rsi/rdi are callee-saved, so they are pushed and restored on both paths.
extern "C" int dps_safe_copy(void* dst, const void* src, size_t size);
extern "C" char dps_safe_copy_begin[];
extern "C" char dps_safe_copy_fault[];

__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl dps_safe_copy\n"
    ".globl dps_safe_copy_begin\n"
    ".globl dps_safe_copy_fault\n"
    ".def dps_safe_copy; .scl 2; .type 32; .endef\n"
    ".seh_proc dps_safe_copy\n"
    "dps_safe_copy:\n"
    "dps_safe_copy_begin:\n"
    "    pushq %rsi\n"
    "    .seh_pushreg %rsi\n"
    "    pushq %rdi\n"
    "    .seh_pushreg %rdi\n"
    "    .seh_endprologue\n"
    "    movq %rcx, %rdi\n"
    "    movq %rdx, %rsi\n"
    "    movq %r8, %rcx\n"
    "    rep movsb\n"
    "    popq %rdi\n"
    "    popq %rsi\n"
    "    movl $1, %eax\n"
    "    ret\n"
    "dps_safe_copy_fault:\n"
    "    popq %rdi\n"
    "    popq %rsi\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".seh_endproc\n");

namespace dps::mem {
namespace {

constexpr DWORD kStatusGuardPageViolation = 0x80000001;
constexpr size_t kLenientReadLimit = 64;
constexpr ULONGLONG kRegionCacheMs = 200;

PVOID g_handler = nullptr;
std::atomic<uint64_t> g_faults{0};

LONG CALLBACK GuardedCopyHandler(PEXCEPTION_POINTERS info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR && code != kStatusGuardPageViolation)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t rip = info->ContextRecord->Rip;
    if (rip >= reinterpret_cast<uintptr_t>(dps_safe_copy_begin) && rip < reinterpret_cast<uintptr_t>(dps_safe_copy_fault)) {
        info->ContextRecord->Rip = reinterpret_cast<uintptr_t>(dps_safe_copy_fault);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

constexpr DWORD kReadableProtect = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                   PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

struct RegionCache {
    uintptr_t start = 0;
    uintptr_t end = 0;
    ULONGLONG at = 0;
};

thread_local RegionCache t_region;

/// VirtualQuery pre-check. It rejects guard pages (touching one would strip the
/// guard from another thread's stack) and obviously unmapped memory; the guarded
/// copy still covers memory freed between this check and the read.
bool RegionReadable(uintptr_t address, size_t size) {
    const uintptr_t end = address + size;
    const ULONGLONG now = GetTickCount64();
    if (address >= t_region.start && end <= t_region.end && now - t_region.at < kRegionCacheMs)
        return true;

    uintptr_t cursor = address;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &info, sizeof(info)) != sizeof(info))
            return size <= kLenientReadLimit;  // Wine can refuse queries on some heap views.

        if (info.State != MEM_COMMIT)
            return false;
        if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || (info.Protect & kReadableProtect) == 0)
            return false;

        const uintptr_t regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const uintptr_t regionEnd = regionStart + info.RegionSize;
        if (regionEnd <= cursor)
            return size <= kLenientReadLimit;

        if (cursor == address) {
            t_region.start = regionStart;
            t_region.end = regionEnd;
            t_region.at = now;
        }
        cursor = regionEnd;
    }
    return true;
}

}  // namespace

void Init() {
    if (!g_handler)
        g_handler = AddVectoredExceptionHandler(1, GuardedCopyHandler);
}

void Shutdown() {
    if (g_handler) {
        RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
}

uint64_t FaultCount() { return g_faults.load(std::memory_order_relaxed); }

bool ReadBytes(uintptr_t address, void* out, size_t size) {
    if (size == 0 || !IsUserPtr(address) || size > kUserMax - address)
        return false;
    if (!RegionReadable(address, size))
        return false;
    return dps_safe_copy(out, reinterpret_cast<const void*>(address), size) != 0;
}

bool ReadPtr(uintptr_t address, uintptr_t& out) {
    uintptr_t value = 0;
    if (!Read(address, value) || !IsUserPtr(value))
        return false;
    out = value;
    return true;
}

uintptr_t Follow(uintptr_t address, const std::vector<int>& offsets, std::string* error) {
    uintptr_t current = address;
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (!IsUserPtr(current)) {
            if (error)
                *error = StrFormat("hop %zu/%zu bad address 0x%llX", i, offsets.size(), static_cast<unsigned long long>(current));
            return 0;
        }

        uintptr_t next = 0;
        if (!Read(current, next)) {
            if (error)
                *error = StrFormat("hop %zu/%zu unreadable 0x%llX", i, offsets.size(), static_cast<unsigned long long>(current));
            return 0;
        }
        if (next == 0) {
            if (error)
                *error = i == 0 ? StrFormat("not allocated at 0x%llX", static_cast<unsigned long long>(current))
                                : StrFormat("hop %zu/%zu null at 0x%llX", i, offsets.size(), static_cast<unsigned long long>(current));
            return 0;
        }
        current = next + static_cast<intptr_t>(offsets[i]);
    }

    if (error)
        error->clear();
    return current;
}

bool ReadCString(uintptr_t address, size_t maxBytes, std::string& out) {
    out.clear();
    char chunk[64];
    size_t total = 0;
    while (total <= maxBytes) {
        const uintptr_t at = address + total;
        // Never cross a page boundary in one read: a short name can end right before
        // an unmapped page.
        const size_t toPageEnd = 0x1000 - (at & 0xFFF);
        size_t want = std::min({sizeof(chunk), toPageEnd, maxBytes + 1 - total});
        if (!ReadBytes(at, chunk, want))
            return false;
        const void* nul = std::memchr(chunk, 0, want);
        if (nul) {
            out.append(chunk, static_cast<const char*>(nul) - chunk);
            return true;
        }
        out.append(chunk, want);
        total += want;
    }
    out.clear();
    return false;
}

// ---- modules ------------------------------------------------------------------

const Section* ModuleInfo::Find(std::string_view name) const {
    for (const auto& section : sections) {
        if (section.name == name)
            return &section;
    }
    return nullptr;
}

bool QueryModule(HMODULE module, ModuleInfo& out) {
    out = {};
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos{};
    if (!Read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000)
        return false;

    IMAGE_NT_HEADERS64 nt{};
    if (!Read(base + static_cast<uintptr_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE)
        return false;

    out.base = base;
    out.size = nt.OptionalHeader.SizeOfImage;
    out.timestamp = nt.FileHeader.TimeDateStamp;

    uintptr_t sectionAt = base + static_cast<uintptr_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                          nt.FileHeader.SizeOfOptionalHeader;
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections && i < 96; ++i, sectionAt += sizeof(IMAGE_SECTION_HEADER)) {
        IMAGE_SECTION_HEADER header{};
        if (!Read(sectionAt, header))
            break;
        Section section;
        section.name.assign(reinterpret_cast<const char*>(header.Name), strnlen(reinterpret_cast<const char*>(header.Name), 8));
        section.start = base + header.VirtualAddress;
        section.size = header.Misc.VirtualSize ? header.Misc.VirtualSize : header.SizeOfRawData;
        section.characteristics = header.Characteristics;
        out.sections.push_back(section);
    }
    return true;
}

// ---- patterns -----------------------------------------------------------------

Pattern ParsePattern(std::string_view text) {
    Pattern pattern;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ')
            ++i;
        if (i >= text.size())
            break;
        size_t j = i;
        while (j < text.size() && text[j] != ' ')
            ++j;
        const std::string_view token = text.substr(i, j - i);
        i = j;

        if (token == "?" || token == "??") {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(0);
            continue;
        }
        if (token.size() != 2)
            return {};
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = hex(token[0]);
        const int lo = hex(token[1]);
        if (hi < 0 || lo < 0)
            return {};
        pattern.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
        pattern.mask.push_back(1);
    }

    // Longest run of exact bytes: searched with Boyer-Moore-Horspool, then the full
    // pattern is verified around each hit.
    size_t runStart = 0;
    for (size_t k = 0; k <= pattern.bytes.size(); ++k) {
        if (k == pattern.bytes.size() || !pattern.mask[k]) {
            const size_t length = k - runStart;
            if (length > pattern.anchorLength) {
                pattern.anchorLength = length;
                pattern.anchorOffset = runStart;
            }
            runStart = k + 1;
        }
    }
    return pattern;
}

bool Matches(const uint8_t* data, const Pattern& pattern) {
    for (size_t k = 0; k < pattern.bytes.size(); ++k) {
        if (pattern.mask[k] && data[k] != pattern.bytes[k])
            return false;
    }
    return true;
}

std::vector<uintptr_t> Scan(uintptr_t start, size_t size, const Pattern& pattern, size_t maxResults) {
    std::vector<uintptr_t> hits;
    if (!pattern.Valid() || size < pattern.bytes.size())
        return hits;

    const auto* begin = reinterpret_cast<const uint8_t*>(start);
    const auto* end = begin + size;
    const auto* anchor = pattern.bytes.data() + pattern.anchorOffset;
    const std::boyer_moore_horspool_searcher searcher(anchor, anchor + pattern.anchorLength);

    const uint8_t* cursor = begin + pattern.anchorOffset;
    const uint8_t* searchEnd = end - (pattern.bytes.size() - pattern.anchorOffset - pattern.anchorLength);
    while (cursor < searchEnd) {
        const auto found = std::search(cursor, searchEnd, searcher);
        if (found == searchEnd)
            break;
        const uint8_t* candidate = found - pattern.anchorOffset;
        if (Matches(candidate, pattern)) {
            hits.push_back(reinterpret_cast<uintptr_t>(candidate));
            if (hits.size() >= maxResults)
                break;
        }
        cursor = found + 1;
    }
    return hits;
}

std::vector<uintptr_t> ScanModule(const ModuleInfo& module, const Pattern& pattern, size_t maxResults) {
    std::vector<uintptr_t> hits;
    for (const auto& section : module.sections) {
        if (!section.Executable() || section.size == 0)
            continue;

        // The scan dereferences raw memory, so only committed, readable spans are
        // searched. Adjacent readable regions are merged so a pattern can straddle
        // a protection boundary.
        uintptr_t cursor = section.start;
        uintptr_t spanStart = 0;
        const uintptr_t sectionEnd = section.start + section.size;
        auto flush = [&](uintptr_t spanEnd) {
            if (spanStart != 0 && spanEnd > spanStart && hits.size() < maxResults) {
                auto found = Scan(spanStart, spanEnd - spanStart, pattern, maxResults - hits.size());
                hits.insert(hits.end(), found.begin(), found.end());
            }
            spanStart = 0;
        };
        while (cursor < sectionEnd) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &info, sizeof(info)) != sizeof(info))
                break;
            const uintptr_t regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
            const uintptr_t regionEnd = std::min<uintptr_t>(regionStart + info.RegionSize, sectionEnd);
            if (regionEnd <= cursor)
                break;
            const bool readable = info.State == MEM_COMMIT && (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
                                  (info.Protect & kReadableProtect) != 0;
            if (readable) {
                if (spanStart == 0)
                    spanStart = cursor;
            } else {
                flush(cursor);
            }
            cursor = regionEnd;
        }
        flush(cursor);
        if (hits.size() >= maxResults)
            break;
    }
    return hits;
}

}  // namespace dps::mem
