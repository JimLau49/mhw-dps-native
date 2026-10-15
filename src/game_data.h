#pragma once

#include <optional>
#include <string>

namespace dps::data {

/// Display name of a stage id, or "Stage <id>".
std::string StageLabel(int stageId);
/// Canonical weapon key used in fight logs and personal bests ("GreatSword"), or nullptr.
const char* WeaponKey(int weaponType);
/// Display name for a weapon key or type ("Great Sword").
const char* WeaponDisplay(int weaponType);
/// English monster name, or "Monster <id>".
std::string MonsterLabel(int monsterType);
/// Canonical part id for a normal (non-severable) collision slot of a monster.
std::optional<int> PartFromNormalSlot(int monsterType, int slot);
/// Display name for a canonical part id, or nullptr.
const char* PartName(int monsterType, int partId);
/// HunterPie's large-monster bitset (also covers the training pole, id 59).
bool IsLargeMonsterId(int monsterType);

}  // namespace dps::data
