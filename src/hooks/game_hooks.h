#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "address_map.h"
#include "memory.h"

namespace dps::hooks {

// ---- singletons (cSystem constructor) -----------------------------------------

/// Hooks cSystem's constructor so every game singleton is recorded as it is created.
/// Must run while the plugin loads (before the game's WinMain builds its systems).
bool InstallSingletonCapture(const mem::ModuleInfo& exe);
size_t SingletonCount();
/// Singleton whose DTI class name matches (e.g. "sMhRender"), or 0.
uintptr_t FindSingleton(const char* name);
/// (name, instance) for every recorded singleton with a resolvable DTI.
std::vector<std::pair<std::string, uintptr_t>> ListSingletons();
/// Class name from the object's MtDTI, decoded from its getDTI() thunk; "" if unknown.
std::string DtiName(uintptr_t object);

// ---- renderer -----------------------------------------------------------------

/// Mid-function hook on the game's renderer setup (SharpPluginLoader's
/// "sMhRender:Assignment"): records the sMhRender instance as it is assigned, so the
/// game's own swap chain can be found without creating a D3D device of our own.
/// Must run while the plugin loads (before WinMain sets up rendering).
bool InstallRenderCapture(const mem::ModuleInfo& exe);
/// Same hook on an explicit "lea rax, [rip+disp32]" site (also used by the self-test).
bool InstallRenderCaptureAt(uintptr_t site);
/// sMhRender captured by that hook, or 0.
uintptr_t CapturedRender();

// ---- damage -------------------------------------------------------------------

/// FUN_DEAL_DAMAGE (local hits) plus, when the map has a verified entry,
/// FUN_SHOW_DAMAGE_CONTEXT (monster part of each hit).
bool InstallDamageHooks(const mem::ModuleInfo& exe, const AddressMap& map, bool partContext, std::string& status);
std::string PartHookStatus();

// ---- actions ------------------------------------------------------------------

/// ActionController::DoAction: tags each local hit with the move being performed.
bool InstallActionHook(const mem::ModuleInfo& exe);
/// Action controller of the local hunter (entity + 0x61C8); 0 disables tagging.
void SetLocalActionController(uintptr_t controller);

// ---- game thread --------------------------------------------------------------

/// sMhMain update (once per game frame). callback runs on the game thread right
/// after the game's own update.
bool InstallMainUpdateHook(const mem::ModuleInfo& exe, void (*callback)());
/// True when the main update hook fired within the last second.
bool MainUpdateAlive();
bool OnGameThread();

// ---- quest names --------------------------------------------------------------

bool ResolveQuestNameFunction(const mem::ModuleInfo& exe);
/// Localized quest name from the game's text tables. Game thread only; "" when unavailable.
std::string QuestName(uintptr_t questInstance, int questId);

}  // namespace dps::hooks
