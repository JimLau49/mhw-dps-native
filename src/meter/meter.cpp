#include "meter/meter.h"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "game_data.h"
#include "hooks/game_hooks.h"
#include "hooks/hook_manager.h"
#include "log.h"
#include "meter/hit_tracker.h"
#include "settings.h"

namespace dps::meter {
namespace {

constexpr double kPollInterval = 0.1;
constexpr int kTrainingStage = 504;
constexpr int kGuidingLandsStage = 109;
constexpr const char* kTrainingHint = "Training  |  %s resets  |  %s time trial  |  DPS since first hit";
constexpr const char* kExpeditionHint = "Expedition  |  %s resets  |  DPS while fighting";
constexpr const char* kEstimateHint = "~ = monster HP lost minus yours";
// Expedition combat clock: a pause in the damage longer than this is not fighting.
constexpr double kCombatGap = 30.0;

// Quest names of expeditions (the Guiding Lands is matched by stage).
const char* const kExpeditionMarkers[] = {
    "Expedition", "Guiding Lands", "Expédition", "Expedición", "Spedizione", "Expedição", "探検", "探索",
    "導きの地",   "탐험",          "안내하는 땅", "Экспедиция", "探险", "调查地点",
};

std::string ResultFromQuestState(uint32_t state) {
    switch (state) {
        case game::kQuestSuccess:
        case game::kQuestCompleted:
            return "complete";
        case game::kQuestFailed:
            return "fail";
        case game::kQuestAbandon:
            return "abandon";
        default:
            return "leave";
    }
}

bool IsTerminal(uint32_t state) {
    return state == game::kQuestSuccess || state == game::kQuestCompleted || state == game::kQuestFailed ||
           state == game::kQuestAbandon || state == game::kQuestQuit;
}

}  // namespace

Meter& Instance() {
    static Meter meter;
    return meter;
}

// ---- snapshot helpers --------------------------------------------------------------

int Meter::Snapshot::Total() const {
    int total = 0;
    for (const auto& member : members)
        total += member.damage;
    return total;
}

int Meter::Snapshot::LocalSlot() const {
    for (const auto& member : members) {
        if (member.isLocal)
            return member.slot;
    }
    return -1;
}

std::array<int, kPartySlots> Meter::Snapshot::SlotDamage() const {
    std::array<int, kPartySlots> out{};
    for (const auto& member : members) {
        if (member.slot >= 0 && member.slot < kPartySlots)
            out[member.slot] = member.damage;
    }
    return out;
}

Meter::Snapshot Meter::LocalOnly(int damage, const std::string& name, uintptr_t instance) {
    Snapshot snapshot;
    Member member;
    member.slot = 0;
    member.name = name.empty() ? "You" : name;
    member.damage = std::max(0, damage);
    member.isLocal = true;
    member.instance = instance;
    snapshot.members.push_back(member);
    return snapshot;
}

bool Meter::IsHuntingStage(int stage) {
    switch (stage) {
        case 301: case 302: case 303:  // Astera, gathering hub, research base
        case 305: case 306:            // Seliana, gathering hub
        case 407:                      // character creation
        case 501: case 502: case 503:  // quarters
        case 504: case 505: case 506:  // training area, chamber of five, Seliana room
        case 100:                      // InfinityOfNothingHandler
            return false;
        default:
            return stage > 0;
    }
}

bool Meter::IsExpedition(int stage, const std::string& questName) {
    if (stage == kGuidingLandsStage)
        return true;
    for (const char* marker : kExpeditionMarkers) {
        if (ContainsInsensitive(questName, marker))
            return true;
    }
    return false;
}

// ---- lifecycle -------------------------------------------------------------------------

void Meter::Init(const mem::ModuleInfo& exe, AddressMap map, int gameBuild, bool buildConfirmed, std::string buildSource) {
    LockGuard guard(updateLock_);
    exe_ = exe;
    map_ = std::move(map);
    gameBuild_ = gameBuild;
    buildConfirmed_ = buildConfirmed;
    buildSource_ = std::move(buildSource);
    reader_.Init(exe_, map_);
    Hits().SetClassifier([this](uintptr_t target) {
        int type = -1;
        return reader_.IsLargeMonster(target, &type) && MonsterTracker::IsTrackedType(type);
    });
    lastUpdateQpc_ = QpcNow();
    ready_ = true;
    status_ = "loaded " + map_.source;
}

void Meter::Post(Command command) {
    LockGuard guard(commandLock_);
    if (commands_.size() < 16)
        commands_.push_back(command);
}

void Meter::SetRenderInfo(std::string info) {
    LockGuard guard(viewLock_);
    renderInfo_ = std::move(info);
}

void Meter::SetHookInfo(std::string info) {
    LockGuard guard(viewLock_);
    hookInfo_ = std::move(info);
}

void Meter::PublishForTest(OverlayView view) {
    LockGuard guard(viewLock_);
    overlay_ = std::move(view);
}

OverlayView Meter::Overlay() {
    LockGuard guard(viewLock_);
    return overlay_;
}

bool Meter::OverlayShown() {
    LockGuard guard(viewLock_);
    return overlay_.show || !overlay_.monsters.empty();
}

PanelView Meter::Panel() {
    LockGuard guard(viewLock_);
    return panel_;
}

std::string Meter::Status() {
    LockGuard guard(viewLock_);
    return publishedStatus_;
}

void Meter::Update() {
    if (!ready_)
        return;
    // The render thread takes over when the game-thread hook stalls; never run twice at once.
    if (!updateLock_.try_lock())
        return;
    try {
        const int64_t now = QpcNow();
        pollAccumulator_ += QpcToSeconds(now - lastUpdateQpc_);
        lastUpdateQpc_ = now;
        ProcessCommands();
        if (pollAccumulator_ >= kPollInterval) {
            pollAccumulator_ = 0.0;
            Poll();
            PublishViews();
        }
    } catch (const std::exception& error) {
        status_ = StrFormat("update failed (%s)", error.what());
        LOG_ERROR("update failed: %s", error.what());
    } catch (...) {
        status_ = "update failed";
        LOG_ERROR("update failed (unknown exception)");
    }
    updateLock_.unlock();
}

void Meter::ProcessCommands() {
    std::vector<Command> commands;
    {
        LockGuard guard(commandLock_);
        commands.swap(commands_);
    }
    for (Command command : commands) {
        switch (command) {
            case Command::Reset:
                ResetTraining();
                ResetExpedition();
                break;
            case Command::ToggleTrial: ToggleTrial(); break;
            case Command::DumpDiagnostics: DumpDiagnostics(); break;
        }
    }
}

void Meter::Poll() {
    localPlayer_ = reader_.LocalPlayer();
    hooks::SetLocalActionController(localPlayer_ ? localPlayer_ + game::off::kActionController : 0);

    monstersPolled_ = false;
    SyncQuestState();
    if (training_)
        PollTraining();
    else if (inQuest_ && expedition_)
        PollExpedition();
    else if (inQuest_)
        PollHunt();
    // Health bars also cover the results screen.
    if (!monstersPolled_ && !training_ && IsHuntingStage(stageId_))
        PollMonsters();
}

int Meter::PollMonsters() {
    monstersPolled_ = true;
    return monsters_.Poll(reader_);
}

// ---- state machine ------------------------------------------------------------------------

void Meter::SyncQuestState() {
    const game::QuestInfo quest = reader_.ReadQuest();
    const int stage = reader_.ReadStage();
    if (!quest.ok || stage < 0) {
        status_ = quest.ok ? "stage unreadable" : "quest data unreadable (main menu?)";
        return;
    }
    lastQuest_ = quest;
    questInstance_ = quest.instance;
    questState_ = quest.state;
    stageId_ = stage;

    if (quest.id <= 0 || quest.state == game::kQuestNone || quest.state == game::kQuestReady)
        finishedQuestId_ = 0;

    if (stage == kTrainingStage && !inQuest_) {
        if (showResults_) {
            showResults_ = false;
            snapshot_.reset();
        }
        BeginTraining();
        return;
    }
    if (training_)
        EndTraining();

    const bool hunting = quest.id > 0 && quest.state == game::kQuestInQuest && IsHuntingStage(stage);
    if (hunting) {
        showResults_ = false;
        BeginHunt(quest.id);
        return;
    }

    if (inQuest_) {
        // Still on the hunting map mid-quest (e.g. a state flicker): keep going.
        if (!IsTerminal(quest.state) && IsHuntingStage(stage) && quest.id > 0)
            return;
        EndHunt(ResultFromQuestState(quest.state));
        return;
    }

    if (showResults_ && !IsHuntingStage(stage)) {
        showResults_ = false;
        snapshot_.reset();
    }

    if (quest.id > 0 && quest.state == game::kQuestReady)
        status_ = StrFormat("quest %d accepted - depart to start the overlay", quest.id);
    else if (quest.id > 0)
        status_ = StrFormat("quest %d (%s)", quest.id, game::DescribeQuestState(quest.state));
    else
        status_ = "not in a hunt";
}

std::string Meter::QuestNameFor(int questId, uintptr_t questInstance) {
    std::string name;
    if (settings::Get().questNames) {
        // Only ever call the game's function on a verified sQuest instance.
        if (!verifiedQuest_ || verifiedQuest_ != questInstance)
            verifiedQuest_ = hooks::DtiName(questInstance) == "sQuest" ? questInstance : hooks::FindSingleton("sQuest");
        if (verifiedQuest_) {
            try {
                name = hooks::QuestName(verifiedQuest_, questId);
            } catch (...) {
                name.clear();
            }
        }
    }
    return name.empty() ? StrFormat("Quest %d", questId) : name;
}

void Meter::BeginHunt(int questId) {
    if (questId <= 0)
        return;
    if (inQuest_ && questId_ == questId)
        return;
    // Same quest that ended a moment ago: the post-quest state flicker, not a new hunt.
    if (questId == finishedQuestId_ && finishedAtQpc_ != 0 && SecondsSince(finishedAtQpc_) < 180.0)
        return;

    // Expeditions read as InQuest for their whole duration; inQuest_ makes this run once.
    const std::string questName = QuestNameFor(questId, questInstance_);
    if (IsExpedition(stageId_, questName)) {
        BeginExpedition(questId, questName);
        return;
    }

    inQuest_ = true;
    expedition_ = false;
    showResults_ = false;
    questId_ = questId;
    questName_ = questName;
    questStartedAtMs_ = UnixMillisNow();
    questTimerStart_ = QpcNow();
    elapsed_ = 0.0f;
    timerSource_ = "local";
    snapshot_ = LocalOnly(0, localName_.empty() ? "You" : localName_, localPlayer_);
    pollAccumulator_ = 0.0;
    monsters_.Reset();
    Hits().Reset();
    Hits().SetAcceptAllTargets(false);
    Hits().SetRecordHits(true);
    recorder_.Reset();
    carts_.Reset();
    lastDps_ = {};
    Logs().BeginHunt();
    status_ = StrFormat("in quest %d", questId);
    LOG_INFO("hunt started: quest %d \"%s\" on %s", questId, questName.c_str(), data::StageLabel(stageId_).c_str());
}

void Meter::BeginExpedition(int questId, const std::string& questName) {
    inQuest_ = true;
    expedition_ = true;
    showResults_ = false;
    questId_ = questId;
    questName_ = questName;
    elapsed_ = 0.0f;
    timerSource_ = "combat";
    snapshot_ = LocalOnly(0, localName_.empty() ? "You" : localName_, localPlayer_);
    pollAccumulator_ = 0.0;
    // Monsters may already be hurt (Guiding Lands, joining a friend): count from now.
    monsters_.Reset(true);
    Hits().Reset();
    Hits().SetAcceptAllTargets(false);
    Hits().SetRecordHits(true);  // drained every poll; feeds the monster candidates
    carts_.Reset();
    combatSeconds_ = 0.0f;
    lastActivityQpc_ = 0;
    lastActivityDamage_ = 0;
    status_ = "expedition: waiting for the first hit";
    LOG_INFO("expedition started: quest %d \"%s\" on %s", questId, questName.c_str(), data::StageLabel(stageId_).c_str());
}

void Meter::ResetExpedition() {
    if (!inQuest_ || !expedition_)
        return;
    monsters_.Rebase();
    Hits().Reset();
    combatSeconds_ = 0.0f;
    lastActivityQpc_ = 0;
    lastActivityDamage_ = 0;
    elapsed_ = 0.0f;
    snapshot_ = LocalOnly(0, localName_.empty() ? "You" : localName_, localPlayer_);
    status_ = "expedition: reset, waiting for the next hit";
}

void Meter::EndExpedition() {
    // No fight log: an expedition has no result, and its clock only counts fighting.
    // finishedQuestId_ stays put so the same expedition can be started again at once.
    LOG_INFO("expedition ended: quest %d, %d dmg by you in %.0fs of fighting, %d hits", questId_, Hits().LocalDamage(),
             combatSeconds_, Hits().Hits());
    inQuest_ = false;
    expedition_ = false;
    showResults_ = false;
    snapshot_.reset();
    elapsed_ = 0.0f;
    Hits().SetRecordHits(false);
    status_ = "left the expedition";
}

void Meter::EndHunt(const std::string& result) {
    if (!inQuest_)
        return;
    if (expedition_) {
        EndExpedition();
        return;
    }

    inQuest_ = false;
    finishedQuestId_ = questId_;
    finishedAtQpc_ = QpcNow();
    elapsed_ = std::max(elapsed_, HuntElapsed());

    std::optional<Snapshot> final;
    try {
        final = ReadPartyWithFallbacks();
        if (final) {
            snapshot_ = final;
            recorder_.ObserveParty(elapsed_, [&] {
                std::vector<std::pair<int, std::string>> roster;
                for (const auto& m : final->members)
                    roster.emplace_back(m.slot, m.name);
                return roster;
            }());
            recorder_.ObserveWeapon(elapsed_, reader_.LocalWeapon(), final->LocalSlot());
        }
        recorder_.ObserveMonsters(elapsed_, monsters_.LastTracked());
        recorder_.AddHits(elapsed_, Hits().Drain(), snapshot_ ? std::max(0, snapshot_->LocalSlot()) : 0);
        if (final)
            ObserveCarts(*final);
    } catch (...) {
        LOG_WARN("final read at quest end failed");
    }
    Hits().SetRecordHits(false);

    const std::vector<Member> members = snapshot_ ? snapshot_->members : std::vector<Member>{};
    int total = 0;
    for (const auto& member : members)
        total += member.damage;

    LOG_INFO("hunt ended: quest %d result %s after %.1fs (%s), total %d, %zu local hits", questId_, result.c_str(), elapsed_,
             timerSource_.c_str(), total, recorder_.HitCount());

    if (settings::Get().writeFightLogs && questId_ > 0 && !members.empty() && total > 0) {
        FightLog log;
        log.kind = "quest";
        log.gameBuild = gameBuild_;
        log.questId = questId_;
        log.questName = questName_;
        log.result = result;
        log.stageId = stageId_;
        log.stage = data::StageLabel(stageId_);
        log.startedAtMs = questStartedAtMs_;
        log.endedAtMs = UnixMillisNow();
        log.durationSeconds = elapsed_;
        log.timerSource = timerSource_;
        log.hitCoverage = "local";
        if (lastQuest_.ok) {
            log.rewards.present = true;
            log.rewards.zenny = static_cast<int>(lastQuest_.rewardMoney);
            log.rewards.hunterRankPoints = static_cast<int>(lastQuest_.rewardHrp);
            log.rewards.stars = lastQuest_.stars;
        }
        const float duration = std::max(elapsed_, 0.001f);
        std::vector<Member> sorted = members;
        std::stable_sort(sorted.begin(), sorted.end(), [](const Member& a, const Member& b) {
            return a.damage != b.damage ? a.damage > b.damage : a.slot < b.slot;
        });
        for (const auto& member : sorted) {
            LogPlayer player;
            player.slot = member.slot;
            player.name = member.name;
            player.isLocal = member.isLocal;
            player.weapon = recorder_.WeaponOf(member.slot);
            if (player.weapon.empty() && member.isLocal)
                player.weapon = recorder_.LocalWeapon();
            player.damage = member.damage;
            player.dps = static_cast<float>(member.damage) / duration;
            player.percent = 100.0f * static_cast<float>(member.damage) / static_cast<float>(total);
            player.carts = recorder_.CartsOf(member.slot);
            log.players.push_back(player);
        }
        log.monsters = recorder_.Monsters();
        log.samples = Logs().TakeSamples();
        log.hits = recorder_.Hits();
        log.events = recorder_.Events();
        Logs().SaveAsync(std::move(log));
    } else {
        Logs().TakeSamples();
    }

    showResults_ = !members.empty();
    if (!showResults_)
        snapshot_.reset();
}

void Meter::BeginTraining() {
    if (training_)
        return;
    training_ = true;
    inQuest_ = false;
    expedition_ = false;
    showResults_ = false;
    pollAccumulator_ = 0.0;
    monsters_.Reset();
    Hits().SetAcceptAllTargets(true);
    Hits().SetRecordHits(false);
    ResetTraining();
    LOG_INFO("entered training area; counting hooked hits");
}

void Meter::EndTraining() {
    if (!training_)
        return;
    training_ = false;
    trial_.Cancel();
    Hits().SetRecordHits(false);
    Hits().SetAcceptAllTargets(false);
    Hits().Reset();
    snapshot_.reset();
    elapsed_ = 0.0f;
    status_ = "left training area";
}

void Meter::ResetTraining() {
    if (!training_)
        return;
    trial_.Cancel();
    Hits().SetRecordHits(false);
    Hits().Reset();
    elapsed_ = 0.0f;
    snapshot_ = LocalOnly(0, localName_.empty() ? "You" : localName_, localPlayer_);
    status_ = "training area: reset, waiting for first hit";
}

void Meter::ToggleTrial() {
    if (!training_) {
        status_ = "time trial only works in the training area";
        return;
    }
    if (trial_.State() == TrialState::Armed || trial_.State() == TrialState::Running) {
        trial_.Cancel();
        Hits().SetRecordHits(false);
        status_ = "time trial cancelled";
        return;
    }
    if (!Hits().Hooked()) {
        status_ = "time trial needs the hit hook (address map mismatch)";
        return;
    }
    Hits().SetRecordHits(true);
    trial_.Arm(settings::Get().trialDurationSeconds);
    const char* weapon = data::WeaponKey(reader_.LocalWeapon());
    trial_.previousBest = Logs().BestTrial(weapon ? weapon : "", trial_.DurationSeconds());
    status_ = StrFormat("time trial %ds armed, waiting for first hit", trial_.DurationSeconds());
    LOG_INFO("time trial %ds armed", trial_.DurationSeconds());
}

void Meter::FinishTrial() {
    Hits().SetRecordHits(false);
    const auto best = Logs().BestTrial(trial_.Weapon(), trial_.DurationSeconds());
    trial_.previousBest = best;
    trial_.personalBest = !best || trial_.Damage() > *best;

    LOG_INFO("time trial %ds finished: %d dmg, %d hits%s", trial_.DurationSeconds(), trial_.Damage(), trial_.HitCount(),
             trial_.personalBest ? ", personal best" : "");
    if (trial_.Damage() <= 0 || !settings::Get().writeFightLogs)
        return;

    FightLog log = trial_.BuildLog(localName_.empty() ? "You" : localName_, stageId_, data::StageLabel(stageId_), gameBuild_);
    trial_.savedFileName = FightLogStore::FileNameFor(log);
    Logs().SaveAsync(std::move(log));
}

// ---- polling --------------------------------------------------------------------------------

Meter::Snapshot Meter::ReadParty(int fallbackLocalDamage) {
    const game::PartyRead read = reader_.ReadParty();
    lastParty_ = read;
    if (!read.localName.empty())
        localName_ = read.localName;

    Snapshot snapshot;
    if (read.partySize > 0) {
        for (int slot = 0; slot < kPartySlots; ++slot) {
            const auto& probe = read.slots[slot];
            const bool occupied = !probe.name.empty() || read.rawDamage[slot] > 0;
            if (!occupied)
                continue;
            Member member;
            member.slot = slot;
            member.name = probe.name.empty() ? StrFormat("Hunter %d", slot + 1) : probe.name;
            member.damage = read.rawDamage[slot];
            member.isLocal = !probe.name.empty() && !read.localName.empty() && probe.name == read.localName;
            member.instance = member.isLocal ? localPlayer_ : probe.memberPtr;
            snapshot.members.push_back(member);
        }
    }

    if (snapshot.members.empty()) {
        // Solo (no session party) or roster unreadable: show ourselves only.
        const int damage = read.rawDamage[0] > 0 ? read.rawDamage[0] : std::max(0, fallbackLocalDamage);
        snapshot = LocalOnly(damage, read.localName.empty() ? "You" : read.localName, localPlayer_);
    } else if (snapshot.LocalSlot() < 0 && snapshot.members.size() == 1) {
        // Name mismatch (encoding, or save name unreadable) while alone.
        snapshot.members[0].isLocal = true;
    }

    bool anyAward = false;
    for (int value : read.rawDamage)
        anyAward = anyAward || value > 0;
    snapshot.hasAwardTable = read.hasTable && anyAward;

    if (snapshot.hasAwardTable) {
        damageSource_ = "quest-award table";
        lastPartyError_.clear();
    } else if (snapshot.members.size() > 1 || read.partySize > 0) {
        damageSource_ = "party list";
        lastPartyError_ = read.hasTable ? "party found, award damage still 0"
                                        : StrFormat("party found, damage table unreadable (%s)", read.damageError.c_str());
    } else if (fallbackLocalDamage > 0) {
        damageSource_ = "monster HP";
        lastPartyError_ = "solo (party size 0)";
    } else {
        damageSource_ = "none";
        lastPartyError_ = read.partySize == 0 ? "solo (party size 0), no damage yet"
                                              : (read.damageError.empty() ? "damage table not allocated" : read.damageError);
    }
    return snapshot;
}

std::optional<Meter::Snapshot> Meter::ReadPartyWithFallbacks() {
    const int fallback = PollMonsters();
    Hits().SetLargeMonsters(monsters_.LiveInstances());
    lastFallbackDamage_ = fallback;

    Snapshot snapshot = ReadParty(fallback);
    if (snapshot.members.empty()) {
        if (snapshot_ && !snapshot_->members.empty())
            return std::nullopt;
        snapshot = LocalOnly(fallback, "You", localPlayer_);
    }
    const int localSlot = snapshot.LocalSlot();
    Hits().SetLocalSlot(localSlot < 0 ? 0 : localSlot);
    return MergeLiveDamage(snapshot, fallback);
}

Meter::Snapshot Meter::MergeLiveDamage(const Snapshot& snapshot, int fallbackLocalDamage) {
    // The award table wins once it has real totals; zero slots are still patched from
    // the hook. Otherwise the live fallbacks fill in: hooked hits per slot, and monster
    // HP lost when solo.
    const auto hooked = Hits().Snapshot();
    Snapshot merged = snapshot;
    if (snapshot.hasAwardTable && snapshot.Total() > 0) {
        for (auto& member : merged.members) {
            if (member.damage > 0 || member.slot < 0 || member.slot >= kPartySlots)
                continue;
            if (hooked[member.slot] > 0)
                member.damage = hooked[member.slot];
        }
        return merged;
    }

    const bool solo = merged.members.size() == 1;
    const int tableTotal = snapshot.Total();
    int hookedTotal = 0;
    for (int value : hooked)
        hookedTotal += value;
    for (auto& member : merged.members) {
        if (member.slot >= 0 && member.slot < kPartySlots)
            member.damage = std::max(member.damage, hooked[member.slot]);
        if (member.isLocal && solo)
            member.damage = std::max(member.damage, std::max(0, fallbackLocalDamage));
    }
    merged.hasAwardTable = false;
    if (!(tableTotal > 0 || merged.members.empty()))
        damageSource_ = hookedTotal > 0 ? "live hits" : solo ? "monster HP" : "party list";
    return merged;
}

float Meter::HuntElapsed() {
    const float local = static_cast<float>(SecondsSince(questTimerStart_));
    // The in-game quest timer is shared by every hunter (and right for SOS joins);
    // trust it only while it is at least as far along as our own clock.
    if (lastQuest_.ok && lastQuest_.timerElapsed >= 0.25f && lastQuest_.timerElapsed <= lastQuest_.timerMax &&
        lastQuest_.timerElapsed + 5.0f >= local) {
        timerSource_ = "quest";
        return lastQuest_.timerElapsed;
    }
    timerSource_ = "local";
    return local;
}

void Meter::PollHunt() {
    const auto snapshot = ReadPartyWithFallbacks();
    if (!snapshot)
        return;

    elapsed_ = HuntElapsed();
    snapshot_ = snapshot;
    status_ = lastPartyError_.empty() ? StrFormat("%zu hunters (%s)", snapshot->members.size(), damageSource_.c_str())
                                      : StrFormat("%zu hunters (%s)", snapshot->members.size(), lastPartyError_.c_str());

    std::vector<std::pair<int, std::string>> roster;
    for (const auto& member : snapshot->members)
        roster.emplace_back(member.slot, member.name);
    const int localSlot = snapshot->LocalSlot();

    recorder_.ObserveMonsters(elapsed_, monsters_.LastTracked());
    recorder_.ObserveParty(elapsed_, roster);
    recorder_.ObserveWeapon(elapsed_, reader_.LocalWeapon(), localSlot);
    std::vector<HitRecord> hits = Hits().Drain();
    if (!monsters_.ListAvailable()) {
        for (const auto& hit : hits)
            monsters_.AddCandidate(hit.target);
    }
    recorder_.AddHits(elapsed_, hits, std::max(0, localSlot));
    ObserveCarts(*snapshot);
    Logs().UpdateSamples(elapsed_, snapshot->SlotDamage());
}

void Meter::PollExpedition() {
    const int hpLost = PollMonsters();
    Hits().SetLargeMonsters(monsters_.LiveInstances());
    lastFallbackDamage_ = hpLost;

    // The roster still comes from the session party; no award table exists here, so
    // party damage is your hooked hits plus everyone else's share of the monster HP lost.
    ReadParty(0);
    const game::PartyRead& party = lastParty_;
    Member local;
    local.isLocal = true;
    local.instance = localPlayer_;
    local.name = localName_.empty() ? "You" : localName_;
    std::vector<int> others;
    bool localFound = false;
    for (int slot = 0; slot < kPartySlots; ++slot) {
        const std::string& name = party.slots[slot].name;
        if (name.empty())
            continue;
        if (!localFound && !party.localName.empty() && name == party.localName) {
            local.slot = slot;
            local.name = name;
            localFound = true;
        } else {
            others.push_back(slot);
        }
    }
    if (!localFound && !others.empty()) {
        // Save name unreadable: assume the first hunter listed is you.
        local.slot = others.front();
        local.name = party.slots[local.slot].name;
        others.erase(others.begin());
    }

    const bool hooked = Hits().Hooked();
    local.damage = hooked ? Hits().LocalDamage() : (others.empty() ? hpLost : 0);
    Snapshot snapshot;
    snapshot.members.push_back(local);
    if (!others.empty()) {
        Member rest;
        rest.slot = others.front();
        rest.name = "~" + (others.size() == 1 ? party.slots[rest.slot].name : std::string("Others"));
        rest.instance = others.size() == 1 ? party.slots[rest.slot].memberPtr : 0;
        rest.damage = std::max(0, hpLost - local.damage);
        snapshot.members.push_back(rest);
    }
    Hits().SetLocalSlot(local.slot);

    // Combat clock: runs between damage events (yours, or HP the monsters lose) that
    // are at most kCombatGap apart, so tracking, gathering and carts do not count.
    const int activity = local.damage + hpLost;
    if (activity > lastActivityDamage_) {
        const int64_t now = QpcNow();
        if (lastActivityQpc_ != 0) {
            const double gap = QpcToSeconds(now - lastActivityQpc_);
            if (gap <= kCombatGap)
                combatSeconds_ += static_cast<float>(gap);
        }
        lastActivityQpc_ = now;
        lastActivityDamage_ = activity;
    }
    elapsed_ = combatSeconds_;
    snapshot_ = snapshot;

    // Only the monster candidates need the hits (builds without a monster list).
    std::vector<HitRecord> hits = Hits().Drain();
    if (!monsters_.ListAvailable()) {
        for (const auto& hit : hits)
            monsters_.AddCandidate(hit.target);
    }

    damageSource_ = hooked ? (others.empty() ? "live hits" : "live hits + monster HP") : "monster HP";
    if (!hooked && !others.empty())
        status_ = "expedition: hit hook is off, only party damage from monster HP";
    else
        status_ = StrFormat("expedition: %s dmg by you in %.0fs of fighting%s", ThousandsSep(local.damage).c_str(),
                            combatSeconds_, others.empty() ? "" : StrFormat(", %zu other hunters", others.size()).c_str());
}

void Meter::PollTraining() {
    PollMonsters();
    if (trial_.Update(Hits().Drain(), reader_.LocalWeapon()))
        FinishTrial();

    // While a trial is armed/running/finished the overlay shows the trial window only.
    const int damage = trial_.Active() ? trial_.Damage() : Hits().LocalDamage();
    elapsed_ = trial_.Active() ? trial_.Elapsed() : static_cast<float>(Hits().SecondsSinceFirstHit());

    Snapshot snapshot = ReadParty(0);
    if (snapshot.members.empty())
        snapshot = LocalOnly(0, "You", localPlayer_);
    for (auto& member : snapshot.members)
        member.damage = member.isLocal ? damage : 0;
    snapshot.hasAwardTable = false;
    const int localSlot = snapshot.LocalSlot();
    Hits().SetLocalSlot(localSlot < 0 ? 0 : localSlot);
    snapshot_ = snapshot;

    damageSource_ = trial_.Active() ? "time trial" : "training hits";
    if (!Hits().Hooked()) {
        status_ = "training area: hit hook is off, no damage can be counted";
    } else {
        switch (trial_.State()) {
            case TrialState::Armed:
                status_ = StrFormat("time trial %ds armed, waiting for first hit", trial_.DurationSeconds());
                break;
            case TrialState::Running:
                status_ = StrFormat("time trial: %d dmg, %.1fs left", damage, trial_.Remaining());
                break;
            case TrialState::Finished:
                status_ = StrFormat("time trial done: %d dmg in %ds (%.1f DPS)", damage, trial_.DurationSeconds(), trial_.Dps());
                break;
            default:
                status_ = StrFormat("training area: %d dmg over %.0fs (%d hits)", damage, elapsed_, Hits().Hits());
                break;
        }
    }
}

void Meter::ObserveCarts(const Snapshot& snapshot) {
    std::string localName;
    for (const auto& member : snapshot.members) {
        if (member.isLocal)
            localName = member.name;
    }
    for (const auto& cart : carts_.Poll(reader_, localPlayer_, snapshot.LocalSlot(), localName)) {
        std::string name = cart.name;
        if (name.empty() && cart.slot >= 0) {
            for (const auto& member : snapshot.members) {
                if (member.slot == cart.slot)
                    name = member.name;
            }
        }
        recorder_.AddCart(elapsed_, cart.slot, name);
        LOG_INFO("cart %d/%d (slot %d %s)", cart.deaths, cart.maxDeaths, cart.slot, name.c_str());
    }
}

// ---- views ---------------------------------------------------------------------------------

void Meter::PublishViews() {
    const Settings s = settings::Get();
    OverlayView view;
    view.show = inQuest_ || showResults_ || training_;
    view.training = training_;
    view.expedition = inQuest_ && expedition_;
    view.elapsed = elapsed_;
    if (snapshot_) {
        const float duration = std::max(elapsed_, 1.0f);
        const int total = std::max(snapshot_->Total(), 0);
        view.total = total;
        for (const auto& member : snapshot_->members) {
            MemberView row;
            row.slot = member.slot;
            row.name = member.name;
            row.damage = member.damage;
            row.isLocal = member.isLocal;
            row.dps = static_cast<float>(member.damage) / duration;
            row.percent = total > 0 ? 100.0f * static_cast<float>(member.damage) / static_cast<float>(total) : 0.0f;
            view.members.push_back(row);
        }
        std::stable_sort(view.members.begin(), view.members.end(), [](const MemberView& a, const MemberView& b) {
            return a.damage != b.damage ? a.damage > b.damage : a.slot < b.slot;
        });
    }
    if (inQuest_ && !expedition_ && carts_.Known() && (carts_.MaxDeaths() > 0 || carts_.Deaths() > 0)) {
        view.showCarts = true;
        view.deaths = carts_.Deaths();
        view.maxDeaths = std::max(carts_.MaxDeaths(), carts_.Deaths());
    }
    if (view.expedition) {
        view.hint = StrFormat(kExpeditionHint, s.keyReset.text.c_str());
        if (snapshot_ && snapshot_->members.size() > 1)
            view.hint += std::string("\n") + kEstimateHint;
    }
    if (training_) {
        view.hint = StrFormat(kTrainingHint, s.keyReset.text.c_str(), s.keyTrial.text.c_str());
        if (trial_.Active()) {
            view.trialActive = true;
            TrialView& trial = view.trial;
            trial.state = trial_.State();
            trial.duration = trial_.DurationSeconds();
            trial.elapsed = trial_.Elapsed();
            trial.remaining = trial_.Remaining();
            trial.damage = trial_.Damage();
            trial.hits = trial_.HitCount();
            trial.crits = trial_.Crits();
            trial.dps = trial_.Dps();
            trial.previousBest = trial_.previousBest;
            trial.personalBest = trial_.personalBest;
            trial.savedFile = trial_.savedFileName;
            if (trial.state == TrialState::Finished)
                trial.topMoves = trial_.TopMoves(5);
        }
    }

    if (s.showMonsterHealth && monstersPolled_ && !training_) {
        for (const auto& monster : monsters_.LastTracked()) {
            if (monster.health > 0.0f && monster.maxHealth > 0.0f)
                view.monsters.push_back({monster.name, monster.health, monster.maxHealth});
        }
    }

    PanelView panel;
    panel.diagnostics = BuildDiagnostics();
    panel.training = training_;
    panel.expedition = inQuest_ && expedition_;
    panel.hitHook = Hits().Hooked();
    panel.trialState = trial_.State();

    LockGuard guard(viewLock_);
    overlay_ = std::move(view);
    panel_ = std::move(panel);
    publishedStatus_ = status_;
}

std::string Meter::BuildDiagnostics() {
    std::string slots;
    for (int slot = 0; slot < kPartySlots; ++slot) {
        const auto& probe = lastParty_.slots[slot];
        slots += StrFormat("  [%d] name='%s' dmg=%d ptr=%s hex=%s\n", slot, probe.name.c_str(), lastParty_.rawDamage[slot],
                           probe.ptrOk ? StrFormat("0x%llX", static_cast<unsigned long long>(probe.memberPtr)).c_str() : "no",
                           probe.nameHex.c_str());
    }
    std::string renderInfo, hookInfo;
    {
        LockGuard guard(viewLock_);
        renderInfo = renderInfo_;
        hookInfo = hookInfo_;
    }
    const auto hooked = Hits().Snapshot();
    return StrFormat(
               "Plugin build: native %s\n"
               "Status: %s\n"
               "Game build: %d (%s%s)  map: %s%s\n"
               "Quest: %s id %d state %s  timer %s %.1fs\n"
               "Stage: %s (%d)%s\n"
               "Damage source: %s\n"
               "Hit hook: %s calls=%d counted=%d ignored=%d local=%d last %s\n"
               "Part capture: %s; %s\n"
               "Recorder: %zu hits, weapon %s\n"
               "Monsters: %s\n"
               "Party size: %d  local name: %s\n"
               "Award table: %s (%s) base 0x%llX\n"
               "Party array: 0x%llX %s\n"
               "Slot damage: %d / %d / %d / %d   hook: %d / %d / %d / %d   HP fallback: %d\n"
               "Slots:\n%s"
               "Hooks: %s\n"
               "Render: %s\n"
               "Threads: game-thread update %s, guarded faults %llu\n"
               "Module: 0x%llX  data: %s",
               kBuildStamp, status_.c_str(), gameBuild_, buildSource_.c_str(), buildConfirmed_ ? "" : ", unconfirmed",
               map_.source.c_str(), map_.embedded ? "" : "  (NO BUILT-IN MAP)", inQuest_ ? (expedition_ ? "expedition" : "yes") : "no", questId_,
               game::DescribeQuestState(questState_), timerSource_.c_str(), elapsed_, data::StageLabel(stageId_).c_str(), stageId_,
               training_ ? "  training mode" : "", damageSource_.c_str(), Hits().Status().c_str(), Hits().Calls(), Hits().Hits(),
               Hits().Ignored(), Hits().LocalDamage(), Hits().LastHit().c_str(), hooks::PartHookStatus().c_str(),
               Hits().PartDiagnostics().c_str(), recorder_.HitCount(),
               recorder_.LocalWeapon().empty() ? "-" : recorder_.LocalWeapon().c_str(), monsters_.LastMonsters().c_str(),
               lastParty_.partySize, localName_.empty() ? "-" : localName_.c_str(), lastParty_.hasTable ? "allocated" : "none",
               lastParty_.damageError.c_str(), static_cast<unsigned long long>(lastParty_.damageBase),
               static_cast<unsigned long long>(lastParty_.partyArray), lastParty_.partyError.c_str(), lastParty_.rawDamage[0],
               lastParty_.rawDamage[1], lastParty_.rawDamage[2], lastParty_.rawDamage[3], hooked[0], hooked[1], hooked[2],
               hooked[3], lastFallbackDamage_, slots.c_str(), hookInfo.c_str(), renderInfo.c_str(),
               hooks::MainUpdateAlive() ? "active" : "inactive", static_cast<unsigned long long>(mem::FaultCount()),
               static_cast<unsigned long long>(exe_.base), WideToUtf8(DataDirectory()).c_str());
}

void Meter::DumpDiagnostics() {
    // Re-read the party even outside a quest so the roster can be checked from the hub.
    if (!inQuest_ && !training_) {
        try {
            ReadParty(0);
        } catch (...) {
        }
    }
    nlohmann::ordered_json json;
    json["at"] = IsoUtc(UnixMillisNow());
    json["plugin"] = std::string("native ") + kBuildStamp;
    json["gameBuild"] = gameBuild_;
    json["map"] = map_.source;
    json["status"] = status_;
    json["inQuest"] = inQuest_;
    json["expedition"] = inQuest_ && expedition_;
    json["training"] = training_;
    json["questId"] = questId_;
    json["questName"] = questName_;
    json["questState"] = questState_;
    json["stageId"] = stageId_;
    json["elapsed"] = elapsed_;
    json["timerSource"] = timerSource_;
    json["questTimer"] = {{"elapsed", lastQuest_.timerElapsed}, {"max", lastQuest_.timerMax}};
    json["localPlayer"] = StrFormat("0x%llX", static_cast<unsigned long long>(localPlayer_));
    json["localWeapon"] = reader_.LocalWeapon();
    json["damageSource"] = damageSource_;
    json["partySize"] = lastParty_.partySize;
    json["localName"] = lastParty_.localName;
    json["damageBase"] = StrFormat("0x%llX", static_cast<unsigned long long>(lastParty_.damageBase));
    json["damageError"] = lastParty_.damageError;
    json["hasAwardTable"] = lastParty_.hasTable;
    json["partyArray"] = StrFormat("0x%llX", static_cast<unsigned long long>(lastParty_.partyArray));
    json["partyError"] = lastParty_.partyError;
    nlohmann::ordered_json slots = nlohmann::ordered_json::array();
    for (int slot = 0; slot < kPartySlots; ++slot) {
        const auto& probe = lastParty_.slots[slot];
        slots.push_back({{"slot", slot},
                         {"name", probe.name},
                         {"nameHex", probe.nameHex},
                         {"memberPtr", StrFormat("0x%llX", static_cast<unsigned long long>(probe.memberPtr))},
                         {"rawDamage", lastParty_.rawDamage[slot]}});
    }
    json["slots"] = slots;
    json["hitHook"] = Hits().Status();
    json["hookCalls"] = Hits().Calls();
    json["hookHits"] = Hits().Hits();
    json["hookIgnored"] = Hits().Ignored();
    json["hookLocalDamage"] = Hits().LocalDamage();
    json["partCapture"] = Hits().PartDiagnostics();
    json["monsters"] = monsters_.LastMonsters();
    json["hooks"] = hooks::Summary();
    {
        LockGuard guard(viewLock_);
        json["render"] = renderInfo_;
    }
    nlohmann::ordered_json singletons = nlohmann::ordered_json::object();
    for (const auto& [name, instance] : hooks::ListSingletons())
        singletons[name] = StrFormat("0x%llX", static_cast<unsigned long long>(instance));
    json["singletons"] = singletons;
    json["diagnostics"] = BuildDiagnostics();

    const std::wstring dir = JoinPath(DataDirectory(), L"logs");
    EnsureDirectory(dir);
    const std::wstring path = JoinPath(dir, L"live-debug.json");
    const bool ok = WriteFileAtomic(path, json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    status_ = ok ? "diagnostics written to logs/live-debug.json" : "could not write live-debug.json";
    LOG_INFO("%s", status_.c_str());
    LOG_INFO("diagnostics:\n%s", BuildDiagnostics().c_str());
}

}  // namespace dps::meter
