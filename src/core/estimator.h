// SkinnerBox++ — telemetry aggregation, flow estimation, and the FSM.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <deque>
#include <cstdint>

#include "config.h"
#include "reward_intent.h"

namespace sbpp {

// Editor-independent. The host feeds raw modification events (ingest_*) as
// they happen and calls tick() at ~1 Hz with a monotonic timestamp; the
// estimator maintains rolling windows, the smoothed flow score, and the
// IDLE/TYPING/FLOW state machine (see reward_intent.h for the diagram).
class FlowEstimator {
public:
    explicit FlowEstimator(const Config& cfg) : cfg_(cfg) {}

    void ingest_insert(double now_s, uint32_t chars);
    void ingest_delete(double now_s, uint32_t chars);
    void note_focus_loss(double now_s);

    // Advance one tick. focused = editor currently holds foreground focus;
    // gate_ok = the content gate's verdict for the current text window (the
    // FSM requires it for FLOW, so momentum alone can never reach the
    // rewardable state).
    AmbientState tick(double now_s, bool focused, bool gate_ok);

    const AmbientState& state() const { return state_; }
    double last_activity() const { return last_activity_s_; }

private:
    struct Slice { double t; uint32_t ins; uint32_t del; };

    void trim(double now_s);
    double window_sum(double now_s, double window_s, bool inserts) const;

    const Config& cfg_;
    std::deque<Slice> slices_;        // per-event aggregates, trimmed to 300 s
    std::deque<double> focus_losses_; // timestamps, trimmed to 300 s
    AmbientState state_{};
    double flow_ = 0.0;               // EWMA accumulator
    double last_activity_s_ = -1.0;   // last insert/delete timestamp
    double burst_start_s_ = -1.0;     // start of current typing burst
};

} // namespace sbpp
