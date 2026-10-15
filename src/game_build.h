#pragma once

#include <string>

#include "memory.h"

namespace dps::build {

/// The exe's version resource is 1.0.0.0, so builds are identified by the number
/// baked into the window title ("MONSTER HUNTER: WORLD(421810)"), which also sits
/// in .rdata as a standalone "421810\0" string.
int FromRdata(const mem::ModuleInfo& exe);
/// 0 until the game window exists.
int FromWindowTitle(HWND* window = nullptr);
/// Stracker's Loader exports the version it detected (loader::GameVersion).
int FromStrackerLoader();

/// Top-level window of this process titled "MONSTER HUNTER: WORLD(...)".
HWND FindGameWindow();

}  // namespace dps::build
