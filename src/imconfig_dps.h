#pragma once

// ImGui build options for the in-game overlay (see third_party/imgui/imconfig.h).
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
// The game owns the IME context; never let ImGui reposition or reset it.
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS

// A failed ImGui assertion must never abort the game: log it and carry on.
void DpsImGuiAssertFailed(const char* expression, const char* file, int line);
#define IM_ASSERT(_EXPR)                                         \
    do {                                                         \
        if (!(_EXPR))                                            \
            DpsImGuiAssertFailed(#_EXPR, __FILE__, __LINE__);    \
    } while (0)
