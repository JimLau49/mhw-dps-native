#include "game/game_reader.h"

#include <algorithm>
#include <cstring>

#include "game_data.h"
#include "log.h"

namespace dps::game {
namespace {

// Known quest time limits in frames (15/20/30/35/50 min, HunterPie MaxQuestTimers).
constexpr uint32_t kQuestLimits[] = {54000, 72000, 108000, 126000, 180000};

uint32_t SnapQuestLimit(uint32_t raw) {
    for (uint32_t limit : kQuestLimits) {
        if (raw <= limit && limit - raw <= 3600)
            return limit;
    }
    return raw;
}

bool ValidHealth(float max, float current) {
    return max >= 1.0f && max <= 50'000'000.0f && current == current && max == max && current >= -1.0f &&
           current <= max * 1.01f + 1.0f;
}

}  // namespace

const char* DescribeQuestState(uint32_t state) {
    switch (state) {
        case kQuestNone: return "none";
        case kQuestReady: return "ready (hub)";
        case kQuestInQuest: return "in quest";
        case kQuestSuccess: return "success";
        case kQuestCompleted: return "completed";
        case kQuestFailed: return "failed";
        case kQuestAbandon: return "abandon";
        case kQuestQuit: return "quit";
        default: return "?";
    }
}

bool DecodeHunterName(const uint8_t* bytes, size_t length, std::string& out) {
    out.clear();
    size_t end = 0;
    while (end < length && bytes[end] != 0)
        ++end;
    if (end == 0)
        return false;

    std::string text(reinterpret_cast<const char*>(bytes), end);
    const std::wstring wide = [&] {
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (count <= 0)
            return std::wstring();
        std::wstring w(static_cast<size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), w.data(), count);
        return w;
    }();
    if (wide.empty() || wide.size() > 32)
        return false;
    for (wchar_t c : wide) {
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0xFFFD)
            return false;
    }

    while (!text.empty() && text.back() == ' ')
        text.pop_back();
    while (!text.empty() && text.front() == ' ')
        text.erase(text.begin());
    if (text.empty())
        return false;
    out = text;
    return true;
}

bool GameReader::Init(const mem::ModuleInfo& exe, const AddressMap& map) {
    exe_ = exe;
    auto abs = [&](const char* key) {
        uintptr_t rva = 0;
        return map.Address(key, rva) ? exe_.base + rva : 0;
    };

    questData_ = abs("QUEST_DATA_ADDRESS");
    questDataOffsets_ = map.OffsetsOr("QUEST_DATA_OFFSETS");
    if (questDataOffsets_.empty())
        questDataOffsets_ = {0};
    questExtraOffsets_ = map.OffsetsOr("QUEST_EXTRA_DATA_OFFSETS");
    questTimerOffsets_ = map.OffsetsOr("QUEST_TIMER_OFFSETS");
    zone_ = abs("ZONE_OFFSET");
    zoneOffsets_ = map.OffsetsOr("ZoneOffsets");
    equipment_ = abs("EQUIPMENT_ADDRESS");
    weapon_ = abs("WEAPON_ADDRESS");
    localPlayerOffsets_ = map.OffsetsOr("LOCAL_PLAYER_OFFSETS");
    weaponOffsets_ = map.OffsetsOr("WEAPON_OFFSETS");
    level_ = abs("LEVEL_OFFSET");
    levelOffsets_ = map.OffsetsOr("LevelOffsets");
    party_ = abs("PARTY_ADDRESS");
    partyOffsets_ = map.OffsetsOr("PARTY_OFFSETS");
    damage_ = abs("DAMAGE_ADDRESS");
    damageOffsets_ = map.OffsetsOr("DAMAGE_OFFSETS");
    session_ = abs("SESSION_OFFSET");
    if (!session_)
        session_ = damage_;
    sessionOffsets_ = map.OffsetsOr("SESSION_PARTY_OFFSETS");
    monsterList_ = abs("MONSTER_LIST_ADDRESS");
    monsterListOffsets_ = map.OffsetsOr("MONSTER_LIST_OFFSETS");
    if (monsterListOffsets_.empty())
        monsterList_ = 0;
    return questData_ != 0;
}

