#include "hooks/game_hooks.h"

#include <array>
#include <atomic>
#include <cstring>

#include "common.h"
#include "game_data.h"
#include "hooks/hook_manager.h"
#include "log.h"
#include "meter/hit_tracker.h"

namespace dps::hooks {
namespace {

// Signatures from SharpPluginLoader's AddressRecords.json (pattern, offset to function start).
struct Signature {
    const char* name;
    const char* pattern;
    int offset;
};

constexpr Signature kSystemCtor = {"cSystem:Ctor", "48 83 C1 08 FF 15 ?? ?? ?? ?? 48 8B C3 C6 43 30 01 48 83 C4 20 5B C3", -19};
constexpr Signature kDoAction = {"ActionController:DoAction", "48 8D 41 07 48 C1 E0 04 46 3B 04 08", -10};
constexpr Signature kMainUpdate = {"Main:Update", "48 8B 8B 88 F8 0A 00 48 8B 01 FF 50 48", -25};
constexpr Signature kGetQuestName = {"Quest:GetQuestName", "41 56 48 83 EC 20 48 8D B1 A0 AE 00 00", -20};

/// Unique match of a signature in the exe's code, or 0.
uintptr_t FindSignature(const mem::ModuleInfo& exe, const Signature& signature) {
    const mem::Pattern pattern = mem::ParsePattern(signature.pattern);
    const int64_t started = QpcNow();
    const auto hits = mem::ScanModule(exe, pattern, 2);
    const double ms = SecondsSince(started) * 1000.0;
    if (hits.size() != 1) {
        LOG_WARN("signature %s: %zu matches (%.1f ms)", signature.name, hits.size(), ms);
        return 0;
    }
    const uintptr_t address = hits[0] + static_cast<intptr_t>(signature.offset);
    LOG_INFO("signature %s at 0x%llX (rva 0x%llX, %.1f ms)", signature.name, static_cast<unsigned long long>(address),
             static_cast<unsigned long long>(address - exe.base), ms);
    return address;
}

std::string HexBytes(uintptr_t address, size_t count) {
    std::vector<uint8_t> bytes(count);
    if (!mem::ReadBytes(address, bytes.data(), bytes.size()))
        return "unreadable";
    std::string out;
    for (uint8_t b : bytes)
        out += StrFormat("%02X ", b);
    if (!out.empty())
        out.pop_back();
    return out;
}

// Detours pass rax back unchanged (uint64_t returns) and forward every register
// argument, so a hook stays transparent even where the exact signature is unknown.

// ---- singletons -------------------------------------------------------------

using SystemCtorFn = uint64_t (*)(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4);
SystemCtorFn g_systemCtor = nullptr;
constexpr size_t kMaxSingletons = 2048;
std::array<std::atomic<uintptr_t>, kMaxSingletons> g_singletons{};
std::atomic<size_t> g_singletonCount{0};
mem::ModuleInfo g_exe;

uint64_t SystemCtorDetour(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4) {
    const uint64_t result = g_systemCtor(self, a2, a3, a4);
    const size_t index = g_singletonCount.fetch_add(1, std::memory_order_relaxed);
    if (index < kMaxSingletons)
        g_singletons[index].store(static_cast<uintptr_t>(self), std::memory_order_release);
    return result;
}

// ---- damage -----------------------------------------------------------------

// HunterPie: void DealDamage(Monster* target, int damage, Vector3* position, BOOL isTenderized,
//                            BOOL isCrit, int unk0, int unk1, char unk2, int attackId)
using DealDamageFn = uint64_t (*)(uint64_t target, int damage, uint64_t position, int tenderized, int crit, int unk0,
                                  int unk1, char unk2, int attackId);
DealDamageFn g_dealDamage = nullptr;

using ShowDamageContextFn = uint64_t (*)(uint64_t controller, uint64_t hitData, uint64_t damageData, uint64_t a4);
ShowDamageContextFn g_showDamageContext = nullptr;
std::string g_partStatus = "off";

/// Collision part of the hit being shown, scoped to the nested DealDamage call.
struct PartContext {
    bool active = false;
    uintptr_t target = 0;
    uintptr_t position = 0;
    int part = -1;
    const char* status = "none";
};

thread_local PartContext t_part;

/// Mirrors the part selector of the game's damage application (421810
/// 0x1402C66DE..0x1402C671E), including the alternate part after a break.
PartContext CapturePart(uintptr_t controller, uintptr_t hitData) {
    PartContext context;
    context.active = true;
    context.position = hitData + 0xE0;
    if (!mem::IsUserPtr(controller) || !mem::IsUserPtr(hitData) || !mem::ReadPtr(controller + 0x08, context.target)) {
        context.status = "unreadable target";
        return context;
    }

    constexpr int kMaxNormalParts = 16;
    constexpr uintptr_t kNormalPartStride = 0x1F8;
    uintptr_t collision = 0;
    int slot = -1;
    if (!mem::ReadPtr(hitData + 0x28, collision) || !mem::Read(collision + 0x60, slot) || slot < 0 || slot >= kMaxNormalParts) {
        context.status = "unreadable collision part";
        return context;
    }
    int state = 0;
    if (!mem::Read(controller + static_cast<uintptr_t>(slot) * kNormalPartStride + 0x208, state)) {
        context.status = "unreadable part state";
        return context;
    }
    if (state == 1 && (!mem::Read(collision + 0x80, slot) || slot < 0 || slot >= kMaxNormalParts)) {
        context.status = "unreadable alternate part";
        return context;
    }
    int monsterType = -1;
    if (!mem::Read(context.target + 0x12280, monsterType)) {
        context.status = "unreadable monster type";
        return context;
    }
    const auto part = data::PartFromNormalSlot(monsterType, slot);
    context.part = part ? *part : -1;
    context.status = part ? "collision slot" : "unmapped monster/slot";
    return context;
}

uint64_t ShowDamageContextDetour(uint64_t controller, uint64_t hitData, uint64_t damageData, uint64_t a4) {
    const PartContext previous = t_part;
    t_part = PartContext{};
    try {
        uintptr_t target = 0;
        if (mem::ReadPtr(controller + 0x08, target) && meter::Hits().WantsPart(target))
            t_part = CapturePart(controller, hitData);
    } catch (...) {
        t_part = PartContext{};
    }
    const uint64_t result = g_showDamageContext(controller, hitData, damageData, a4);
    t_part = previous;
    return result;
}

uint64_t DealDamageDetour(uint64_t target, int damage, uint64_t position, int tenderized, int crit, int unk0, int unk1,
                          char unk2, int attackId) {
    const uint64_t result = g_dealDamage(target, damage, position, tenderized, crit, unk0, unk1, unk2, attackId);
    try {
        int part = -1;
        const char* reason = "no hit context";
        if (t_part.active) {
            if (t_part.target == target && t_part.position == position) {
                part = t_part.part;
                reason = t_part.status;
            } else {
                reason = "hit context mismatch";
            }
        }
        meter::Hits().OnDealDamage(target, damage, tenderized != 0, crit != 0, attackId, part, reason);
    } catch (...) {
        // never let bookkeeping escape into the game's damage call
    }
    return result;
}

// ---- actions ----------------------------------------------------------------

using DoActionFn = uint64_t (*)(uint64_t controller, uint64_t info, uint64_t a3, uint64_t a4);
DoActionFn g_doAction = nullptr;
std::atomic<uintptr_t> g_localController{0};

uint64_t DoActionDetour(uint64_t controller, uint64_t info, uint64_t a3, uint64_t a4) {
    const uint64_t result = g_doAction(controller, info, a3, a4);
    const uintptr_t local = g_localController.load(std::memory_order_relaxed);
    if (local != 0 && controller == local) {
        int pair[2] = {0, 0};  // ActionInfo { int actionSet; int actionId; }
        if (mem::ReadBytes(info, pair, sizeof(pair)))
            meter::Hits().SetCurrentAction(pair[0], pair[1]);
    }
    return result;
}

// ---- main update --------------------------------------------------------------

using MainUpdateFn = uint64_t (*)(uint64_t main, uint64_t a2, uint64_t a3, uint64_t a4);
MainUpdateFn g_mainUpdate = nullptr;
void (*g_mainCallback)() = nullptr;
std::atomic<int64_t> g_mainLastQpc{0};
std::atomic<DWORD> g_gameThreadId{0};

uint64_t MainUpdateDetour(uint64_t main, uint64_t a2, uint64_t a3, uint64_t a4) {
    const uint64_t result = g_mainUpdate(main, a2, a3, a4);
    g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_mainLastQpc.store(QpcNow(), std::memory_order_relaxed);
    if (g_mainCallback) {
        try {
            g_mainCallback();
        } catch (...) {
        }
    }
    return result;
}

// ---- quest names --------------------------------------------------------------

using GetQuestNameFn = const char* (*)(uintptr_t quest, int questId, int unk);
GetQuestNameFn g_getQuestName = nullptr;

}  // namespace

// ---- singletons -----------------------------------------------------------------

bool InstallSingletonCapture(const mem::ModuleInfo& exe) {
    g_exe = exe;
    const uintptr_t target = FindSignature(exe, kSystemCtor);
    if (!target)
        return false;
    return Install("cSystem-ctor", reinterpret_cast<void*>(target), reinterpret_cast<void*>(&SystemCtorDetour),
                   reinterpret_cast<void**>(&g_systemCtor));
}

size_t SingletonCount() { return std::min(g_singletonCount.load(std::memory_order_relaxed), kMaxSingletons); }

std::string DtiName(uintptr_t object) {
    uintptr_t vtable = 0, getDti = 0;
    if (!mem::Read(object, vtable) || !g_exe.Contains(vtable) || !mem::Read(vtable + 4 * 8, getDti) || !g_exe.Contains(getDti))
        return {};

    // getDTI() is "lea rax, [rip+dti]; ret" (or "mov rax, [rip+ptr]; ret").
    uint8_t code[8];
    if (!mem::ReadBytes(getDti, code, sizeof(code)) || code[0] != 0x48 || code[7] != 0xC3)
        return {};
    int32_t displacement = 0;
    std::memcpy(&displacement, code + 3, sizeof(displacement));
    uintptr_t dti = getDti + 7 + static_cast<intptr_t>(displacement);
    if (code[1] == 0x8B && code[2] == 0x05) {
        if (!mem::ReadPtr(dti, dti))
            return {};
    } else if (!(code[1] == 0x8D && code[2] == 0x05)) {
        return {};
    }

    uintptr_t name = 0;
    std::string text;
    if (!mem::ReadPtr(dti + 8, name) || !mem::ReadCString(name, 96, text))
        return {};
    return text;
}

uintptr_t FindSingleton(const char* name) {
    const size_t count = SingletonCount();
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t object = g_singletons[i].load(std::memory_order_acquire);
        if (object && DtiName(object) == name)
            return object;
    }
    return 0;
}

