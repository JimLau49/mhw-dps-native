#include "meter/hit_tracker.h"

#include <algorithm>

namespace dps::meter {

HitTracker& Hits() {
    static HitTracker tracker;
    return tracker;
}

void HitTracker::OnDealDamage(uintptr_t target, int damage, bool tenderized, bool crit, int attackId, int part,
                              const char* partReason) {
    bool counted = false;
    Classifier classifier;
    {
        LockGuard guard(lock_);
        ++calls_;
        lastTarget_ = target;
        lastDamage_ = damage;
        if (damage <= 0 || damage > 100'000) {
            ++ignored_;
            return;
        }
        counted = acceptAll_ || std::binary_search(large_.begin(), large_.end(), target);
        if (!counted)
            classifier = classifier_;
    }

    // Not in the last poll's list (spawned since, or no monster list for this build):
    // classify outside the lock. The reader caches the verdict per instance and type.
    if (!counted)
        counted = classifier && classifier(target);

    LockGuard guard(lock_);
    if (!counted) {
        ++ignored_;
        return;
    }

    const int next = localDamage_ + damage;
    if (next >= 0 && next <= 50'000'000)
        localDamage_ = next;
    const int64_t now = QpcNow();
    if (hits_ == 0)
        firstHitQpc_ = now;
    ++hits_;

    if (recordHits_ && pending_.size() < kMaxPending) {
        if (part >= 0)
            ++partTagged_;
        else
            ++partUnknown_;
        partLast_ = partReason ? partReason : "none";
        HitRecord hit;
        hit.qpc = now;
        hit.target = target;
        hit.damage = damage;
        hit.crit = crit;
        hit.tenderized = tenderized;
        hit.attackId = attackId;
        hit.actionSet = actionSet_;
        hit.actionId = actionId_;
        hit.part = part;
        pending_.push_back(hit);
    }
}

bool HitTracker::WantsPart(uintptr_t target) {
    LockGuard guard(lock_);
    return recordHits_ && !acceptAll_ && std::binary_search(large_.begin(), large_.end(), target);
}

void HitTracker::SetCurrentAction(int actionSet, int actionId) {
    LockGuard guard(lock_);
    actionSet_ = actionSet;
    actionId_ = actionId;
}

void HitTracker::SetClassifier(Classifier classifier) {
    LockGuard guard(lock_);
    classifier_ = std::move(classifier);
}

void HitTracker::SetLargeMonsters(std::vector<uintptr_t> instances) {
    std::sort(instances.begin(), instances.end());
    instances.erase(std::unique(instances.begin(), instances.end()), instances.end());
    LockGuard guard(lock_);
    large_ = std::move(instances);
}

void HitTracker::SetLocalSlot(int slot) {
    LockGuard guard(lock_);
    localSlot_ = (slot >= 0 && slot < kPartySlots) ? slot : 0;
}

void HitTracker::SetAcceptAllTargets(bool value) {
    LockGuard guard(lock_);
    acceptAll_ = value;
}

void HitTracker::SetRecordHits(bool value) {
    LockGuard guard(lock_);
    recordHits_ = value;
    if (!value)
        pending_.clear();
}

std::vector<HitRecord> HitTracker::Drain() {
    LockGuard guard(lock_);
    std::vector<HitRecord> drained;
    drained.swap(pending_);
    return drained;
}

void HitTracker::Reset() {
    LockGuard guard(lock_);
    localDamage_ = 0;
    hits_ = 0;
    calls_ = 0;
    ignored_ = 0;
    partTagged_ = 0;
    partUnknown_ = 0;
    partLast_ = "none";
    lastTarget_ = 0;
    lastDamage_ = 0;
    firstHitQpc_ = 0;
    pending_.clear();
}

void HitTracker::SetHookStatus(bool hooked, std::string status) {
    LockGuard guard(lock_);
    hooked_ = hooked;
    status_ = std::move(status);
}

bool HitTracker::Hooked() {
    LockGuard guard(lock_);
    return hooked_;
}

std::string HitTracker::Status() {
    LockGuard guard(lock_);
    return status_;
}

int HitTracker::LocalDamage() {
    LockGuard guard(lock_);
    return localDamage_;
}

int HitTracker::Hits() {
    LockGuard guard(lock_);
    return hits_;
}

int HitTracker::Calls() {
    LockGuard guard(lock_);
    return calls_;
}

int HitTracker::Ignored() {
    LockGuard guard(lock_);
    return ignored_;
}

double HitTracker::SecondsSinceFirstHit() {
    LockGuard guard(lock_);
    return firstHitQpc_ == 0 ? 0.0 : SecondsSince(firstHitQpc_);
}

std::array<int, kPartySlots> HitTracker::Snapshot() {
    LockGuard guard(lock_);
    std::array<int, kPartySlots> out{};
    out[localSlot_] = localDamage_;
    return out;
}

std::string HitTracker::LastHit() {
    LockGuard guard(lock_);
    return StrFormat("target=0x%llX dmg=%d", static_cast<unsigned long long>(lastTarget_), lastDamage_);
}

std::string HitTracker::PartDiagnostics() {
    LockGuard guard(lock_);
    return StrFormat("tagged=%d unknown=%d last=%s context=%s", partTagged_, partUnknown_, partLast_.c_str(), partContext_.c_str());
}

void HitTracker::NotePartContext(const char* status) {
    LockGuard guard(lock_);
    partContext_ = status ? status : "none";
}

}  // namespace dps::meter
