#include "render/d3d11_hook.h"

#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <vector>

#include "common.h"
#include "hooks/hook_manager.h"
#include "log.h"
#include "meter/meter.h"
#include "render/input.h"
#include "render/overlay.h"
#include "settings.h"

namespace dps::render {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_present = nullptr;
ResizeBuffersFn g_resizeBuffers = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<long long> g_presents{0};
void (*g_frameCallback)() = nullptr;
void (*g_afterDraw)(IDXGISwapChain*, ID3D11DeviceContext*) = nullptr;

struct State {
    IDXGISwapChain* swapChain = nullptr;  // the game's; not AddRef'd
    std::vector<IDXGISwapChain*> ignored;  // swap chains of other windows
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    HWND window = nullptr;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool imgui = false;
    bool disabled = false;  // permanent failure: stop trying
    int rtvFailures = 0;
    std::string iniPath;
    std::string how;
    std::string info = "Present not hooked yet";
};

State g;
Lock g_lock;

std::atomic<HWND> g_targetWindow{nullptr};
wchar_t g_targetClass[128] = {};

bool IsGameWindow(HWND window) {
    const HWND target = g_targetWindow.load();
    if (!target || !window)
        return false;
    if (window == target || GetAncestor(window, GA_ROOT) == target)
        return true;
    // The game recreated its window (display mode change): same class, same process.
    if (IsWindow(target) || !g_targetClass[0])
        return false;
    wchar_t name[128] = {};
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    return pid == GetCurrentProcessId() && GetClassNameW(window, name, static_cast<int>(std::size(name))) > 0 &&
           wcscmp(name, g_targetClass) == 0;
}

DXGI_FORMAT ViewFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default: return format;
    }
}

void ReleaseRenderTarget() {
    if (g.rtv) {
        g.rtv->Release();
        g.rtv = nullptr;
    }
}