std::vector<std::pair<std::string, uintptr_t>> ListSingletons() {
    std::vector<std::pair<std::string, uintptr_t>> out;
    const size_t count = SingletonCount();
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t object = g_singletons[i].load(std::memory_order_acquire);
        std::string name = object ? DtiName(object) : std::string();
        if (!name.empty())
            out.emplace_back(std::move(name), object);
    }
    return out;
}

// ---- damage -----------------------------------------------------------------------

bool InstallDamageHooks(const mem::ModuleInfo& exe, const AddressMap& map, bool partContext, std::string& status) {
    uintptr_t rva = 0;
    if (!map.Address("FUN_DEAL_DAMAGE", rva)) {
        status = "no FUN_DEAL_DAMAGE in the address map";
        meter::Hits().SetHookStatus(false, status);
        return false;
    }

    const uintptr_t dealDamage = exe.base + rva;
    // Functions are padded with int3/nop; anything else before the entry means the
    // address does not start a function in this exe.
    uint8_t before = 0;
    if (!mem::Read(dealDamage - 1, before) || (before != 0xCC && before != 0x90 && before != 0xC3)) {
        status = StrFormat("FUN_DEAL_DAMAGE 0x%llX does not look like a function start (prev byte %02X)",
                           static_cast<unsigned long long>(dealDamage), before);
        LOG_WARN("%s", status.c_str());
        meter::Hits().SetHookStatus(false, status);
        return false;
    }
    LOG_INFO("FUN_DEAL_DAMAGE 0x%llX: %s", static_cast<unsigned long long>(dealDamage), HexBytes(dealDamage, 16).c_str());

    if (!Install("deal-damage", reinterpret_cast<void*>(dealDamage), reinterpret_cast<void*>(&DealDamageDetour),
                 reinterpret_cast<void**>(&g_dealDamage))) {
        status = "deal-damage hook failed";
        meter::Hits().SetHookStatus(false, status);
        return false;
    }
    status = StrFormat("hook 0x%llX", static_cast<unsigned long long>(dealDamage));
    meter::Hits().SetHookStatus(true, status);

    g_partStatus = "no verified context hook for this map";
    if (partContext && map.Address("FUN_SHOW_DAMAGE_CONTEXT", rva)) {
        const uintptr_t show = exe.base + rva;
        // Fail closed if the function changed or another mod patched it.
        static const uint8_t kExpected[] = {0x40, 0x55, 0x56, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x80,
                                            0x00, 0x00, 0x00, 0x48, 0x8B, 0xF2, 0x4C, 0x8B, 0xE9};
        uint8_t actual[sizeof(kExpected)];
        if (!mem::ReadBytes(show, actual, sizeof(actual)) || std::memcmp(actual, kExpected, sizeof(actual)) != 0) {
            g_partStatus = StrFormat("context hook signature mismatch (%s)", HexBytes(show, sizeof(kExpected)).c_str());
            LOG_WARN("%s", g_partStatus.c_str());
        } else if (Install("damage-context", reinterpret_cast<void*>(show), reinterpret_cast<void*>(&ShowDamageContextDetour),
                           reinterpret_cast<void**>(&g_showDamageContext))) {
            g_partStatus = StrFormat("collision hook 0x%llX", static_cast<unsigned long long>(show));
        } else {
            g_partStatus = "context hook failed";
        }
    } else if (!partContext) {
        g_partStatus = "disabled in settings";
    }
    meter::Hits().NotePartContext(g_partStatus.c_str());
    return true;
}

