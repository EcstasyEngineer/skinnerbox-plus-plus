// SkinnerBox++ — core reward contract.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.
//
// The policy layer never speaks device language. It emits semantic intents;
// adapters map them to whatever a channel can safely do. This is the contract
// every output — in-editor visuals, audio, log files, and future MCP-connected
// hardware — implements against. See docs/output-contract.md for the wire form.

#pragma once

#include <cstdint>
#include <string>

namespace sbpp {

// Discrete (phasic) reward classes. Adapters decide what each means for their
// channel; classes are stable identifiers, so append, never rename.
enum class RewardClass {
    MicroReward,       // qualifying moment during sustained flow
    RecoveryReward,    // re-entered productive drafting after a stall
    SessionSummary,    // end-of-session close-out event
};

struct RewardIntent {
    RewardClass kind;
    double confidence;      // 0-1, how sure the estimator is the state is real
    double dose;            // 0-1 abstract magnitude; NEVER a hardware amplitude
    uint32_t max_duration_ms; // upper bound for any time-based delivery
    std::string reason;     // machine-readable trigger tag, e.g. "flow_vi_reward"
    bool withheld;          // true: policy qualified the moment but chose not to
                            // deliver (counterfactual sample) — log it, don't act
};

// Continuous (tonic) state broadcast on every engine tick. Adapters use this
// for slow ambient feedback; it is weather, not a payout.
// Paused = in-focus idle past the grace window but short of a stall: the
// environment goes neutral (not dimmed, not held) while the writer thinks.
enum class Regime { Drafting, Flow, Editing, Stall, Paused };

struct AmbientState {
    double flow = 0.0;      // smoothed 0-1 flow estimate
    Regime regime = Regime::Drafting;
    double net_rate_cpm = 0.0;   // net chars/minute over the short window
    double deletion_ratio = 0.0; // deleted / (inserted + deleted)
    double burst_seconds = 0.0;  // length of the current typing burst
    double idle_seconds = 0.0;   // time since the last document modification
    uint32_t focus_losses = 0;   // focus departures in the long window
    // Content facets over recently typed text (see core/content.h) and the
    // must-pass gate verdict computed from them. Rewards require gate_ok.
    double repetition = 0.0;
    double entropy = 0.0;
    double stall_frac = 0.0;
    bool gate_ok = false;
};

inline const char* regime_name(Regime r) {
    switch (r) {
        case Regime::Drafting: return "DRAFTING";
        case Regime::Flow:     return "FLOW";
        case Regime::Editing:  return "EDITING";
        case Regime::Stall:    return "STALL";
        case Regime::Paused:   return "PAUSED";
    }
    return "UNKNOWN";
}

inline const char* reward_class_name(RewardClass k) {
    switch (k) {
        case RewardClass::MicroReward:    return "micro_reward";
        case RewardClass::RecoveryReward: return "recovery_reward";
        case RewardClass::SessionSummary: return "session_summary";
    }
    return "unknown";
}

} // namespace sbpp
