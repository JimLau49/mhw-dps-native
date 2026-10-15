#pragma once

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "meter/hit_tracker.h"
#include "meter/monster_tracker.h"

namespace dps::meter {

// Fight log model: the same schema-2 JSON the C# MhwDpsMeter writes, so its web and
// GPUI viewers read these logs unchanged. Optional fields use empty/-1 sentinels.

struct LogHit {
    float t = 0.0f;
    int slot = 0;
    std::string monster;  // "m1"..., empty for untracked targets
    int damage = 0;
    bool crit = false;
    bool tenderized = false;
    int attackId = 0;
    int part = -1;
    std::string partName;
    int actionSet = 0;
    int actionId = 0;
    std::string action;
};

struct LogEvent {
    float t = 0.0f;
    std::string type;
    int slot = -1;
    std::string monster;
    std::string detail;
    bool hasDetail = false;
};

struct LogMonster {
    std::string id;
    int type = 0;
    std::string name;
    int variant = 0;
    float maxHealth = 0.0f;
    float lastHealth = 0.0f;
    float firstSeenT = 0.0f;
    float diedT = -1.0f;
};

struct LogPlayer {
    int slot = 0;
    std::string name;
    bool isLocal = false;
    std::string weapon;
    int damage = 0;
    float dps = 0.0f;
    float percent = 0.0f;
    int carts = 0;
};

struct LogSample {
    float t = 0.0f;
    std::array<int, kPartySlots> damage{};
};

struct LogRewards {
    bool present = false;
    int zenny = 0;
    int hunterRankPoints = 0;
    int stars = 0;
};

struct FightLog {
    std::string kind = "quest";  // or "trial"
    int gameBuild = 0;
    int questId = 0;
    std::string questName;
    std::string result;
    int stageId = 0;
    std::string stage;
    int64_t startedAtMs = 0;  // Unix ms, UTC
    int64_t endedAtMs = 0;
    float durationSeconds = 0.0f;
    std::string timerSource = "local";
    std::string hitCoverage = "local";
    LogRewards rewards;
    std::vector<LogPlayer> players;
    std::vector<LogMonster> monsters;
    std::vector<LogSample> samples;
    std::vector<LogHit> hits;
    std::vector<LogEvent> events;
    std::string fileName;  // set when saved
};

/// Collects everything a fight log needs beyond party totals: monsters seen, the
/// local hunter's individual hits and timeline events. Used from one thread.
class Recorder {
public:
    static constexpr size_t kMaxHits = 50'000;
    static constexpr size_t kMaxEvents = 5'000;

    using ActionResolver = std::function<std::string(int actionSet, int actionId)>;
    explicit Recorder(ActionResolver resolver) : resolveAction_(std::move(resolver)) {}

    void Reset();
    void ObserveMonsters(float elapsed, const std::vector<MonsterState>& monsters);
    /// (slot, name) of the current roster; records join/leave events on change.
    void ObserveParty(float elapsed, const std::vector<std::pair<int, std::string>>& roster);
    void ObserveWeapon(float elapsed, int weaponType, int localSlot);
    /// Hunt hits: elapsed is the hunt clock now; each hit's age puts it on that clock.
    void AddHits(float elapsed, const std::vector<HitRecord>& hits, int localSlot);
    /// Trial hits: exact offset from the trial's first hit.
    void AddHitsRelativeTo(int64_t startQpc, const std::vector<HitRecord>& hits, int localSlot);
    void AddCart(float elapsed, int slot, const std::string& name);
    void AddEvent(float elapsed, const std::string& type, int slot = -1, const std::string* detail = nullptr);

    std::vector<LogMonster> Monsters() const;
    std::vector<LogHit> Hits() const;
    std::vector<LogEvent> Events() const;
    size_t HitCount() const { return hits_.size(); }
    const std::string& LocalWeapon() const { return localWeapon_; }
    std::string WeaponOf(int slot) const;
    int CartsOf(int slot) const;
    /// "m1" for the only large monster alive, else "".
    std::string SingleLiveMonsterId() const;

private:
    void AddHitsCore(const std::vector<HitRecord>& hits, int localSlot, const std::function<float(const HitRecord&)>& timeOf);
    const std::string& ActionName(int actionSet, int actionId);
    void AddEventInternal(float t, const std::string& type, int slot, const std::string& monster, const std::string* detail);

    ActionResolver resolveAction_;
    std::vector<LogHit> hits_;
    std::vector<LogEvent> events_;
    std::map<uintptr_t, size_t> monsterByInstance_;  // index into monsters_
    std::vector<LogMonster> monsters_;
    std::map<int, std::string> weaponBySlot_;
    std::map<std::pair<int, int>, std::string> actionNames_;
    std::map<int, std::string> roster_;
    std::map<int, int> cartsBySlot_;
    std::string localWeapon_;
};

}  // namespace dps::meter