std::string PartHookStatus() { return g_partStatus; }

// ---- actions ----------------------------------------------------------------------

bool InstallActionHook(const mem::ModuleInfo& exe) {
    const uintptr_t target = FindSignature(exe, kDoAction);
    return target && Install("do-action", reinterpret_cast<void*>(target), reinterpret_cast<void*>(&DoActionDetour),
                             reinterpret_cast<void**>(&g_doAction));
}

void SetLocalActionController(uintptr_t controller) { g_localController.store(controller, std::memory_order_relaxed); }

// ---- main update ------------------------------------------------------------------

bool InstallMainUpdateHook(const mem::ModuleInfo& exe, void (*callback)()) {
    const uintptr_t target = FindSignature(exe, kMainUpdate);
    if (!target)
        return false;
    g_mainCallback = callback;
    return Install("main-update", reinterpret_cast<void*>(target), reinterpret_cast<void*>(&MainUpdateDetour),
                   reinterpret_cast<void**>(&g_mainUpdate));
}

bool MainUpdateAlive() {
    const int64_t last = g_mainLastQpc.load(std::memory_order_relaxed);
    return last != 0 && SecondsSince(last) < 1.0;
}

bool OnGameThread() { return GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed); }

// ---- quest names --------------------------------------------------------------------

bool ResolveQuestNameFunction(const mem::ModuleInfo& exe) {
    const uintptr_t target = FindSignature(exe, kGetQuestName);
    g_getQuestName = reinterpret_cast<GetQuestNameFn>(target);
    return target != 0;
}

std::string QuestName(uintptr_t questInstance, int questId) {
    if (!g_getQuestName || !questInstance || questId <= 0 || !OnGameThread())
        return {};
    const char* text = g_getQuestName(questInstance, questId, 0);
    std::string name;
    if (!text || !mem::ReadCString(reinterpret_cast<uintptr_t>(text), 256, name))
        return {};
    // Placeholder entries in the game's text tables.
    if (name.empty() || name == "Unavailable" || name == "Unavaliable" || StartsWith(name, "---"))
        return {};
    return name;
}

}  // namespace dps::hooks
