#include "meter/monster_tracker.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "game_data.h"

namespace dps::meter {

bool MonsterTracker::IsTrackedType(int type) {
    switch (type) {
        case 0x36:  // Magmacore
        case 0x37:  // Magmacore2
        case 0x39:  // SmallBarrel
        case 0x3A:  // LargeBarrel
        case 0x3B:  // TrainingPole
        case 0x3C:  // TrainingWagon
        case 0x62:  // Unavailable
            return false;
        default:
            return type >= 0;
    }
}

void MonsterTracker::Reset(bool countFromFirstSight) {
    live_.clear();
    candidates_.clear();
    tracked_.clear();
    described_.clear();
    completed_ = 0;
    fromFirstSight_ = countFromFirstSight;
}

void MonsterTracker::Rebase() {
    for (auto& [instance, live] : live_) {
        live.base += live.dealt;
        live.dealt = 0;
    }
    completed_ = 0;
}

void MonsterTracker::AddCandidate(uintptr_t instance) {
    if (instance && std::find(candidates_.begin(), candidates_.end(), instance) == candidates_.end() && candidates_.size() < 64)
        candidates_.push_back(instance);
}

std::vector<uintptr_t> MonsterTracker::LiveInstances() const {
    std::vector<uintptr_t> out;
    out.reserve(live_.size());
    for (const auto& [instance, live] : live_)
        out.push_back(instance);
    return out;
}

int MonsterTracker::Total() const {
    int total = completed_;
    for (const auto& [instance, live] : live_)
        total += live.dealt;
    return total;
}

int MonsterTracker::Poll(game::GameReader& reader) {
    std::vector<game::MonsterInfo> monsters;
    if (reader.HasMonsterList()) {
        // An unreadable list cannot tell which monsters despawned: keep everything.
        listAvailable_ = reader.ReadLargeMonsters(monsters);
        if (!listAvailable_)
            return Total();
    } else {
        // No monster list for this build: follow the monsters we have hit.
        listAvailable_ = false;
        for (uintptr_t instance : candidates_) {
            game::MonsterInfo info;
            if (reader.ReadMonster(instance, info) || info.type >= 0)
                monsters.push_back(info);
        }
    }

    std::set<uintptr_t> seen;
    std::vector<MonsterState> tracked;
    std::string described;
    for (const auto& monster : monsters) {
        seen.insert(monster.instance);
        if (!described.empty())
            described += " | ";
        described += StrFormat("%s@0x%llX %.0f/%.0f", data::MonsterLabel(monster.type).c_str(),
                               static_cast<unsigned long long>(monster.instance), monster.health, monster.maxHealth);

        // A failed HP read does not mean this instance despawned (it stays in seen).
        if (!monster.valid || !IsTrackedType(monster.type) || monster.maxHealth < 800.0f)
            continue;

        const int lost = static_cast<int>(std::clamp(monster.maxHealth - std::max(monster.health, 0.0f), 0.0f, monster.maxHealth));
        auto it = live_.find(monster.instance);
        if (it == live_.end()) {
            Live live;
            live.base = fromFirstSight_ ? lost : 0;
            it = live_.emplace(monster.instance, live).first;
        } else if (static_cast<float>(lost) + 0.25f * monster.maxHealth < static_cast<float>(it->second.base + it->second.dealt)) {
            // HP jumped back up: the game reused this instance for a new monster (second
            // Beotodus after the first died). Bank the old damage and start over.
            completed_ += it->second.dealt;
            it->second = Live{};
        }
        it->second.dealt = std::max(it->second.dealt, lost - it->second.base);

        MonsterState state;
        state.instance = monster.instance;
        state.type = monster.type;
        state.name = data::MonsterLabel(monster.type);
        state.variant = monster.variant;
        state.maxHealth = monster.maxHealth;
        state.health = monster.health;
        tracked.push_back(std::move(state));
    }

    for (auto it = live_.begin(); it != live_.end();) {
        if (!seen.count(it->first)) {
            completed_ += it->second.dealt;
            it = live_.erase(it);
        } else {
            ++it;
        }
    }
    if (!listAvailable_) {
        candidates_.erase(std::remove_if(candidates_.begin(), candidates_.end(), [&](uintptr_t p) { return !seen.count(p); }),
                          candidates_.end());
    }

    tracked_ = std::move(tracked);
    described_ = described.empty() ? "(none)" : described;
    return Total();
}

}  // namespace dps::meter