QuestInfo GameReader::ReadQuest() const {
    QuestInfo info;
    if (!questData_)
        return info;
    const uintptr_t quest = mem::Follow(questData_, questDataOffsets_);
    if (!quest)
        return info;

    int id = 0;
    uint32_t state = 0;
    if (!mem::Read(quest + off::kQuestId, id) || !mem::Read(quest + off::kQuestState, state))
        return info;
    info.ok = true;
    info.instance = quest;
    info.id = id;
    info.state = state;
    mem::Read(quest + off::kQuestStars, info.stars);
    mem::Read(quest + off::kQuestRewardMoney, info.rewardMoney);
    mem::Read(quest + off::kQuestRewardHrp, info.rewardHrp);

    // HunterPie (421631+): frames remaining (u64) with the limit (u32) 0x10 later.
    if (!questTimerOffsets_.empty()) {
        const uintptr_t timer = mem::Follow(questData_, questTimerOffsets_);
        uint64_t remaining = 0;
        uint32_t limitRaw = 0;
        if (timer && mem::Read(timer, remaining) && mem::Read(timer + off::kTimerMaxFrames, limitRaw)) {
            const uint32_t limit = SnapQuestLimit(limitRaw);
            if (limit >= 3600 && limit <= 60 * 60 * 60 * 3 && remaining <= limit) {
                info.timerMax = static_cast<float>(limit) / 60.0f;
                info.timerElapsed = static_cast<float>(limit - remaining) / 60.0f;
            }
        }
    }
    return info;
}

int GameReader::ReadStage() const {
    if (!zone_ || zoneOffsets_.empty())
        return -1;
    const uintptr_t address = mem::Follow(zone_, zoneOffsets_);
    int stage = 0;
    if (!address || !mem::Read(address, stage))
        return -1;
    return stage;
}

uintptr_t GameReader::LocalPlayer() const {
    const uintptr_t root = equipment_ ? equipment_ : weapon_;
    if (!root || localPlayerOffsets_.empty())
        return 0;
    const uintptr_t player = mem::Follow(root, localPlayerOffsets_);
    return mem::IsUserPtr(player) && HasGameVtable(player) ? player : 0;
}

int GameReader::LocalWeapon() const {
    if (!weapon_ || weaponOffsets_.empty())
        return -1;
    const uintptr_t address = mem::Follow(weapon_, weaponOffsets_);
    uint8_t type = 0xFF;
    if (!address || !mem::Read(address, type) || type > 13)
        return -1;
    return type;
}

bool GameReader::EntityHealth(uintptr_t entity, float& health, float& maxHealth) const {
    uintptr_t component = 0;
    return mem::IsUserPtr(entity) && mem::ReadPtr(entity + off::kEntityHealth, component) &&
           mem::Read(component + off::kHealthMax, maxHealth) && mem::Read(component + off::kHealthCurrent, health) &&
           ValidHealth(maxHealth, health);
}

std::string GameReader::SaveName() const {
    if (!level_ || levelOffsets_.empty())
        return {};
    const uintptr_t firstSave = mem::Follow(level_, levelOffsets_);
    if (!firstSave)
        return {};

    uint32_t slot = 0;
    if (!mem::Read(firstSave + 0x44, slot) || slot > 2)
        slot = 0;
    uintptr_t header = 0;
    if (!mem::ReadPtr(firstSave, header))
        return {};

    uint8_t bytes[off::kNameBytes];
    std::string name;
    if (!mem::ReadBytes(header + off::kSaveSlotStride * slot + 0x50, bytes, sizeof(bytes)) ||
        !DecodeHunterName(bytes, sizeof(bytes), name))
        return {};
    return name;
}

PartyRead GameReader::ReadParty() const {
    PartyRead read;

    if (session_ && !sessionOffsets_.empty()) {
        const uintptr_t address = mem::Follow(session_, sessionOffsets_);
        int size = 0;
        if (address && mem::Read(address, size) && size >= 1 && size <= kPartySlots)
            read.partySize = size;
    }

    read.localName = SaveName();

    // Quest-award table (results-screen totals, synced for every hunter).
    if (damage_ && !damageOffsets_.empty()) {
        read.damageBase = mem::Follow(damage_, damageOffsets_, &read.damageError);
        if (read.damageBase) {
            read.hasTable = true;
            for (int slot = 0; slot < kPartySlots; ++slot) {
                int value = 0;
                if (!mem::Read(read.damageBase + static_cast<uintptr_t>(slot) * off::kDamageStride, value) || value < 0 ||
                    value > 50'000'000) {
                    read.hasTable = false;
                    break;
                }
                read.rawDamage[slot] = value;
            }
            if (!read.hasTable)
                read.rawDamage = {};
        }
    }

    // Roster: 4 structs of 0x58 bytes, each a pointer to a member with its name at +0x49.
    if (party_ && !partyOffsets_.empty()) {
        read.partyArray = mem::Follow(party_, partyOffsets_, &read.partyError);
        for (int slot = 0; read.partyArray && slot < kPartySlots; ++slot) {
            auto& probe = read.slots[slot];
            if (!mem::ReadPtr(read.partyArray + static_cast<uintptr_t>(slot) * off::kPartyMemberStride, probe.memberPtr)) {
                probe.memberPtr = 0;
                continue;
            }
            probe.ptrOk = true;
            uint8_t bytes[off::kNameBytes];
            if (mem::ReadBytes(probe.memberPtr + off::kPartyName, bytes, sizeof(bytes))) {
                for (size_t i = 0; i < 16; ++i)
                    probe.nameHex += StrFormat("%02X", bytes[i]);
                DecodeHunterName(bytes, sizeof(bytes), probe.name);
            }
        }
    }
    return read;
}