bool CreateRenderTarget(IDXGISwapChain* swapChain) {
    ReleaseRenderTarget();
    ID3D11Texture2D* back = nullptr;
    HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back));
    if (FAILED(hr) || !back) {
        if (g.rtvFailures++ < 5)
            LOG_WARN("GetBuffer(0) failed 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = ViewFormat(desc.Format);
    view.ViewDimension = desc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    hr = g.device->CreateRenderTargetView(back, &view, &g.rtv);
    back->Release();
    if (FAILED(hr) || !g.rtv) {
        g.rtv = nullptr;
        if (g.rtvFailures++ < 5)
            LOG_WARN("CreateRenderTargetView(format %d) failed 0x%08lX", static_cast<int>(desc.Format), static_cast<unsigned long>(hr));
        return false;
    }
    if (g.width != desc.Width || g.height != desc.Height || g.format != desc.Format)
        LOG_INFO("back buffer %ux%u format %d samples %u", desc.Width, desc.Height, static_cast<int>(desc.Format), desc.SampleDesc.Count);
    g.width = desc.Width;
    g.height = desc.Height;
    g.format = desc.Format;
    g.rtvFailures = 0;
    return true;
}

bool TryAddFont(ImGuiIO& io, const std::string& path, ImFontConfig& config, ImFont** font) {
    if (path.empty() || !FileExists(Utf8ToWide(path)))
        return false;
    ImFont* added = io.Fonts->AddFontFromFileTTF(path.c_str(), 0.0f, &config);
    if (!added) {
        LOG_WARN("font %s could not be loaded", path.c_str());
        return false;
    }
    if (font)
        *font = added;
    LOG_INFO("font %s%s", path.c_str(), config.MergeMode ? " (merged)" : "");
    return true;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const Settings s = settings::Get();

    // Latin first; CJK/Hangul fonts are merged in so Japanese, Chinese and Korean
    // hunter names render. ImGui 1.92 rasterizes glyphs on demand, so a large CJK
    // font costs nothing until a name actually uses it.
    const std::vector<std::string> mains = {
        s.fontPath,
        "C:\\windows\\Fonts\\segoeui.ttf",
        "C:\\windows\\Fonts\\tahoma.ttf",
        "C:\\windows\\Fonts\\arial.ttf",
        "C:\\windows\\Fonts\\verdana.ttf",
        "Z:\\System\\Library\\Fonts\\Supplemental\\Arial.ttf",
    };
    const std::vector<std::string> cjk = {
        s.cjkFontPath,
        "C:\\windows\\Fonts\\SourceHanSans-Regular.otf",
        "C:\\windows\\Fonts\\meiryo.ttc",
        "C:\\windows\\Fonts\\msgothic.ttc",
        "C:\\windows\\Fonts\\YuGothR.ttc",
        "C:\\windows\\Fonts\\msyh.ttc",
        "Z:\\System\\Library\\Fonts\\ヒラギノ角ゴシック W3.ttc",
        "Z:\\System\\Library\\Fonts\\Hiragino Sans GB.ttc",
        "C:\\windows\\Fonts\\simsun.ttc",
    };
    const std::vector<std::string> hangul = {
        "C:\\windows\\Fonts\\SourceHanSansK-Regular.otf",
        "C:\\windows\\Fonts\\malgun.ttf",
        "Z:\\System\\Library\\Fonts\\AppleSDGothicNeo.ttc",
    };

    ImFontConfig config;
    ImFont* main = nullptr;
    for (const auto& path : mains) {
        if (TryAddFont(io, path, config, &main))
            break;
    }
    if (!main) {
        io.Fonts->AddFontDefault();
        LOG_INFO("font: ImGui default (no system font found)");
    }
    config.MergeMode = true;
    for (const auto& path : cjk) {
        if (TryAddFont(io, path, config, nullptr))
            break;
    }
    for (const auto& path : hangul) {
        if (TryAddFont(io, path, config, nullptr))
            break;
    }
}

bool InitImGui(IDXGISwapChain* swapChain, HWND window) {
    ID3D11Device* device = nullptr;
    const HRESULT hr = swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
    if (FAILED(hr) || !device) {
        g.disabled = true;
        g.info = "swap chain has no D3D11 device: DirectX 12 mode is not supported, switch the game to DirectX 11";
        LOG_ERROR("%s (0x%08lX)", g.info.c_str(), static_cast<unsigned long>(hr));
        return false;
    }
    g.device = device;
    g.device->GetImmediateContext(&g.context);
    g.window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    g.iniPath = WideToUtf8(JoinPath(DataDirectory(), L"imgui.ini"));
    io.IniFilename = g.iniPath.c_str();
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    overlay::ApplyStyle();
    LoadFonts();

    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX11_Init(g.device, g.context)) {
        g.disabled = true;
        g.info = "ImGui backend init failed";
        LOG_ERROR("%s", g.info.c_str());
        return false;
    }
    input::Attach(window);
    g.imgui = true;
    LOG_INFO("overlay initialized on window 0x%p (device 0x%p)", static_cast<void*>(window), static_cast<void*>(g.device));
    return true;
}

/// Accepts the game's swap chain (first one presenting to the game window).
bool AdoptSwapChain(IDXGISwapChain* swapChain) {
    if (std::find(g.ignored.begin(), g.ignored.end(), swapChain) != g.ignored.end())
        return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow || !IsGameWindow(desc.OutputWindow)) {
        if (g.ignored.size() < 16)
            g.ignored.push_back(swapChain);
        return false;
    }

    if (!g.imgui) {
        if (!InitImGui(swapChain, desc.OutputWindow))
            return false;
    } else {
        // The game recreated its swap chain (display mode change).
        ID3D11Device* device = nullptr;
        swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device));
        if (device != g.device) {
            LOG_WARN("swap chain moved to another device; re-creating overlay resources");
            ReleaseRenderTarget();
            ImGui_ImplDX11_Shutdown();
            if (g.context)
                g.context->Release();
            if (g.device)
                g.device->Release();
            g.device = device;
            g.context = nullptr;
            if (g.device) {
                g.device->GetImmediateContext(&g.context);
                ImGui_ImplDX11_Init(g.device, g.context);
            }
        } else if (device) {
            device->Release();
        }
        if (desc.OutputWindow != g.window) {
            g.window = desc.OutputWindow;
            input::Attach(g.window);
        }
        ReleaseRenderTarget();
    }
    g.swapChain = swapChain;
    g.info = StrFormat("drawing on swap chain 0x%p (%s), %ux%u", static_cast<void*>(swapChain), g.how.c_str(),
                       desc.BufferDesc.Width, desc.BufferDesc.Height);
    LOG_INFO("%s", g.info.c_str());
    return true;
}

