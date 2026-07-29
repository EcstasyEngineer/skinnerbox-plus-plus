// SkinnerBox++ — core reward contract.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.
//
// The policy layer never speaks device language. It emits semantic intents;
// adapters map them to whatever a channel can safely do.

#pragma once

#include <cstdint>
#include <string>

namespace sbpp {

// Discrete (phasic) reward classes. Classes are stable identifiers: append,
// never rename.
enum class RewardClass {
    MicroReward,       // qualifying moment during sustained flow
};

struct RewardIntent {
    RewardClass kind;
    double confidence;      // 0-1, how sure the estimator is the state is real
    double dose;            // 0-1 abstract magnitude; NEVER a hardware amplitude
    uint32_t max_duration_ms; // upper bound for any time-based delivery
    std::string reason;     // machine-readable trigger tag, e.g. "flow_vi_reward"
};

// The whole machine is this finite state machine:
//
//                   activity              score>=enter && gate_ok
//         ┌──────┐ ────────► ┌────────┐ ─────────────────────► ┌──────┐
//         │ IDLE │           │ TYPING │                        │ FLOW │
//         └──────┘ ◄──────── └────────┘ ◄───────────────────── └──────┘
//                idle>idle_s        score<exit || !gate_ok
//
//   (IDLE is also reachable directly from FLOW on idle>idle_s.)
//
// IDLE   — no recent input. Nothing measured, nothing owed.
// TYPING — producing input, but momentum or content hasn't qualified yet.
// FLOW   — momentum above threshold (hysteresis: enter/exit) AND the content
//          gate passes. The only state in which rewards can mature and fire.
enum class Regime { Idle, Typing, Flow };

// Continuous (tonic) state broadcast on every engine tick. Adapters use this
// for slow ambient feedback; it is weather, not a payout.
struct AmbientState {
    double flow = 0.0;      // smoothed 0-1 flow estimate
    Regime regime = Regime::Idle;
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
    double bigram_bpc = 0.0;
    bool gate_ok = false;
};

inline const char* regime_name(Regime r) {
    switch (r) {
        case Regime::Idle:   return "IDLE";
        case Regime::Typing: return "TYPING";
        case Regime::Flow:   return "FLOW";
    }
    return "UNKNOWN";
}

inline const char* reward_class_name(RewardClass k) {
    switch (k) {
        case RewardClass::MicroReward:    return "micro_reward";
    }
    return "unknown";
}

} // namespace sbpp
