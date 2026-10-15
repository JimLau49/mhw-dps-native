#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dps::hooks {

bool Init();
/// Disables and removes every hook (plugin unload).
void Shutdown();

/// Creates and enables an inline hook; *original receives the trampoline.
bool Install(const char* name, void* target, void* detour, void** original);

struct HookInfo {
    std::string name;
    uintptr_t target = 0;
    bool enabled = false;
    std::string status;
};

std::vector<HookInfo> List();
/// "deal-damage ok @0x141CC5F80, present ok @0x..." for diagnostics.
std::string Summary();

}  // namespace dps::hooks
