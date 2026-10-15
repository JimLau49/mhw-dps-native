#pragma once

#include <map>
#include <string>
#include <vector>

#include "game/game_reader.h"

namespace dps::meter {

/// Per-poll view of one tracked (large) monster.
struct MonsterState {
    uintptr_t instance = 0;
    int type = -1;
    std::string name;
    int variant = 0;
    float maxHealth = 0.0f;
    float health = 0.0f;
};

/// Sums HP lost by large monsters, including ones that already despawned. Used as
/// the solo/arena fallback when the quest-award damage table is not allocated.
class MonsterTracker {
public:
    void Reset();
    /// Monster hit by the local hunter; tracked even without a monster list (421631).
    void AddCandidate(uintptr_t instance);
    /// Returns the total HP lost by tracked monsters.
    int Poll(game::GameReader& reader);

    const std::vector<MonsterState>& LastTracked() const { return tracked_; }
    std::vector<uintptr_t> LiveInstances() const;
    const std::string& LastMonsters() const { return described_; }
    bool ListAvailable() const { return listAvailable_; }

    /// Barrels, training objects and magmacores are not hunt targets.
    static bool IsTrackedType(int type);

private:
    int Total() const;

    std::map<uintptr_t, int> live_;  // instance -> HP lost
    std::vector<uintptr_t> candidates_;
    std::vector<MonsterState> tracked_;
    std::string described_;
    int completed_ = 0;
    bool listAvailable_ = false;
};

}  // namespace dps::meter
