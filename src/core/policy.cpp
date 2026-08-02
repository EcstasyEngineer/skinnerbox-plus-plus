// SkinnerBox++ — state-gated variable-interval reward policy.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "policy.h"

#include <algorithm>
#include <cmath>

namespace sbpp {

std::optional<RewardIntent> RewardPolicy::tick(double now_s, double dt_s,
                                               const AmbientState& state) {
    // --- quality-tier bookkeeping (always runs, never returns) ---
    if (state.regime != Regime::Flow) {
        // Eligibility and the hold clock do not survive leaving FLOW.
        flow_entered_s_ = -1.0;
        eligible_ = false;
    } else {
        if (flow_entered_s_ < 0.0) flow_entered_s_ = now_s;
        const bool held = now_s - flow_entered_s_ >= cfg_.min_flow_hold_s;
        if (held && !eligible_) {
            // Exponential hazard: P(mature in dt) = 1 - exp(-dt/mean).
            // Guard mean so a zero/negative config cannot saturate the hazard.
            const double mean = std::max(cfg_.mean_reward_interval_s, 1.0);
            std::uniform_real_distribution<double> u(0.0, 1.0);
            const double p = 1.0 - std::exp(-dt_s / mean);
            if (u(rng_) < p) eligible_ = true;
        }
    }

    // Fire only while keys are actively moving (idle < 1 s). The reward must
    // land DURING the behavior it reinforces: delivering in a pause pairs the
    // reward with stopping — live test showed the writer instantly reading it
    // as "I stopped, so it paid," which conditions exactly the wrong thing.
    const bool typing_now = state.idle_seconds < 1.0;

    // --- quality tier ("red coin") — the potent one wins ties ---
    if (state.regime == Regime::Flow && cfg_.vi_reward_enabled && eligible_ &&
        typing_now && now_s - last_delivery_s_ >= cfg_.min_cooldown_s) {
        eligible_ = false;
        last_delivery_s_ = now_s;
        RewardIntent intent;
        intent.kind = RewardClass::MicroReward;
        intent.confidence = state.flow;
        intent.dose = std::clamp(0.3 + 0.5 * state.flow, 0.0, 1.0);
        // Abstract time ceiling for logging/contracts — NOT a visual or
        // hardware duration. Adapters map dose + their own channel config
        // (bloom_ms, buzz_ms) independently.
        intent.max_duration_ms =
            static_cast<uint32_t>(std::lround(1500.0 + 3500.0 * intent.dose));
        intent.reason = "flow_vi_reward";
        return intent;
    }

    // --- regularity tier ("yellow coin"): fixed-ratio on net typed chars,
    // gate-free, active whenever the writer is producing at all ---
    // Zero doubles as the unarmed sentinel: the watermark arms at the first
    // nonzero count, forfeiting at most one tick of chars once per session —
    // always in the under-reward direction, never a double-fire.
    if (yellow_marker_chars_ == 0) yellow_marker_chars_ = state.typed_chars_session;
    if (cfg_.coins_enabled && state.regime != Regime::Idle && typing_now &&
        cfg_.coin_yellow_interval_chars > 0 &&
        state.typed_chars_session >=
            yellow_marker_chars_ +
                static_cast<uint64_t>(
                    std::max(1.0, cfg_.coin_yellow_interval_chars))) {
        yellow_marker_chars_ = state.typed_chars_session;
        RewardIntent intent;
        intent.kind = RewardClass::RegularityCoin;
        intent.confidence = 1.0; // char counting is not an estimate
        intent.dose = 0.25;      // small on purpose: frequent + cheap
        intent.max_duration_ms = 1500;
        intent.reason = "typing_regularity";
        return intent;
    }
    return std::nullopt;
}

} // namespace sbpp
