#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dps::mem {

constexpr uintptr_t kUserMin = 0x10000;
constexpr uintptr_t kUserMax = 0x00007FFFFFFEFFFFULL;

inline bool IsUserPtr(uintptr_t p) { return p >= kUserMin && p <= kUserMax; }

/// Installs the vectored exception handler that turns a fault inside the guarded
/// copy routine into a failed read. Must run before any Read*() call.
void Init();
void Shutdown();
/// Number of faults the guarded copy has absorbed (diagnostics).
uint64_t FaultCount();

/// Copies size bytes from address into out. Never faults: an invalid, freed,
/// guard or no-access address returns false instead of crashing the game.
bool ReadBytes(uintptr_t address, void* out, size_t size);

template <typename T>
bool Read(uintptr_t address, T& out) {
    return ReadBytes(address, &out, sizeof(T));
}

template <typename T>
T ReadOr(uintptr_t address, T fallback) {
    T value;
    return Read(address, value) ? value : fallback;
}

/// Reads a pointer and requires it to look like a user-space address.
bool ReadPtr(uintptr_t address, uintptr_t& out);

/// HunterPie-style chain: for each offset, address = *address + offset.
/// Returns 0 (and describes the failing hop) when a pointer is null or unreadable.
uintptr_t Follow(uintptr_t address, const std::vector<int>& offsets, std::string* error = nullptr);

/// NUL-terminated string of at most maxBytes (terminator excluded).
bool ReadCString(uintptr_t address, size_t maxBytes, std::string& out);

// ---- modules ------------------------------------------------------------------

struct Section {
    std::string name;
    uintptr_t start = 0;
    size_t size = 0;
    uint32_t characteristics = 0;
    bool Executable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
};

struct ModuleInfo {
    uintptr_t base = 0;
    size_t size = 0;
    uint32_t timestamp = 0;
    std::vector<Section> sections;

    bool Contains(uintptr_t p) const { return base != 0 && p >= base && p < base + size; }
    const Section* Find(std::string_view name) const;
};

bool QueryModule(HMODULE module, ModuleInfo& out);

// ---- patterns -----------------------------------------------------------------

/// IDA-style byte pattern: "48 8B ?? 05" ("?" and "??" are wildcards).
struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;  // 1 = must match
    size_t anchorOffset = 0;    // longest exact run, searched first
    size_t anchorLength = 0;
    bool Valid() const { return !bytes.empty() && anchorLength > 0; }
};

Pattern ParsePattern(std::string_view text);
bool Matches(const uint8_t* data, const Pattern& pattern);
std::vector<uintptr_t> Scan(uintptr_t start, size_t size, const Pattern& pattern, size_t maxResults);
/// Scans every executable section of a module.
std::vector<uintptr_t> ScanModule(const ModuleInfo& module, const Pattern& pattern, size_t maxResults = 2);

}  // namespace dps::mem
