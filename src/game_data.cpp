#include "game_data.h"

#include <map>
#include <vector>

#include "common.h"

namespace dps::data {
namespace {

struct NameEntry {
    int id;
    const char* key;
    const char* name;
};

struct PartEntry {
    int monster;
    int part;
    bool severable;
    const char* name;
};

#include "game_data_tables.inc"

// Game weapon ids (wp00..wp13). Keys match the C# plugin's log keys.
const NameEntry kWeapons[] = {
    {0, "GreatSword", "Great Sword"},     {1, "SwordAndShield", "Sword & Shield"}, {2, "DualBlades", "Dual Blades"},
    {3, "LongSword", "Long Sword"},       {4, "Hammer", "Hammer"},                {5, "HuntingHorn", "Hunting Horn"},
    {6, "Lance", "Lance"},                {7, "GunLance", "Gunlance"},            {8, "SwitchAxe", "Switch Axe"},
    {9, "ChargeBlade", "Charge Blade"},   {10, "InsectGlaive", "Insect Glaive"},  {11, "Bow", "Bow"},
    {12, "HeavyBowgun", "Heavy Bowgun"},  {13, "LightBowgun", "Light Bowgun"},
};

template <size_t N>
const NameEntry* FindEntry(const NameEntry (&table)[N], int id) {
    for (const auto& entry : table) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

/// Normal-part ids per monster in collision-slot order (severable meters skipped).
const std::map<int, std::vector<int>>& NormalParts() {
    static const std::map<int, std::vector<int>> table = [] {
        std::map<int, std::vector<int>> out;
        for (const auto& entry : kMonsterParts) {
            if (!entry.severable)
                out[entry.monster].push_back(entry.part);
        }
        return out;
    }();
    return table;
}

}  // namespace

std::string StageLabel(int stageId) {
    const NameEntry* entry = FindEntry(kStageNames, stageId);
    return entry ? entry->name : StrFormat("Stage %d", stageId);
}

const char* WeaponKey(int weaponType) {
    const NameEntry* entry = FindEntry(kWeapons, weaponType);
    return entry ? entry->key : nullptr;
}

const char* WeaponDisplay(int weaponType) {
    const NameEntry* entry = FindEntry(kWeapons, weaponType);
    return entry ? entry->name : "?";
}

std::string MonsterLabel(int monsterType) {
    const NameEntry* entry = FindEntry(kMonsterNames, monsterType);
    if (!entry || std::string_view(entry->key) == "Unavaliable")
        return StrFormat("Monster %d", monsterType);
    return entry->name;
}

std::optional<int> PartFromNormalSlot(int monsterType, int slot) {
    const auto& table = NormalParts();
    auto it = table.find(monsterType);
    if (it == table.end() || slot < 0 || slot >= static_cast<int>(it->second.size()))
        return std::nullopt;
    return it->second[static_cast<size_t>(slot)];
}

const char* PartName(int monsterType, int partId) {
    for (const auto& entry : kMonsterParts) {
        if (entry.monster == monsterType && entry.part == partId)
            return entry.name;
    }
    return nullptr;
}

bool IsLargeMonsterId(int monsterType) {
    // Each bit marks a monster id that is a large monster (HunterPie WorldUtils).
    constexpr uint64_t kLow = 0b1110'1000000010'0000000000'1111111111'1111111111'1111111111'1010010011ULL;
    constexpr uint64_t kHigh = 0b00000000'0000000000'0000000011'1111111111'1110000011'1111111111'111111ULL;
    if (monsterType < 0)
        return false;
    if (monsterType < 64)
        return ((kLow >> monsterType) & 1) != 0;
    if (monsterType < 128)
        return ((kHigh >> (monsterType - 64)) & 1) != 0;
    return false;
}

}  // namespace dps::data
