#pragma once

#include <dxgi.h>

#include <string>

struct ID3D11DeviceContext;

namespace dps::render {

/// The game window (found by the worker thread). Only swap chains presenting to it are
/// drawn on. Compared by handle: GetWindowText from inside Present would send a
/// message to the game's main thread, which may be waiting on the render thread.
void SetTargetWindow(HWND window);

/// Hooks IDXGISwapChain::Present/ResizeBuffers through the vtable of swapChain
/// (inline hooks on the implementation, so every swap chain of that class is covered).
bool InstallForSwapChain(IDXGISwapChain* swapChain, const char* how);
/// Fallback when the game's swap chain cannot be located: create a throwaway
/// device + swap chain on a hidden window just to find Present.
bool InstallViaDummyDevice();
bool Installed();
/// Number of Present calls the overlay has seen (0 = hook not reached yet).
long long PresentCount();
std::string Describe();

/// Per-frame work on the render thread before the overlay is drawn (e.g. polling
/// the meter when the game-thread hook is unavailable).
void SetFrameCallback(void (*callback)());

/// Test hook: runs after the overlay drew into the back buffer, before the real Present.
void SetAfterDrawCallback(void (*callback)(IDXGISwapChain* swapChain, ID3D11DeviceContext* context));

}  // namespace dps::render
