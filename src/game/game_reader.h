#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "address_map.h"
#include "common.h"
#include "memory.h"

namespace dps::game {

// Struct offsets shared by builds 421631 and 421810 (SharpPluginLoader / HunterPie).
namespace off {
constexpr int kQuestId = 0x4C;
constexpr int kQuestStars = 0x50;
constexpr int kQuestState = 0x54;
constexpr int kQuestRewardMoney = 0x60;
constexpr int kQuestRewardHrp = 0x68;
constexpr int kTimerMaxFrames = 0x10;  // from the quest timer block

constexpr int kEntityHealth = 0x7630;   // hunter -> health component
constexpr int kMonsterHealth = 0x7670;  // monster -> health component
constexpr int kHealthMax = 0x60;
constexpr int kHealthCurrent = 0x64;
constexpr int kMonsterType = 0x12280;
constexpr int kMonsterVariant = 0x12288;
constexpr int kMonsterEmPath = 0x2A0;   // -> resource whose path string is at +0x0C
constexpr int kComponentOwner = 0x138;  // monster-list component -> monster
constexpr int kMonsterListLength = 128;

constexpr int kActionController = 0x61C8;  // inline in the hunter entity
constexpr int kActionLists = 0x68;
constexpr int kActionListStride = 0x10;
constexpr int kActionName = 0x20;

constexpr int kDamageStride = 0x2A0;
constexpr int kPartyMemberStride = 0x58;
constexpr int kPartyName = 0x49;
constexpr int kNameBytes = 32;
constexpr uintptr_t kSaveSlotStride = 0x26CC00;
}  // namespace off

enum QuestState : uint32_t {
    kQuestNone = 0,
    kQuestReady = 1,
    kQuestInQuest = 2,
    kQuestSuccess = 3,
    kQuestCompleted = 4,
    kQuestFailed = 5,
    kQuestAbandon = 6,
    kQuestQuit = 7,
};

const char* DescribeQuestState(uint32_t state);

struct QuestInfo {
    bool ok = false;
    uintptr_t instance = 0;
    int id = 0;
    uint32_t state = 0;
    int stars = 0;
    uint32_t rewardMoney = 0;
    uint32_t rewardHrp = 0;
    /// Seconds since the quest started from the in-game quest timer; < 0 when unreadable.
    float timerElapsed = -1.0f;
    float timerMax = 0.0f;
};

struct MonsterInfo {
    uintptr_t instance = 0;
    bool valid = false;  // type and HP readable; false = listed but mid-spawn/teardown
    int type = -1;
    int variant = 0;
    float maxHealth = 0.0f;
    float health = 0.0f;
};

struct PartySlotProbe {
    uintptr_t memberPtr = 0;
    bool ptrOk = false;
    std::string name;  // validated UTF-8 hunter name, empty when absent
    std::string nameHex;
};

struct PartyRead {
    int partySize = 0;  // 0 = solo / no session party
    std::string localName;
    uintptr_t damageBase = 0;
    std::string damageError;
    bool hasTable = false;  // quest-award table allocated and sane
    std::array<int, kPartySlots> rawDamage{};
    uintptr_t partyArray = 0;
    std::string partyError;
    std::array<PartySlotProbe, kPartySlots> slots{};
};

/// Reads game state through guarded memory reads only: nothing here calls into
/// game code, so a stale pointer yields "unknown" instead of a crash.
class GameReader {
public:
    bool Init(const mem::ModuleInfo& exe, const AddressMap& map);
    bool Ready() const { return exe_.base != 0; }
    const mem::ModuleInfo& Exe() const { return exe_; }
    uintptr_t Base() const { return exe_.base; }

    QuestInfo ReadQuest() const;
    /// Stage id, 0 in the main menu, -1 when unreadable.
    int ReadStage() const;
    uintptr_t LocalPlayer() const;
    /// Weapon type 0..13, or -1.
    int LocalWeapon() const;
    bool EntityHealth(uintptr_t entity, float& health, float& maxHealth) const;
    std::string SaveName() const;
    PartyRead ReadParty() const;

    bool HasMonsterList() const { return monsterList_ != 0; }
    /// Large monsters from the game's monster list. False when this build has no list
    /// or it could not be read this time (the caller must not infer despawns then).
    bool ReadLargeMonsters(std::vector<MonsterInfo>& out);
    bool ReadMonster(uintptr_t instance, MonsterInfo& out) const;
    /// Classifies an arbitrary hit target: large monster (em, not ems) or not.
    bool IsLargeMonster(uintptr_t instance, int* type = nullptr);

    bool ReadDeaths(int& deaths, int& maxDeaths) const;
    std::string ActionName(uintptr_t entity, int actionSet, int actionId) const;
    /// True if the object's vtable points into the game image.
    bool HasGameVtable(uintptr_t object) const;

private:
    struct Classified {
        int type = -1;
        bool large = false;
    };

    mem::ModuleInfo exe_;
    uintptr_t questData_ = 0;
    std::vector<int> questDataOffsets_, questExtraOffsets_, questTimerOffsets_;
    uintptr_t zone_ = 0;
    std::vector<int> zoneOffsets_;
    uintptr_t equipment_ = 0, weapon_ = 0;
    std::vector<int> localPlayerOffsets_, weaponOffsets_;
    uintptr_t level_ = 0;
    std::vector<int> levelOffsets_;
    uintptr_t party_ = 0, damage_ = 0, session_ = 0;
    std::vector<int> partyOffsets_, damageOffsets_, sessionOffsets_;
    uintptr_t monsterList_ = 0;
    std::vector<int> monsterListOffsets_;

    Lock classifyLock_;
    std::map<uintptr_t, Classified> classified_;
};

/// Validates and decodes a fixed-size UTF-8 hunter name field.
bool DecodeHunterName(const uint8_t* bytes, size_t length, std::string& out);

}  // namespace dps::game
