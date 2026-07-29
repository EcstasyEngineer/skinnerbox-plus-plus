// SkinnerBox++ — engine configuration.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <cstdint>

namespace sbpp {

// All tunables in one place. Loaded from the plugin's INI; defaults here are
// the shipped baseline. Every knob serves the one loop:
// measure (momentum + content entropy) -> FLOW -> variable-interval reward.
struct Config {
    // --- momentum measurement ---
    double target_net_cpm = 120.0;   // net chars/min that counts as "full" momentum
    double ewma_alpha = 0.06;        // per-tick smoothing (1 Hz ticks, ~20 s constant)
    double burst_gap_seconds = 3.0;  // a pause longer than this ends a burst
    double grace_seconds = 9.0;      // in-focus pause this short keeps burst
                                     // continuity; beyond it the raw score starves
    double idle_seconds = 30.0;      // idle beyond this is the IDLE state

    // --- FSM thresholds (hysteresis on the FLOW boundary) ---
    double flow_enter = 0.70;
    double flow_exit = 0.50;

    // --- content gate (must-pass conjunction on FLOW and on every reward) ---
    double slop_repetition_max = 0.55; // repeated-token mass above this fails
    double slop_entropy_min = 3.4;     // char entropy (bits) below this fails
    double slop_stall_frac_max = 0.06; // "uuuh"-class token share above fails
    double slop_bigram_bpc_max = 4.20; // English char-bigram cost (bits/char)
                                       // above this fails: the mash detector.
                                       // Experiment 02: real typed windows
                                       // mean 3.39 / p99 3.70; mash 7.37,
                                       // "uuuh" filler 5.30. 4.20 clears all
                                       // observed real text with margin.
    double gate_min_chars = 80.0;      // less recent text than this = no
                                       // content evidence = gate fails closed

    // --- reward policy (variable interval, state-gated) ---
    double min_flow_hold_s = 30.0;         // must hold FLOW this long to qualify
    double mean_reward_interval_s = 120.0; // VI hazard mean once qualified
    double min_cooldown_s = 40.0;          // hard floor between deliveries —
                                           // kept well under the VI mean so it
                                           // doesn't clip the hazard's left
                                           // tail into a clockable schedule

    // --- in-editor channels ---
    bool visual_enabled = true;
    bool statusbar_enabled = true;
    uint32_t bloom_ms = 8000;      // phasic tint duration
    double bloom_lift = 0.12;      // added tint above tonic, NOT a jump-to
    uint32_t message_ms = 25000;   // status-bar reward message dwell

    // --- Intiface / Buttplug v4 output (the hardware reward channel) ---
    // Buttplug has NO server-side intensity cap and NO per-command duration:
    // a value persists until changed, so the client-side ceiling here is the
    // only cap, and this plugin owns the stop. The protocol's ping timeout is
    // the dead-man's switch — if we stop pinging, the server stops devices.
    bool intiface_enabled = true;
    double intiface_max_intensity = 0.30; // fraction of the device's own range
    uint32_t intiface_ms = 2200;          // base sustain; actual sustain is
                                          // base*(0.5+dose) (capped 4 s) plus
                                          // ~0.5 s of ease-in/out ramps —
                                          // reward magnitude is carried by
                                          // DURATION at fixed peak

    // --- debug telemetry (default-off, one switch): per-event raw log
    // INCLUDING typed text, for offline experiments (this is where the
    // salience/quality corpora come from). Status bar shows REC while on. ---
    bool debug_telemetry = false;
};

} // namespace sbpp
