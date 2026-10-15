#include "meter/recorder.h"

#include <algorithm>

#include "game_data.h"

namespace dps::meter {

void Recorder::Reset() {
    hits_.clear();
    events_.clear();
    monsterByInstance_.clear();
    monsters_.clear();
    weaponBySlot_.clear();
    actionNames_.clear();
    roster_.clear();
    cartsBySlot_.clear();
    localWeapon_.clear();
}

void Recorder::ObserveMonsters(float elapsed, const std::vector<MonsterState>& monsters) {
    for (const auto& state : monsters) {
        auto it = monsterByInstance_.find(state.instance);
        bool create = it == monsterByInstance_.end();
        if (!create) {
            const LogMonster& entry = monsters_[it->second];
            // Same pointer but HP jumped back up after a death: the game reused the
            // instance for a new monster (second Beotodus in Trophy Fishin').
            const bool respawned = entry.maxHealth > 0.0f &&
                                   (entry.diedT >= 0.0f || state.health > entry.lastHealth + 0.25f * entry.maxHealth) &&
                                   state.health > 0.5f * state.maxHealth;
            create = respawned;
        }
        if (create) {
            LogMonster entry;
            entry.id = StrFormat("m%zu", monsters_.size() + 1);
            entry.type = state.type;
            entry.name = state.name;
            entry.variant = state.variant;
            entry.maxHealth = state.maxHealth;
            entry.firstSeenT = elapsed;
            monsters_.push_back(entry);
            monsterByInstance_[state.instance] = monsters_.size() - 1;
            it = monsterByInstance_.find(state.instance);
        }

        LogMonster& entry = monsters_[it->second];
        entry.lastHealth = state.health;
        if (entry.maxHealth <= 0.0f && state.maxHealth > 0.0f)
            entry.maxHealth = state.maxHealth;
        if (entry.diedT < 0.0f && state.health <= 0.0f && entry.maxHealth > 0.0f) {
            entry.diedT = elapsed;
            AddEventInternal(elapsed, "death", -1, entry.id, nullptr);
        }
    }
}

void Recorder::ObserveParty(float elapsed, const std::vector<std::pair<int, std::string>>& roster) {
    std::map<int, std::string> current;
    for (const auto& [slot, name] : roster) {
        if (slot >= 0 && slot < kPartySlots)
            current[slot] = name;
    }

    for (const auto& [slot, name] : current) {
        auto known = roster_.find(slot);
        if (known != roster_.end() && known->second == name)
            continue;
        if (known != roster_.end()) {
            AddEventInternal(elapsed, "leave", slot, {}, &known->second);
            weaponBySlot_.erase(slot);
        }
        AddEventInternal(elapsed, "join", slot, {}, &name);
        roster_[slot] = name;
    }

    for (auto it = roster_.begin(); it != roster_.end();) {
        if (!current.count(it->first)) {
            AddEventInternal(elapsed, "leave", it->first, {}, &it->second);
            weaponBySlot_.erase(it->first);
            it = roster_.erase(it);
        } else {
            ++it;
        }
    }
}

void Recorder::ObserveWeapon(float elapsed, int weaponType, int localSlot) {
    const char* key = data::WeaponKey(weaponType);
    if (!key)
        return;
    const std::string name = key;
    if (localSlot >= 0)
        weaponBySlot_[localSlot] = name;
    if (localWeapon_ == name)
        return;
    // Action ids are weapon-specific: a cached name would describe the old weapon.
    actionNames_.clear();
    localWeapon_ = name;
    AddEventInternal(elapsed, "weapon", localSlot, {}, &name);
}

void Recorder::AddHits(float elapsed, const std::vector<HitRecord>& hits, int localSlot) {
    if (hits.empty())
        return;
    const int64_t now = QpcNow();
    AddHitsCore(hits, localSlot, [&](const HitRecord& hit) {
        return elapsed - static_cast<float>(QpcToSeconds(now - hit.qpc));
    });
}

void Recorder::AddHitsRelativeTo(int64_t startQpc, const std::vector<HitRecord>& hits, int localSlot) {
    if (hits.empty())
        return;
    AddHitsCore(hits, localSlot, [&](const HitRecord& hit) { return static_cast<float>(QpcToSeconds(hit.qpc - startQpc)); });
}

void Recorder::AddHitsCore(const std::vector<HitRecord>& hits, int localSlot,
                           const std::function<float(const HitRecord&)>& timeOf) {
    for (const auto& hit : hits) {
        if (hits_.size() >= kMaxHits)
            return;
        LogHit row;
        row.t = std::max(0.0f, timeOf(hit));
        row.slot = std::max(0, localSlot);
        auto monster = monsterByInstance_.find(hit.target);
        if (monster != monsterByInstance_.end()) {
            const LogMonster& entry = monsters_[monster->second];
            row.monster = entry.id;
            if (hit.part >= 0) {
                const char* partName = data::PartName(entry.type, hit.part);
                if (partName)
                    row.partName = partName;
            }
        }
        row.damage = hit.damage;
        row.crit = hit.crit;
        row.tenderized = hit.tenderized;
        row.attackId = hit.attackId;
        row.part = hit.part;
        row.actionSet = hit.actionSet;
        row.actionId = hit.actionId;
        row.action = ActionName(hit.actionSet, hit.actionId);
        hits_.push_back(std::move(row));
    }
}

const std::string& Recorder::ActionName(int actionSet, int actionId) {
    const auto key = std::make_pair(actionSet, actionId);
    auto it = actionNames_.find(key);
    if (it != actionNames_.end())
        return it->second;
    std::string name;
    try {
        if (resolveAction_)
            name = resolveAction_(actionSet, actionId);
    } catch (...) {
        name.clear();
    }
    return actionNames_.emplace(key, std::move(name)).first->second;
}

void Recorder::AddCart(float elapsed, int slot, const std::string& name) {
    if (slot >= 0 && slot < kPartySlots)
        ++cartsBySlot_[slot];
    AddEventInternal(elapsed, "cart", slot, {}, name.empty() ? nullptr : &name);
}

void Recorder::AddEvent(float elapsed, const std::string& type, int slot, const std::string* detail) {
    AddEventInternal(elapsed, type, slot, {}, detail);
}

void Recorder::AddEventInternal(float t, const std::string& type, int slot, const std::string& monster, const std::string* detail) {
    if (events_.size() >= kMaxEvents)
        return;
    LogEvent event;
    event.t = t;
    event.type = type;
    event.slot = slot;
    event.monster = monster;
    if (detail) {
        event.detail = *detail;
        event.hasDetail = true;
    }
    events_.push_back(std::move(event));
}

std::vector<LogMonster> Recorder::Monsters() const {
    std::vector<LogMonster> out = monsters_;
    std::stable_sort(out.begin(), out.end(), [](const LogMonster& a, const LogMonster& b) { return a.firstSeenT < b.firstSeenT; });
    return out;
}

std::vector<LogHit> Recorder::Hits() const {
    std::vector<LogHit> out = hits_;
    std::stable_sort(out.begin(), out.end(), [](const LogHit& a, const LogHit& b) { return a.t < b.t; });
    return out;
}

std::vector<LogEvent> Recorder::Events() const {
    std::vector<LogEvent> out = events_;
    std::stable_sort(out.begin(), out.end(), [](const LogEvent& a, const LogEvent& b) { return a.t < b.t; });
    return out;
}

std::string Recorder::WeaponOf(int slot) const {
    auto it = weaponBySlot_.find(slot);
    return it == weaponBySlot_.end() ? std::string() : it->second;
}

int Recorder::CartsOf(int slot) const {
    auto it = cartsBySlot_.find(slot);
    return it == cartsBySlot_.end() ? 0 : it->second;
}

std::string Recorder::SingleLiveMonsterId() const {
    std::string id;
    int alive = 0;
    for (const auto& monster : monsters_) {
        if (monster.diedT < 0.0f && monster.lastHealth > 0.0f) {
            id = monster.id;
            ++alive;
        }
    }
    return alive == 1 ? id : std::string();
}

}  // namespace dps::meter
