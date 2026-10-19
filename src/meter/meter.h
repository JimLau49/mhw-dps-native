#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "address_map.h"
#include "common.h"
#include "game/game_reader.h"
#include "meter/cart_tracker.h"
#include "meter/fight_log_store.h"
#include "meter/monster_tracker.h"
#include "meter/recorder.h"
#include "meter/time_trial.h"

namespace dps::meter {

struct MemberView {
    int slot = 0;
    std::string name;
    int damage = 0;
    float dps = 0.0f;
    float percent = 0.0f;
    bool isLocal = false;
};

struct TrialView {
    TrialState state = TrialState::Idle;
    int duration = 60;
    float elapsed = 0.0f;
    float remaining = 0.0f;
    int damage = 0;
    int hits = 0;
    int crits = 0;
    float dps = 0.0f;
    std::optional<int> previousBest;
    bool personalBest = false;
    std::string savedFile;
    std::vector<TrialMove> topMoves;
};

struct MonsterBar {
    std::string name;
    float health = 0.0f;
    float maxHealth = 0.0f;
};

/// Everything the overlay draws; rebuilt by every poll and copied by the renderer.
struct OverlayView {
    bool show = false;  // in a quest or expedition, on the results screen, or in the training area
    bool training = false;
    bool expedition = false;  // expeditions and the Guiding Lands
    std::vector<MemberView> members;  // sorted by damage
    int total = 0;
    float elapsed = 0.0f;
    bool showCarts = false;
    int deaths = 0;
    int maxDeaths = 0;
    std::string hint;
    bool trialActive = false;
    TrialView trial;
    /// Large monsters alive on the current map (health bars), also on the results screen.
    std::vector<MonsterBar> monsters;
};

struct PanelView {
    std::string diagnostics;
    bool training = false;
    bool expedition = false;
    bool hitHook = false;
    TrialState trialState = TrialState::Idle;
};

/// Reset clears the training area or expedition damage.
enum class Command { Reset, ToggleTrial, DumpDiagnostics };

/// Hunt / expedition / training state machine (port of the C# Plugin): polls the game
/// at 10 Hz, merges award-table, hooked-hit and monster-HP damage, records fight logs.
class Meter {
public:
    void Init(const mem::ModuleInfo& exe, AddressMap map, int gameBuild, bool buildConfirmed, std::string buildSource);
    bool Ready() const { return ready_; }

    /// Called every frame on the game thread (or the render thread as a fallback).
    void Update();
    void Post(Command command);

    OverlayView Overlay();
    /// Cheap per-frame check: is there anything for the overlay to draw?
    bool OverlayShown();
    PanelView Panel();
    /// Short status for the F9 panel header.
    std::string Status();

    void SetRenderInfo(std::string info);
    void SetHookInfo(std::string info);
    /// Test harness: show a fixed overlay without a game.
    void PublishForTest(OverlayView view);
    game::GameReader& Reader() { return reader_; }

private:
    struct Member {
        int slot = 0;
        std::string name;
        int damage = 0;
        bool isLocal = false;
        uintptr_t instance = 0;
    };

    struct Snapshot {
        std::vector<Member> members;
        bool hasAwardTable = false;
        int Total() const;
        int LocalSlot() const;
        std::array<int, kPartySlots> SlotDamage() const;
    };

    // state machine
    void Poll();
    void SyncQuestState();
    void BeginHunt(int questId);
    void EndHunt(const std::string& result);
    void BeginExpedition(int questId, const std::string& questName);
    void EndExpedition();
    void BeginTraining();
    void EndTraining();
    void ResetTraining();
    void ResetExpedition();
    void ToggleTrial();
    void FinishTrial();
    void PollHunt();
    void PollExpedition();
    void PollTraining();
    int PollMonsters();
    void DumpDiagnostics();

    // readers
    std::optional<Snapshot> ReadPartyWithFallbacks();
    Snapshot ReadParty(int fallbackLocalDamage);
    Snapshot MergeLiveDamage(const Snapshot& snapshot, int fallbackLocalDamage);
    float HuntElapsed();
    void ObserveCarts(const Snapshot& snapshot);
    void ProcessCommands();
    void PublishViews();
    std::string BuildDiagnostics();
    std::string QuestNameFor(int questId, uintptr_t questInstance);
    static Snapshot LocalOnly(int damage, const std::string& name, uintptr_t instance);
    /// Field maps with large monsters, the Guiding Lands included.
    static bool IsHuntingStage(int stage);
    /// Expeditions and the Guiding Lands: no quest-award damage table and no time limit.
    static bool IsExpedition(int stage, const std::string& questName);

    bool ready_ = false;
    mem::ModuleInfo exe_;
    AddressMap map_;
    game::GameReader reader_;
    int gameBuild_ = 0;
    bool buildConfirmed_ = false;
    std::string buildSource_;

    Lock updateLock_;  // one Update at a time
    Lock viewLock_;
    Lock commandLock_;
    std::vector<Command> commands_;
    // Published copies for the render thread (viewLock_); everything else is
    // owned by the update thread.
    OverlayView overlay_;
    PanelView panel_;
    std::string publishedStatus_ = "starting";
    std::string renderInfo_ = "not hooked";
    std::string hookInfo_;

    MonsterTracker monsters_;
    CartTracker carts_;
    Recorder recorder_{[this](int set, int id) { return reader_.ActionName(localPlayer_, set, id); }};
    TimeTrial trial_{[this](int set, int id) { return reader_.ActionName(localPlayer_, set, id); }};

    std::optional<Snapshot> snapshot_;
    std::array<float, kPartySlots> lastDps_{};
    int64_t lastUpdateQpc_ = 0;
    double pollAccumulator_ = 0.0;
    bool monstersPolled_ = false;  // monsters_ read during the current poll
    bool inQuest_ = false;
    bool showResults_ = false;
    bool training_ = false;
    bool expedition_ = false;  // with inQuest_: an expedition or the Guiding Lands
    int questId_ = 0;
    std::string questName_;
    uint32_t questState_ = 0;
    int stageId_ = -1;
    uintptr_t questInstance_ = 0;
    uintptr_t verifiedQuest_ = 0;  // sQuest confirmed by its DTI name
    game::QuestInfo lastQuest_;
    int64_t questStartedAtMs_ = 0;
    int64_t questTimerStart_ = 0;  // QPC
    float elapsed_ = 0.0f;
    std::string timerSource_ = "local";
    int finishedQuestId_ = 0;
    int64_t finishedAtQpc_ = 0;
    // Expedition combat clock: gaps between damage longer than kCombatGap are not counted.
    float combatSeconds_ = 0.0f;
    int64_t lastActivityQpc_ = 0;
    int lastActivityDamage_ = 0;
    uintptr_t localPlayer_ = 0;
    std::string localName_;
    std::string status_ = "starting";
    std::string damageSource_ = "n/a";
    int lastFallbackDamage_ = 0;
    game::PartyRead lastParty_;
    std::string lastPartyError_;
};

Meter& Instance();

}  // namespace dps::meter
