// SkinnerBox++ — state-gated variable-interval reward policy.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "policy.h"

#include <algorithm>
#include <cmath>

namespace sbpp {

std::optional<RewardIntent> RewardPolicy::tick(double now_s, double dt_s,
                                               const AmbientState& state) {
    if (state.regime != Regime::Flow) {
        // Eligibility and the hold clock do not survive leaving FLOW.
        flow_entered_s_ = -1.0;
        eligible_ = false;
        return std::nullopt;
    }

    if (flow_entered_s_ < 0.0) flow_entered_s_ = now_s;
    const bool held = now_s - flow_entered_s_ >= cfg_.min_flow_hold_s;
    if (held && !eligible_) {
        // Exponential hazard: P(mature in dt) = 1 - exp(-dt/mean).
        std::uniform_real_distribution<double> u(0.0, 1.0);
        const double p = 1.0 - std::exp(-dt_s / cfg_.mean_reward_interval_s);
        if (u(rng_) < p) eligible_ = true;
    }

    // Fire only while keys are actively moving (idle < 1 s). The buzz must
    // land DURING the behavior it reinforces: delivering in a pause pairs the
    // reward with stopping — live test showed the writer instantly reading it
    // as "I stopped, so it paid," which conditions exactly the wrong thing.
    const bool typing_now = state.idle_seconds < 1.0;
    if (eligible_ && typing_now &&
        now_s - last_delivery_s_ >= cfg_.min_cooldown_s) {
        eligible_ = false;
        last_delivery_s_ = now_s;
        RewardIntent intent;
        intent.kind = RewardClass::MicroReward;
        intent.confidence = state.flow;
        intent.dose = std::clamp(0.3 + 0.5 * state.flow, 0.0, 1.0);
        intent.max_duration_ms = cfg_.bloom_ms;
        intent.reason = "flow_vi_reward";
        return intent;
    }
    return std::nullopt;
}

} // namespace sbpp
