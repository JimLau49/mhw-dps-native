#pragma once

#include <windows.h>

namespace dps::render::input {

/// Subclasses the game window. Messages are queued and replayed into ImGui on the
/// render thread (ImGui is not thread-safe); while the F9 panel is open, mouse and
/// keyboard messages ImGui wants are kept from the game.
void Attach(HWND window);
HWND Window();

/// Render thread, before ImGui::NewFrame: feeds queued messages to ImGui.
void BeginFrame();
/// Render thread, after ImGui::NewFrame: publishes ImGui's capture flags.
void EndFrame();

/// Edge-triggered hotkeys from settings.json (only while the game window is focused).
void PollHotkeys();

bool MenuOpen();
void SetMenuOpen(bool open);

}  // namespace dps::render::input
