// SkinnerBox++ — engine configuration.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <cstdint>

namespace sbpp {

// All tunables in one place. Loaded from the plugin's INI; defaults here are
// the shipped baseline and deliberately conservative.
struct Config {
    // --- flow estimation ---
    double target_net_cpm = 120.0;   // net chars/min that counts as "full" momentum
    double ewma_alpha = 0.06;        // per-tick smoothing (1 Hz ticks, ~20 s constant)
    double flow_enter = 0.70;        // hysteresis: enter FLOW at/above this
    double flow_exit = 0.50;         // ...leave FLOW below this
    double editing_deletion_ratio = 0.45; // above this the regime reads EDITING
    double stall_idle_seconds = 30.0;     // idle beyond this is a STALL
    double burst_gap_seconds = 3.0;  // policy boundary: short natural pause
    double grace_seconds = 9.0;      // in-focus idle grace: burst continuity
                                     // survives pauses shorter than this, and
                                     // score starvation starts only beyond it

    // --- reward policy ---
    double mean_reward_interval_s = 420.0; // variable-interval average (7 min)
    double min_cooldown_s = 240.0;         // hard floor between deliveries
    double min_flow_hold_s = 90.0;         // must hold FLOW this long to qualify
    double withhold_probability = 0.15;    // counterfactual no-reward fraction
    double recovery_chars = 120.0;   // chars within recovery window after stall...
    double recovery_window_s = 60.0; // ...that count as a STALL recovery

    // --- content gate (must-pass conjunction on reward eligibility) ---
    double slop_repetition_max = 0.55; // repeated-token mass above this fails
    double slop_entropy_min = 3.4;     // char entropy (bits) below this fails
    double slop_stall_frac_max = 0.06; // "uuuh"-class token share above fails
    double gate_min_chars = 80.0;      // less recent text than this = no
                                       // content evidence = gate fails closed

    // --- reward types (each independently switchable; a type that isn't
    // earning its place should be turned off, not tolerated) ---
    bool micro_reward_enabled = true;
    bool recovery_reward_enabled = true;

    // --- channels ---
    // Sound and sudden brightness changes are OUT by design: they interrupt
    // reading, which is the opposite of the goal. Reward legibility comes from
    // the status-bar message plus the slow color ramp.
    bool visual_enabled = true;
    bool audio_enabled = false;
    bool statusbar_enabled = true;
    bool statusbar_verbose = false; // facet sub-items; off = just the meter

    // --- MCP hardware output (default-off; must be armed per session) ---
    bool mcp_enabled = false;
    double mcp_max_intensity = 0.30; // client ceiling; backend caps too
    uint32_t mcp_max_seconds = 3;    // per-delivery auto-stop bound
    int mcp_channel = 0;

    // --- debug telemetry (default-off, one switch): per-event raw log
    // INCLUDING typed text and buffer switches — a keylogger, for offline
    // estimator debugging and reverse-engineering. Experiment tooling, not a
    // production feature; the status bar shows REC while it's on. ---
    bool debug_telemetry = false;

    // Phasic tint lift and how long the reward message stays legible.
    uint32_t bloom_ms = 8000;
    double bloom_lift = 0.12;      // added tint above tonic, NOT a jump-to
    uint32_t message_ms = 25000;   // status-bar reward message dwell
};

} // namespace sbpp