float FontSize(const ImGuiIO& io) {
    const float configured = settings::Get().fontSize;
    if (configured > 0.0f)
        return configured;
    // ~16 px at 1080p, scaled with the window height.
    return std::clamp(std::round(io.DisplaySize.y / 1080.0f * 16.0f), 13.0f, 30.0f);
}

void RenderFrame(IDXGISwapChain* swapChain) {
    if (!g.rtv && !CreateRenderTarget(swapChain))
        return;

    input::PollHotkeys();
    const bool menu = input::MenuOpen();
    // Nothing to draw (hub, title screen, overlay hidden): skip the ImGui frame.
    if (!menu && (!settings::Get().overlayVisible || !meter::Instance().OverlayShown()))
        return;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = menu;  // the game hides the OS cursor while hunting
    if (menu)
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    input::BeginFrame();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) {
        io.DisplaySize = ImVec2(static_cast<float>(g.width), static_cast<float>(g.height));
    } else if (g.width > 0 && g.height > 0) {
        // Back buffer and client area can differ (Retina / high-resolution mode).
        io.DisplayFramebufferScale =
            ImVec2(static_cast<float>(g.width) / io.DisplaySize.x, static_cast<float>(g.height) / io.DisplaySize.y);
    }
    if (!menu)
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);

    ImGui::NewFrame();
    input::EndFrame();
    const float fontSize = FontSize(io);
    ImGui::PushFont(nullptr, fontSize);
    overlay::Draw(menu, fontSize);
    ImGui::PopFont();
    ImGui::Render();

    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    g.context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    g.context->OMSetRenderTargets(1, &g.rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g.context->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv)
        oldRtv->Release();
    if (oldDsv)
        oldDsv->Release();
    if (g_afterDraw)
        g_afterDraw(swapChain, g.context);
}

void OnPresent(IDXGISwapChain* swapChain) {
    LockGuard guard(g_lock);
    if (g.disabled)
        return;
    if (swapChain != g.swapChain && !AdoptSwapChain(swapChain))
        return;
    if (g_frameCallback)
        g_frameCallback();
    RenderFrame(swapChain);
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    g_presents.fetch_add(1, std::memory_order_relaxed);
    if (!(flags & DXGI_PRESENT_TEST)) {
        try {
            OnPresent(swapChain);
        } catch (const std::exception& error) {
            LOG_ERROR("overlay frame failed: %s", error.what());
        } catch (...) {
            LOG_ERROR("overlay frame failed");
        }
    }
    return g_present(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                              UINT flags) {
    {
        // The game cannot resize while we hold a view of its back buffer.
        LockGuard guard(g_lock);
        if (swapChain == g.swapChain)
            ReleaseRenderTarget();
    }
    return g_resizeBuffers(swapChain, count, width, height, format, flags);
}

}  // namespace

