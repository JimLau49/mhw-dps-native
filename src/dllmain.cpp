// MHW DPS Meter (native): Stracker's Loader plugin.
//
// Stracker's Loader LoadLibrary()s every DLL in nativePC/plugins from its
// dinput8.dll proxy, while Steam's DRM stub resolves the game's imports: the exe
// is already decrypted, but WinMain has not run yet. DllMain therefore only hooks
// cSystem's constructor (to catch the game's singletons as they are built) and
// starts a worker thread that does everything else once the game window exists.

#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cctype>

#include "address_map.h"
#include "common.h"
#include "game_build.h"
#include "hooks/game_hooks.h"
#include "hooks/hook_manager.h"
#include "log.h"
#include "memory.h"
#include "meter/fight_log_store.h"
#include "meter/meter.h"
#include "render/d3d11_hook.h"
#include "settings.h"

namespace dps {
namespace {

mem::ModuleInfo g_exe;
bool g_singletonHook = false;
bool g_renderCapture = false;

void OnGameUpdate() { meter::Instance().Update(); }

void OnRenderFrame() {
    // Game-thread hook missing or stalled (e.g. during a load): poll from the render thread.
    if (!hooks::MainUpdateAlive())
        meter::Instance().Update();
}

std::string ModuleNameOf(uintptr_t address) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !GetModuleFileNameA(module, path, MAX_PATH))
        return {};
    std::string name = path;
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
}

/// The game's IDXGISwapChain from sMhRender (SharpPluginLoader D3DModule: render+0x78 is
/// the cD3DRender, +0x1488 its DX11 swap chain). Validated without calling into it
/// until its vtable is known to live in a DXGI/D3D module.
IDXGISwapChain* FindGameSwapChain(std::string& why) {
    uintptr_t render = hooks::CapturedRender();
    if (!render)
        render = hooks::FindSingleton("sMhRender");
    if (!render) {
        why = g_renderCapture ? "renderer not set up yet" : "render capture hook unavailable";
        return nullptr;
    }
    uintptr_t renderer = 0, swapChain = 0, vtable = 0, present = 0;
    if (!mem::ReadPtr(render + 0x78, renderer) || !mem::ReadPtr(renderer + 0x1488, swapChain)) {
        why = "renderer/swap chain not created yet";
        return nullptr;
    }
    if (!mem::ReadPtr(swapChain, vtable) || !mem::ReadPtr(vtable + 8 * 8, present)) {
        why = "swap chain vtable unreadable";
        return nullptr;
    }
    const std::string vtableModule = ModuleNameOf(vtable);
    const std::string presentModule = ModuleNameOf(present);
    if (vtableModule.empty() || vtableModule != presentModule ||
        (vtableModule.find("dxgi") == std::string::npos && vtableModule.find("d3d") == std::string::npos)) {
        why = StrFormat("0x%llX is not a DXGI swap chain (vtable in '%s', Present in '%s')",
                        static_cast<unsigned long long>(swapChain), vtableModule.c_str(), presentModule.c_str());
        return nullptr;
    }

    auto* candidate = reinterpret_cast<IUnknown*>(swapChain);
    IDXGISwapChain* typed = nullptr;
    if (FAILED(candidate->QueryInterface(__uuidof(IDXGISwapChain), reinterpret_cast<void**>(&typed))) || !typed) {
        why = "QueryInterface(IDXGISwapChain) failed";
        return nullptr;
    }
    typed->Release();  // the game owns it; we only need the pointer
    why = StrFormat("sMhRender 0x%llX -> swap chain 0x%llX (%s)", static_cast<unsigned long long>(render),
                    static_cast<unsigned long long>(swapChain), vtableModule.c_str());
    return typed;
}

void LogStaticSingletons(const AddressMap& map) {
    // Cross-check: the static addresses from the map should hold these singletons.
    struct Probe {
        const char* key;
        const char* expected;
    };
    const Probe probes[] = {{"QUEST_DATA_ADDRESS", "sQuest"}, {"EQUIPMENT_ADDRESS", "sPlayer"}, {"ZONE_OFFSET", "?"}};
    for (const auto& probe : probes) {
        uintptr_t rva = 0, object = 0;
        if (!map.Address(probe.key, rva) || !mem::ReadPtr(g_exe.base + rva, object))
            continue;
        LOG_INFO("map %s -> 0x%llX %s (expected %s)", probe.key, static_cast<unsigned long long>(object),
                 hooks::DtiName(object).c_str(), probe.expected);
    }
}

