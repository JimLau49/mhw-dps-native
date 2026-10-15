#include "render/input.h"

#include <imgui.h>
#include <imgui_impl_win32.h>

#include <atomic>
#include <vector>

#include "common.h"
#include "log.h"
#include "meter/meter.h"
#include "settings.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dps::render::input {
namespace {

struct QueuedMessage {
    HWND hwnd;
    UINT msg;
    WPARAM wParam;
    LPARAM lParam;
};

HWND g_window = nullptr;
WNDPROC g_original = nullptr;
std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_wantMouse{false};
std::atomic<bool> g_wantKeyboard{false};
Lock g_queueLock;
std::vector<QueuedMessage> g_queue;

bool IsMouseMessage(UINT msg) {
    return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE;
}

bool IsKeyboardMessage(UINT msg) {
    return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR ||
           msg == WM_SYSCHAR || msg == WM_UNICHAR;
}

bool IsForwarded(UINT msg) {
    return IsMouseMessage(msg) || IsKeyboardMessage(msg) || msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
           msg == WM_INPUTLANGCHANGE;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_menuOpen.load(std::memory_order_relaxed)) {
        if (IsForwarded(msg)) {
            LockGuard guard(g_queueLock);
            if (g_queue.size() < 1024)
                g_queue.push_back({hwnd, msg, wParam, lParam});
        }
        if ((g_wantMouse.load(std::memory_order_relaxed) && IsMouseMessage(msg) && msg != WM_MOUSEMOVE) ||
            (g_wantKeyboard.load(std::memory_order_relaxed) && IsKeyboardMessage(msg)))
            return 0;
    }
    return CallWindowProcW(g_original, hwnd, msg, wParam, lParam);
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool HotkeyDown(const Hotkey& key) {
    if (!key.Valid() || !Down(key.vk))
        return false;
    return Down(VK_CONTROL) == key.ctrl && Down(VK_SHIFT) == key.shift && Down(VK_MENU) == key.alt;
}

struct EdgeState {
    bool menu = false, overlay = false, reset = false, trial = false, diagnostics = false;
};

EdgeState g_edges;

bool Pressed(bool down, bool& previous) {
    const bool pressed = down && !previous;
    previous = down;
    return pressed;
}

}  // namespace

void Attach(HWND window) {
    if (!window || g_window == window)
        return;
    g_window = window;
    g_original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
    if (!g_original)
        LOG_WARN("could not subclass the game window (%lu); the F9 panel will not take mouse input", GetLastError());
}

HWND Window() { return g_window; }

void BeginFrame() {
    std::vector<QueuedMessage> messages;
    {
        LockGuard guard(g_queueLock);
        messages.swap(g_queue);
    }
    if (!g_menuOpen.load(std::memory_order_relaxed))
        return;
    for (const auto& m : messages)
        ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wParam, m.lParam);
}

void EndFrame() {
    const ImGuiIO& io = ImGui::GetIO();
    const bool open = g_menuOpen.load(std::memory_order_relaxed);
    g_wantMouse.store(open && io.WantCaptureMouse, std::memory_order_relaxed);
    g_wantKeyboard.store(open && io.WantCaptureKeyboard, std::memory_order_relaxed);
}

bool MenuOpen() { return g_menuOpen.load(std::memory_order_relaxed); }

void SetMenuOpen(bool open) {
    g_menuOpen.store(open, std::memory_order_relaxed);
    if (!open) {
        g_wantMouse.store(false, std::memory_order_relaxed);
        g_wantKeyboard.store(false, std::memory_order_relaxed);
        LockGuard guard(g_queueLock);
        g_queue.clear();
    }
}

void PollHotkeys() {
    const Settings s = settings::Get();
    // Hotkeys only while the game has focus (not while typing in another app).
    const HWND foreground = GetForegroundWindow();
    const bool focused = g_window && (foreground == g_window || GetAncestor(foreground, GA_ROOTOWNER) == g_window);

    const bool menu = focused && HotkeyDown(s.keyMenu);
    const bool overlay = focused && HotkeyDown(s.keyOverlay);
    const bool reset = focused && HotkeyDown(s.keyReset);
    const bool trial = focused && HotkeyDown(s.keyTrial);
    const bool diagnostics = focused && HotkeyDown(s.keyDiagnostics);

    if (Pressed(menu, g_edges.menu))
        SetMenuOpen(!MenuOpen());
    if (Pressed(overlay, g_edges.overlay))
        settings::Update([](Settings& value) { value.overlayVisible = !value.overlayVisible; });
    if (Pressed(reset, g_edges.reset))
        meter::Instance().Post(meter::Command::ResetTraining);
    if (Pressed(trial, g_edges.trial))
        meter::Instance().Post(meter::Command::ToggleTrial);
    if (Pressed(diagnostics, g_edges.diagnostics))
        meter::Instance().Post(meter::Command::DumpDiagnostics);
}

}  // namespace dps::render::input
