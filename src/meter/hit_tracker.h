#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common.h"

namespace dps::meter {

/// One hit seen by the deal-damage hook, stamped with QueryPerformanceCounter.
struct HitRecord {
    int64_t qpc = 0;
    uintptr_t target = 0;
    int damage = 0;
    bool crit = false;
    bool tenderized = false;
    int attackId = 0;
    int actionSet = 0;
    int actionId = 0;
    int part = -1;  // canonical part id, -1 when unknown
};

/// Local hunter's hits from FUN_DEAL_DAMAGE. The game only runs that function for
/// hits simulated on this client, so this is exact damage for you alone; party
/// damage comes from the quest-award table.
class HitTracker {
public:
    /// Decides whether a target the poll has not seen yet is a large monster.
    using Classifier = std::function<bool(uintptr_t target)>;

    // ---- hook side ----
    void OnDealDamage(uintptr_t target, int damage, bool tenderized, bool crit, int attackId, int part, const char* partReason);
    /// Part capture only runs for hunt hits on tracked large monsters.
    bool WantsPart(uintptr_t target);
    void SetCurrentAction(int actionSet, int actionId);

    // ---- poll side ----
    void SetClassifier(Classifier classifier);
    void SetLargeMonsters(std::vector<uintptr_t> instances);
    void SetLocalSlot(int slot);
    /// Training area: the pole and wagon are not large monsters, so count every hit.
    void SetAcceptAllTargets(bool value);
    /// Buffer individual hits for the fight log (quests and trials).
    void SetRecordHits(bool value);
    std::vector<HitRecord> Drain();
    void Reset();

    void SetHookStatus(bool hooked, std::string status);
    bool Hooked();
    std::string Status();
    int LocalDamage();
    int Hits();
    int Calls();
    int Ignored();
    /// Seconds since the first counted hit after the last reset; 0 if none yet.
    double SecondsSinceFirstHit();
    /// Per-slot damage from the hook. Only the local slot is ever non-zero.
    std::array<int, kPartySlots> Snapshot();
    std::string LastHit();
    std::string PartDiagnostics();
    void NotePartContext(const char* status);

private:
    static constexpr size_t kMaxPending = 4096;

    Lock lock_;
    Classifier classifier_;
    std::vector<uintptr_t> large_;  // sorted, from the last monster poll
    std::vector<HitRecord> pending_;
    bool hooked_ = false;
    std::string status_ = "off";
    bool acceptAll_ = false;
    bool recordHits_ = false;
    int localSlot_ = 0;
    int localDamage_ = 0;
    int hits_ = 0;
    int calls_ = 0;
    int ignored_ = 0;
    uintptr_t lastTarget_ = 0;
    int lastDamage_ = 0;
    int64_t firstHitQpc_ = 0;
    int actionSet_ = 0;
    int actionId_ = 0;
    int partTagged_ = 0;
    int partUnknown_ = 0;
    std::string partLast_ = "none";
    std::string partContext_ = "none";
};

HitTracker& Hits();

}  // namespace dps::meter
