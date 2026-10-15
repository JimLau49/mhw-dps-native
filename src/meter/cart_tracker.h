#pragma once

#include <string>
#include <vector>

#include "game/game_reader.h"

namespace dps::meter {

/// One cart detected from the quest death counter; slot -1 when unattributed.
struct CartRecord {
    int slot = -1;
    std::string name;
    int deaths = 0;
    int maxDeaths = 0;
};

/// Reads the party-wide quest cart counter (MaxDeaths + Deaths) and attributes an
/// increment to the local hunter when their HP hit zero just before it.
class CartTracker {
public:
    void Reset();
    std::vector<CartRecord> Poll(const game::GameReader& reader, uintptr_t localPlayer, int localSlot, const std::string& localName);

    int Deaths() const { return deaths_; }
    int MaxDeaths() const { return maxDeaths_; }
    bool Known() const { return known_; }
    const std::string& LastError() const { return lastError_; }

private:
    static constexpr double kWindowSeconds = 4.0;

    int previous_ = -1;
    int deaths_ = 0;
    int maxDeaths_ = 0;
    bool known_ = false;
    bool localWasAlive_ = true;
    int64_t localDiedAt_ = 0;
    std::string lastError_;
};

}  // namespace dps::meter
