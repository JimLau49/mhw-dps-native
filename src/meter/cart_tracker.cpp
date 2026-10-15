#include "meter/cart_tracker.h"

namespace dps::meter {

void CartTracker::Reset() {
    previous_ = -1;
    deaths_ = 0;
    maxDeaths_ = 0;
    known_ = false;
    localWasAlive_ = true;
    localDiedAt_ = 0;
    lastError_.clear();
}

std::vector<CartRecord> CartTracker::Poll(const game::GameReader& reader, uintptr_t localPlayer, int localSlot,
                                          const std::string& localName) {
    std::vector<CartRecord> carts;

    float health = 0.0f, maxHealth = 0.0f;
    if (localPlayer && reader.EntityHealth(localPlayer, health, maxHealth)) {
        if (health <= 0.5f) {
            if (localWasAlive_) {
                localWasAlive_ = false;
                localDiedAt_ = QpcNow();
            }
        } else {
            localWasAlive_ = true;
            localDiedAt_ = 0;
        }
    }

    int deaths = 0, maxDeaths = 0;
    if (!reader.ReadDeaths(deaths, maxDeaths)) {
        lastError_ = "quest death counter unreadable";
        return carts;
    }
    lastError_.clear();
    known_ = true;
    deaths_ = deaths;
    maxDeaths_ = maxDeaths;

    if (previous_ < 0) {
        // First read this hunt: baseline only (SOS join-in-progress).
        previous_ = deaths;
        return carts;
    }
    if (deaths <= previous_) {
        previous_ = deaths;
        return carts;
    }

    const int added = deaths - previous_;
    previous_ = deaths;
    for (int i = 0; i < added; ++i) {
        CartRecord cart;
        cart.deaths = deaths;
        cart.maxDeaths = maxDeaths;
        if (!localWasAlive_ && localSlot >= 0 && localDiedAt_ != 0 && SecondsSince(localDiedAt_) <= kWindowSeconds) {
            cart.slot = localSlot;
            cart.name = localName;
            localDiedAt_ = 0;  // one cart per death
        }
        carts.push_back(cart);
    }
    return carts;
}

}  // namespace dps::meter