bool GameReader::ReadMonster(uintptr_t instance, MonsterInfo& out) const {
    out = {};
    out.instance = instance;
    uintptr_t component = 0;
    if (!mem::Read(instance + off::kMonsterType, out.type) || out.type < 0 || out.type > 0x200)
        return false;
    uint32_t variant = 0;
    mem::Read(instance + off::kMonsterVariant, variant);
    out.variant = static_cast<int>(variant);
    out.valid = mem::ReadPtr(instance + off::kMonsterHealth, component) &&
                mem::Read(component + off::kHealthMax, out.maxHealth) &&
                mem::Read(component + off::kHealthCurrent, out.health) && ValidHealth(out.maxHealth, out.health);
    return out.valid;
}

bool GameReader::IsLargeMonster(uintptr_t instance, int* typeOut) {
    int type = -1;
    if (!mem::IsUserPtr(instance) || !mem::Read(instance + off::kMonsterType, type))
        return false;
    if (typeOut)
        *typeOut = type;

    {
        LockGuard guard(classifyLock_);
        auto it = classified_.find(instance);
        if (it != classified_.end() && it->second.type == type)
            return it->second.large;
    }

    // "em\em001\..." is a large monster, "em\ems..." a small one (HunterPie). Without a
    // readable path fall back to the id bitset.
    bool large = false;
    uintptr_t resource = 0;
    std::string path;
    if (mem::ReadPtr(instance + off::kMonsterEmPath, resource) && mem::ReadCString(resource + 0x0C, 64, path) &&
        StartsWith(path, "em\\"))
        large = StartsWith(path, "em\\em") && !StartsWith(path, "em\\ems");
    else
        large = data::IsLargeMonsterId(type);

    LockGuard guard(classifyLock_);
    if (classified_.size() > 4096)
        classified_.clear();
    classified_[instance] = Classified{type, large};
    return large;
}

bool GameReader::ReadLargeMonsters(std::vector<MonsterInfo>& out) {
    out.clear();
    if (!monsterList_)
        return false;
    const uintptr_t list = mem::Follow(monsterList_, monsterListOffsets_);
    if (!list)
        return false;

    uintptr_t components[off::kMonsterListLength] = {};
    if (!mem::ReadBytes(list, components, sizeof(components)))
        return false;

    for (uintptr_t component : components) {
        uintptr_t monster = 0;
        if (!mem::IsUserPtr(component) || !mem::ReadPtr(component + off::kComponentOwner, monster))
            continue;
        if (std::any_of(out.begin(), out.end(), [&](const MonsterInfo& m) { return m.instance == monster; }))
            continue;
        if (!IsLargeMonster(monster))
            continue;
        MonsterInfo info;
        info.valid = ReadMonster(monster, info);
        info.instance = monster;
        out.push_back(info);
    }
    return true;
}

bool GameReader::ReadDeaths(int& deaths, int& maxDeaths) const {
    if (!questData_ || questExtraOffsets_.empty())
        return false;
    const uintptr_t extra = mem::Follow(questData_, questExtraOffsets_);
    if (!extra || !mem::Read(extra + 4, deaths) || deaths < 0 || deaths > 99)
        return false;
    if (!mem::Read(extra, maxDeaths) || maxDeaths < 0 || maxDeaths > 99)
        maxDeaths = 0;
    return true;
}

std::string GameReader::ActionName(uintptr_t entity, int actionSet, int actionId) const {
    if (actionSet < 0 || actionSet > 3 || actionId < 0 || actionId >= 4096 || !mem::IsUserPtr(entity))
        return {};
    const uintptr_t list = entity + off::kActionController + off::kActionLists + static_cast<uintptr_t>(actionSet) * off::kActionListStride;
    uintptr_t actions = 0, action = 0, name = 0;
    int count = 0;
    if (!mem::ReadPtr(list, actions) || !mem::Read(list + 0x08, count) || count <= 0 || count > 4096 || actionId >= count ||
        !mem::ReadPtr(actions + static_cast<uintptr_t>(actionId) * 8, action) || !mem::ReadPtr(action + off::kActionName, name))
        return {};
    std::string text;
    if (!mem::ReadCString(name, 255, text))
        return {};
    return text;
}

bool GameReader::HasGameVtable(uintptr_t object) const {
    uintptr_t vtable = 0;
    return mem::Read(object, vtable) && exe_.Contains(vtable);
}

}  // namespace dps::game
