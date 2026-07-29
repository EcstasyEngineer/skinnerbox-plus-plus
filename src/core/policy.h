// SkinnerBox++ — state-gated variable-interval reward policy.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <optional>
#include <random>

#include "config.h"
#include "reward_intent.h"

namespace sbpp {

// Decides WHEN a reward happens, never what it physically is. One trigger:
// while the FSM is in FLOW (held for at least min_flow_hold_s), reward
// eligibility matures under an exponential hazard with the configured mean
// interval. When mature, the reward fires on the next tick where the writer
// is actively typing (idle < 1 s), subject to the hard cooldown — the
// reinforcer lands during the behavior, never in the pause after it.
// Leaving FLOW forfeits eligibility and the hold clock.
class RewardPolicy {
public:
    RewardPolicy(const Config& cfg, uint64_t seed) : cfg_(cfg), rng_(seed) {}

    // Called once per tick with the fresh ambient state. Returns an intent for
    // this tick, or nothing. dt_s is the seconds since the previous tick.
    std::optional<RewardIntent> tick(double now_s, double dt_s,
                                     const AmbientState& state);

    double last_delivery() const { return last_delivery_s_; }

private:
    const Config& cfg_;
    std::mt19937_64 rng_;
    double flow_entered_s_ = -1.0;  // when the current FLOW stretch began
    bool eligible_ = false;         // VI timer has matured, awaiting boundary
    double last_delivery_s_ = -1e9;
};

} // namespace sbpp
