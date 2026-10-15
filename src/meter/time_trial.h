#pragma once

#include <optional>
#include <string>
#include <vector>

#include "meter/recorder.h"

namespace dps::meter {

enum class TrialState {
    Idle,
    Armed,     // the clock starts on the first hit the hook counts
    Running,
    Finished,  // deadline passed; numbers frozen until re-armed or cleared
};

struct TrialMove {
    std::string name;
    int damage = 0;
    int hits = 0;
    int crits = 0;
};

/// Training-area time trial: a fixed window of damage timed from the first hit after
/// arming. The window is measured on the hits' own timestamps, so a hit that lands
/// after the deadline never counts even if the poll that drains it runs late.
class TimeTrial {
public:
    static constexpr int kMinSeconds = 5;
    static constexpr int kMaxSeconds = 600;
    static constexpr int kPresets[] = {30, 60, 90, 120, 180};

    explicit TimeTrial(Recorder::ActionResolver resolver) : recorder_(std::move(resolver)) {}

    TrialState State() const { return state_; }
    bool Active() const { return state_ != TrialState::Idle; }
    int DurationSeconds() const { return duration_; }
    int Damage() const { return damage_; }
    int HitCount() const { return hits_; }
    int Crits() const { return crits_; }
    const std::string& Weapon() const { return recorder_.LocalWeapon(); }
    /// Seconds of the window used so far; the full duration once finished.
    float Elapsed() const;
    float Remaining() const;
    /// Floored at 1 s so the first hit does not read as thousands of DPS.
    float Dps() const;

    void Arm(int durationSeconds);
    void Cancel() { Clear(); }
    /// Feeds hits drained since the last poll; true on the poll that finishes the trial.
    bool Update(const std::vector<HitRecord>& hits, int weaponType);
    std::vector<TrialMove> TopMoves(size_t count) const;
    FightLog BuildLog(const std::string& hunterName, int stageId, const std::string& stageName, int gameBuild) const;

    /// "WP_02::RANBU" reads as "RANBU" in the overlay; the full name stays in the log.
    static std::string ShortMoveName(const std::string& name);

    std::optional<int> previousBest;
    bool personalBest = false;
    std::string savedFileName;

private:
    void Clear();

    Recorder recorder_;
    TrialState state_ = TrialState::Idle;
    int duration_ = 60;
    int damage_ = 0;
    int hits_ = 0;
    int crits_ = 0;
    int64_t startQpc_ = 0;
    int64_t cutoffQpc_ = 0;
    int64_t startedAtMs_ = 0;
};

}  // namespace dps::meter
