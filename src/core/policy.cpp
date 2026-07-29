// SkinnerBox++ — state-gated variable-interval reward policy.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "policy.h"

#include <algorithm>
#include <cmath>

namespace sbpp {

std::optional<RewardIntent> RewardPolicy::emit(double now_s, RewardClass kind,
                                               const char* reason,
                                               double confidence, double dose) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const bool withheld = u(rng_) < cfg_.withhold_probability;
    if (!withheld) last_delivery_s_ = now_s;
    RewardIntent intent;
    intent.kind = kind;
    intent.confidence = confidence;
    intent.dose = std::clamp(dose, 0.0, 1.0);
    intent.max_duration_ms = cfg_.bloom_ms;
    intent.reason = reason;
    intent.withheld = withheld;
    return intent;
}

std::optional<RewardIntent> RewardPolicy::tick(double now_s, double dt_s,
                                               const AmbientState& state) {
    std::uniform_real_distribution<double> u(0.0, 1.0);

    // --- stall recovery tracking ---
    if (prev_regime_ == Regime::Stall && state.regime != Regime::Stall) {
        recovery_started_s_ = now_s;
        recovery_accum_chars_ = 0.0;
    }
    if (state.regime == Regime::Stall) recovery_started_s_ = -1.0;
    std::optional<RewardIntent> result;
    if (recovery_started_s_ >= 0.0) {
        if (now_s - recovery_started_s_ > cfg_.recovery_window_s) {
            recovery_started_s_ = -1.0; // window closed without qualifying
        } else {
            recovery_accum_chars_ += state.net_rate_cpm * dt_s / 60.0;
            if (recovery_accum_chars_ >= cfg_.recovery_chars &&
                state.gate_ok && // conjunction: recovery must be real content
                now_s - last_delivery_s_ >= cfg_.min_cooldown_s) {
                recovery_started_s_ = -1.0;
                result = emit(now_s, RewardClass::RecoveryReward,
                              "stall_recovery", 0.7,
                              0.3 + 0.4 * state.flow);
            }
        }
    }

    // --- flow variable-interval ---
    if (state.regime == Regime::Flow) {
        // A PAUSED interlude doesn't restart the hold clock.
        if (prev_regime_ != Regime::Flow && prev_regime_ != Regime::Paused)
            flow_entered_s_ = now_s;
        if (flow_entered_s_ < 0.0) flow_entered_s_ = now_s;
        // Conjunction: the hazard clock only advances while the content gate
        // passes — spoofed momentum accrues zero eligibility.
        const bool held = now_s - flow_entered_s_ >= cfg_.min_flow_hold_s;
        if (held && state.gate_ok && !eligible_) {
            // Exponential hazard: P(mature in dt) = 1 - exp(-dt/mean).
            const double p = 1.0 - std::exp(-dt_s / cfg_.mean_reward_interval_s);
            if (u(rng_) < p) eligible_ = true;
        }
        // Fire at a burst boundary: a short natural pause, not mid-keystroke.
        const bool at_boundary =
            state.idle_seconds >= 1.0 &&
            state.idle_seconds <= cfg_.burst_gap_seconds + 2.0;
        if (!result && eligible_ && at_boundary && state.gate_ok &&
            now_s - last_delivery_s_ >= cfg_.min_cooldown_s) {
            eligible_ = false;
            result = emit(now_s, RewardClass::MicroReward, "flow_vi_reward",
                          state.flow, 0.3 + 0.5 * state.flow);
        }
    } else if (state.regime == Regime::Paused) {
        // Neutral: nothing matures, nothing fires, nothing is lost beyond the
        // score's own honest decay. flow_entered_s_ is kept so a quick return
        // to FLOW doesn't restart the hold clock from zero.
        eligible_ = false;
    } else {
        flow_entered_s_ = -1.0;
        eligible_ = false; // eligibility does not survive leaving FLOW
    }

    prev_regime_ = state.regime;
    return result;
}

} // namespace sbpp
