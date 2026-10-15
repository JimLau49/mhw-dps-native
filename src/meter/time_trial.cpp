#include "meter/time_trial.h"

#include <algorithm>
#include <map>

#include "meter/fight_log_store.h"

namespace dps::meter {

float TimeTrial::Elapsed() const {
    switch (state_) {
        case TrialState::Running:
            return std::min(static_cast<float>(SecondsSince(startQpc_)), static_cast<float>(duration_));
        case TrialState::Finished:
            return static_cast<float>(duration_);
        default:
            return 0.0f;
    }
}

float TimeTrial::Remaining() const { return std::max(0.0f, static_cast<float>(duration_) - Elapsed()); }

float TimeTrial::Dps() const {
    if (state_ != TrialState::Running && state_ != TrialState::Finished)
        return 0.0f;
    return static_cast<float>(damage_) / std::max(Elapsed(), 1.0f);
}

void TimeTrial::Arm(int durationSeconds) {
    Clear();
    duration_ = std::clamp(durationSeconds, kMinSeconds, kMaxSeconds);
    state_ = TrialState::Armed;
}

void TimeTrial::Clear() {
    state_ = TrialState::Idle;
    damage_ = 0;
    hits_ = 0;
    crits_ = 0;
    startQpc_ = 0;
    cutoffQpc_ = 0;
    startedAtMs_ = 0;
    previousBest.reset();
    personalBest = false;
    savedFileName.clear();
    recorder_.Reset();
}

bool TimeTrial::Update(const std::vector<HitRecord>& hits, int weaponType) {
    if (state_ == TrialState::Idle || state_ == TrialState::Finished)
        return false;

    const int64_t now = QpcNow();
    // Resolve this batch's action ids against the current weapon, as hunt polls do.
    if (state_ == TrialState::Running)
        recorder_.ObserveWeapon(Elapsed(), weaponType, 0);
    for (const auto& hit : hits) {
        if (state_ == TrialState::Armed) {
            startQpc_ = hit.qpc;
            cutoffQpc_ = startQpc_ + SecondsToQpc(duration_);
            startedAtMs_ = UnixMillisNow() - static_cast<int64_t>(QpcToSeconds(now - hit.qpc) * 1000.0);
            state_ = TrialState::Running;
            recorder_.ObserveWeapon(0.0f, weaponType, 0);
        }
        if (hit.qpc > cutoffQpc_)
            continue;
        damage_ += hit.damage;
        ++hits_;
        if (hit.crit)
            ++crits_;
        recorder_.AddHitsRelativeTo(startQpc_, {hit}, 0);
    }

    if (state_ == TrialState::Running && now >= cutoffQpc_) {
        state_ = TrialState::Finished;
        return true;
    }
    return false;
}

std::vector<TrialMove> TimeTrial::TopMoves(size_t count) const {
    std::map<std::string, TrialMove> moves;
    for (const auto& hit : recorder_.Hits()) {
        const std::string key = hit.action.empty() ? StrFormat("action %d/%d", hit.actionSet, hit.actionId) : hit.action;
        TrialMove& move = moves[key];
        move.name = key;
        move.damage += hit.damage;
        ++move.hits;
        if (hit.crit)
            ++move.crits;
    }
    std::vector<TrialMove> out;
    for (auto& [name, move] : moves)
        out.push_back(move);
    std::sort(out.begin(), out.end(), [](const TrialMove& a, const TrialMove& b) { return a.damage > b.damage; });
    if (out.size() > count)
        out.resize(count);
    return out;
}

std::string TimeTrial::ShortMoveName(const std::string& name) {
    const size_t separator = name.rfind("::");
    return separator == std::string::npos ? name : name.substr(separator + 2);
}

FightLog TimeTrial::BuildLog(const std::string& hunterName, int stageId, const std::string& stageName, int gameBuild) const {
    FightLog log;
    log.kind = "trial";
    log.gameBuild = gameBuild;
    log.questId = 0;
    log.questName = StrFormat("Time trial %ds", duration_);
    log.result = "trial";
    log.stageId = stageId;
    log.stage = stageName;
    log.startedAtMs = startedAtMs_;
    log.endedAtMs = startedAtMs_ + static_cast<int64_t>(duration_) * 1000;
    log.durationSeconds = static_cast<float>(duration_);
    log.timerSource = "trial";
    log.hitCoverage = "local";

    LogPlayer player;
    player.slot = 0;
    player.name = hunterName;
    player.isLocal = true;
    player.weapon = recorder_.LocalWeapon();
    player.damage = damage_;
    player.dps = static_cast<float>(damage_) / static_cast<float>(duration_);
    player.percent = 100.0f;
    log.players.push_back(player);

    log.hits = recorder_.Hits();
    log.events = recorder_.Events();

    // Samples rebuilt from the hits so the viewers' charts work unchanged.
    size_t next = 0;
    int cumulative = 0;
    auto sampleAt = [&](float t) {
        while (next < log.hits.size() && log.hits[next].t <= t)
            cumulative += log.hits[next++].damage;
        LogSample sample;
        sample.t = t;
        sample.damage = {cumulative, 0, 0, 0};
        return sample;
    };
    log.samples.push_back(LogSample{});
    const float step = FightLogStore::kSampleInterval;
    for (float t = step; t <= static_cast<float>(duration_) + 0.001f; t += step)
        log.samples.push_back(sampleAt(t));
    if (log.samples.back().t < static_cast<float>(duration_) - 0.001f)
        log.samples.push_back(sampleAt(static_cast<float>(duration_)));
    return log;
}

}  // namespace dps::meter