void Worker(void*) {
    const Settings settings = settings::Get();
    LOG_INFO("worker started");

    // ---- game build and address map ----
    int build = build::FromRdata(g_exe);
    std::string source = "rdata";
    const int loaderBuild = build::FromStrackerLoader();
    if (build == 0 && loaderBuild != 0) {
        build = loaderBuild;
        source = "Stracker's Loader";
    }
    LOG_INFO("game build %d (%s; Stracker's Loader reports %d)", build, source.c_str(), loaderBuild);

    HWND window = nullptr;
    for (int i = 0; !window; ++i) {
        window = build::FindGameWindow();
        if (!window)
            Sleep(250);
        if (i == 240)
            LOG_INFO("still waiting for the game window...");
    }
    render::SetTargetWindow(window);
    const int titleBuild = build::FromWindowTitle();
    if (titleBuild != 0 && titleBuild != build) {
        LOG_WARN("window title says build %d (detected %d); using the title", titleBuild, build);
        build = titleBuild;
        source = "window title";
    }
    const bool confirmed = titleBuild != 0 && titleBuild == build;

    AddressMap map = AddressMap::Load(build);
    bool exactMap = map.embedded || !map.Empty();
    if (!exactMap) {
        const auto builds = AddressMap::EmbeddedBuilds();
        map = AddressMap::Load(builds.front());
        LOG_WARN("no address map for build %d; trying %s (game hooks disabled)", build, map.source.c_str());
    }
    LOG_INFO("address map: %s", map.source.c_str());
    LogStaticSingletons(map);

    meter::Instance().Init(g_exe, map, build, confirmed, source);
    meter::Logs().Init(DataDirectory());

    // ---- game hooks ----
    if (exactMap) {
        std::string damageStatus;
        hooks::InstallDamageHooks(g_exe, map, settings.hookPartContext, damageStatus);
        if (settings.hookActions)
            hooks::InstallActionHook(g_exe);
        if (settings.hookMainUpdate)
            hooks::InstallMainUpdateHook(g_exe, &OnGameUpdate);
        if (settings.questNames)
            hooks::ResolveQuestNameFunction(g_exe);
    }
    meter::Instance().SetHookInfo(hooks::Summary());

    // ---- overlay ----
    render::SetFrameCallback(&OnRenderFrame);
    const int64_t searchStart = QpcNow();
    std::string why;
    while (!render::Installed()) {
        if (IDXGISwapChain* swapChain = FindGameSwapChain(why)) {
            LOG_INFO("%s", why.c_str());
            render::InstallForSwapChain(swapChain, "game swap chain");
            break;
        }
        // Last resort: find Present through a probe device. Only once the game has had
        // time to finish its startup load: creating and tearing down a second device
        // while the game is loading froze it (0.1.0 builds with a 3 s wait).
        if (SecondsSince(searchStart) > 20.0) {
            LOG_WARN("game swap chain not found (%s); using a probe device", why.c_str());
            std::string names;
            for (const auto& [name, instance] : hooks::ListSingletons()) {
                if (names.size() < 1500)
                    names += name + " ";
            }
            LOG_INFO("singletons seen: %s", names.empty() ? "(none)" : names.c_str());
            render::InstallViaDummyDevice();
            break;
        }
        Sleep(250);
    }
    meter::Instance().SetHookInfo(hooks::Summary());

    const auto singletons = hooks::ListSingletons();
    LOG_INFO("%zu singletons recorded, %zu named", hooks::SingletonCount(), singletons.size());

    bool warned = false;
    for (;;) {
        Sleep(1000);
        meter::Instance().SetRenderInfo(render::Describe());
        meter::Instance().SetHookInfo(hooks::Summary());
        if (!warned && render::Installed() && render::PresentCount() == 0 && SecondsSince(searchStart) > 60.0) {
            warned = true;
            LOG_WARN("Present is hooked but has not been called after 60 s; the overlay will not draw");
        }
    }
}

void Startup(HMODULE self) {
    EnsureDirectory(DataDirectory());
    log::Init(JoinPath(DataDirectory(), L"MhwDpsMeter.log"));
    settings::Load();
    const Settings s = settings::Get();
    if (s.logLevel == "debug")
        log::SetMinLevel(log::Level::Debug);

    wchar_t dll[MAX_PATH] = {};
    GetModuleFileNameW(self, dll, MAX_PATH);
    LOG_INFO("MHW DPS Meter native %s loading from %s", kBuildStamp, WideToUtf8(dll).c_str());
    LOG_INFO("game folder %s", WideToUtf8(GameDirectory()).c_str());

    mem::Init();
    if (!mem::QueryModule(GetModuleHandleW(nullptr), g_exe)) {
        LOG_ERROR("could not read the game module headers; plugin disabled");
        return;
    }
    LOG_INFO("exe base 0x%llX size 0x%zX timestamp 0x%08X", static_cast<unsigned long long>(g_exe.base), g_exe.size,
             g_exe.timestamp);
    if (!hooks::Init())
        return;

    // Singletons and the renderer are set up during WinMain; this is the only chance to see them.
    g_singletonHook = hooks::InstallSingletonCapture(g_exe);
    g_renderCapture = hooks::InstallRenderCapture(g_exe);

    if (!RunDetached(&Worker, nullptr))
        LOG_ERROR("could not start the worker thread");
}

}  // namespace
}  // namespace dps

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        try {
            dps::Startup(instance);
        } catch (...) {
            // never fail the game's startup because of the overlay
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // Explicit unload only; at process exit other threads are already gone.
        dps::hooks::Shutdown();
        dps::mem::Shutdown();
    }
    return TRUE;
}
