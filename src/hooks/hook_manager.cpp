#include "hooks/hook_manager.h"

#include <MinHook.h>

#include "common.h"
#include "log.h"

namespace dps::hooks {
namespace {

Lock g_lock;
bool g_initialized = false;
std::vector<HookInfo> g_hooks;

}  // namespace

bool Init() {
    LockGuard guard(g_lock);
    if (g_initialized)
        return true;
    const MH_STATUS status = MH_Initialize();
    g_initialized = status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
    if (!g_initialized)
        LOG_ERROR("MinHook init failed: %s", MH_StatusToString(status));
    return g_initialized;
}

void Shutdown() {
    LockGuard guard(g_lock);
    if (!g_initialized)
        return;
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_initialized = false;
    for (auto& hook : g_hooks)
        hook.enabled = false;
}

bool Install(const char* name, void* target, void* detour, void** original) {
    LockGuard guard(g_lock);
    HookInfo info;
    info.name = name;
    info.target = reinterpret_cast<uintptr_t>(target);

    if (!g_initialized) {
        info.status = "MinHook not initialized";
    } else if (!target) {
        info.status = "no target";
    } else {
        MH_STATUS status = MH_CreateHook(target, detour, original);
        if (status == MH_OK)
            status = MH_EnableHook(target);
        info.enabled = status == MH_OK;
        info.status = info.enabled ? "ok" : MH_StatusToString(status);
        if (!info.enabled)
            MH_RemoveHook(target);
    }

    if (info.enabled)
        LOG_INFO("hook %s enabled at 0x%llX", name, static_cast<unsigned long long>(info.target));
    else
        LOG_WARN("hook %s not installed at 0x%llX: %s", name, static_cast<unsigned long long>(info.target), info.status.c_str());
    const bool enabled = info.enabled;
    g_hooks.push_back(std::move(info));
    return enabled;
}

std::vector<HookInfo> List() {
    LockGuard guard(g_lock);
    return g_hooks;
}

std::string Summary() {
    LockGuard guard(g_lock);
    std::string out;
    for (const auto& hook : g_hooks) {
        if (!out.empty())
            out += ", ";
        out += StrFormat("%s %s", hook.name.c_str(), hook.enabled ? "ok" : hook.status.c_str());
    }
    return out.empty() ? "none" : out;
}

}  // namespace dps::hooks