bool InstallForSwapChain(IDXGISwapChain* swapChain, const char* how) {
    if (g_installed.load())
        return true;
    if (!swapChain)
        return false;
    void** vtable = *reinterpret_cast<void***>(swapChain);
    void* present = vtable[8];
    void* resize = vtable[13];

    char module[MAX_PATH] = "?";
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(present), &owner))
        GetModuleFileNameA(owner, module, MAX_PATH);
    LOG_INFO("swap chain 0x%p via %s: Present 0x%p ResizeBuffers 0x%p in %s", static_cast<void*>(swapChain), how, present,
             resize, module);

    {
        LockGuard guard(g_lock);
        g.how = how;
    }
    if (!hooks::Install("present", present, reinterpret_cast<void*>(&PresentDetour), reinterpret_cast<void**>(&g_present)))
        return false;
    hooks::Install("resize-buffers", resize, reinterpret_cast<void*>(&ResizeBuffersDetour), reinterpret_cast<void**>(&g_resizeBuffers));
    if (!g_resizeBuffers) {
        // Without the resize hook a display-mode change would fail while we hold the back buffer.
        LOG_WARN("ResizeBuffers not hooked: the overlay releases its back buffer view every frame");
    }
    g_installed.store(true);
    LockGuard guard(g_lock);
    g.info = StrFormat("Present hooked (%s), waiting for the game's first frame", how);
    return true;
}

namespace {

struct ProbeStart {
    HANDLE done = nullptr;
    bool ok = false;
};

/// Creates the probe window, device and swap chain, hooks Present through it, then
/// keeps all of it alive and answers the window's messages for the rest of the
/// session. Releasing the probe device, or leaving its window without a message
/// loop, could block the game's own thread (a 0.1.0 build froze at the startup load).
DWORD WINAPI ProbeThread(LPVOID param) {
    auto* start = static_cast<ProbeStart*>(param);
    auto finish = [&](bool ok) {
        start->ok = ok;
        SetEvent(start->done);  // start is the caller's from here on
    };

    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11)
        d3d11 = LoadLibraryW(L"d3d11.dll");
    auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
                              GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"))
                        : nullptr;
    if (!create) {
        LOG_ERROR("probe: D3D11CreateDeviceAndSwapChain not available");
        finish(false);
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MhwDpsMeterProbe";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"MhwDpsMeterProbe", WS_OVERLAPPED, 0,
                                  0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        LOG_ERROR("probe: window creation failed (%lu)", GetLastError());
        finish(false);
        return 0;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL level{};
    LOG_INFO("probe: creating device");
    const HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, static_cast<UINT>(std::size(levels)),
                              D3D11_SDK_VERSION, &desc, &swapChain, &device, &level, &context);
    bool ok = false;
    if (SUCCEEDED(hr) && swapChain)
        ok = InstallForSwapChain(swapChain, "probe device");
    else
        LOG_ERROR("probe: D3D11CreateDeviceAndSwapChain failed 0x%08lX", static_cast<unsigned long>(hr));
    LOG_INFO("probe: %s; keeping the probe alive", ok ? "Present hooked" : "failed");
    finish(ok);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace

bool InstallViaDummyDevice() {
    if (g_installed.load())
        return true;
    auto* start = new ProbeStart;
    start->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = start->done ? CreateThread(nullptr, 0, ProbeThread, start, 0, nullptr) : nullptr;
    if (!thread) {
        LOG_ERROR("probe: could not start its thread (%lu)", GetLastError());
        if (start->done)
            CloseHandle(start->done);
        delete start;
        return false;
    }
    CloseHandle(thread);
    if (WaitForSingleObject(start->done, 20000) != WAIT_OBJECT_0) {
        LOG_WARN("probe: no result after 20 s; giving up on the overlay for this session");
        return false;  // the probe thread may still use start: leave it allocated
    }
    const bool ok = start->ok;
    CloseHandle(start->done);
    delete start;
    return ok;
}

void SetTargetWindow(HWND window) {
    if (window)
        GetClassNameW(window, g_targetClass, static_cast<int>(std::size(g_targetClass)));
    g_targetWindow.store(window);
}

bool Installed() { return g_installed.load(); }

long long PresentCount() { return g_presents.load(std::memory_order_relaxed); }

std::string Describe() {
    LockGuard guard(g_lock);
    return StrFormat("%s; %lld presents", g.info.c_str(), g_presents.load(std::memory_order_relaxed));
}

void SetFrameCallback(void (*callback)()) { g_frameCallback = callback; }

void SetAfterDrawCallback(void (*callback)(IDXGISwapChain*, ID3D11DeviceContext*)) { g_afterDraw = callback; }

}  // namespace dps::render
