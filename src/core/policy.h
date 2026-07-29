// SkinnerBox++ — state-gated variable-interval reward policy.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <optional>
#include <random>

#include "config.h"
#include "reward_intent.h"

namespace sbpp {

// Decides WHEN a reward happens, never what it physically is. Two triggers:
//
//  1. Flow variable-interval: while the regime is FLOW (held for at least
//     min_flow_hold_s), reward eligibility matures under an exponential hazard
//     with the configured mean interval. When mature, the reward fires at the
//     next burst boundary (a short natural pause), subject to the cooldown.
//  2. Stall recovery: leaving STALL and producing recovery_chars of forward
//     progress within recovery_window_s earns a RecoveryReward.
//
// A configurable fraction of qualifying moments is withheld (delivered with
// withheld=true) so the log records what happens when a qualifying moment
// goes unrewarded — counterfactual data for later policy learning.
class RewardPolicy {
public:
    RewardPolicy(const Config& cfg, uint64_t seed)
        : cfg_(cfg), rng_(seed) {}

    // Called once per tick with the fresh ambient state. Returns an intent for
    // this tick, or nothing. dt_s is the seconds since the previous tick.
    std::optional<RewardIntent> tick(double now_s, double dt_s,
                                     const AmbientState& state);

    double last_delivery() const { return last_delivery_s_; }

private:
    std::optional<RewardIntent> emit(double now_s, RewardClass kind,
                                     const char* reason, double confidence,
                                     double dose);

    const Config& cfg_;
    std::mt19937_64 rng_;
    double flow_entered_s_ = -1.0;    // when the current FLOW stretch began
    bool eligible_ = false;           // VI timer has matured, awaiting boundary
    double last_delivery_s_ = -1e9;   // last non-withheld delivery
    Regime prev_regime_ = Regime::Drafting;
    // Stall-recovery tracking: baseline of the monotonic insert counter at the
    // moment the stall ended, so only fresh characters count.
    double recovery_started_s_ = -1.0;
    uint64_t recovery_baseline_chars_ = 0;
};

} // namespace sbpp
