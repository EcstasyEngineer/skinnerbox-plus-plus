// SkinnerBox++ — telemetry aggregation, flow estimation, and the FSM.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "estimator.h"

#include <algorithm>
#include <cmath>

namespace sbpp {

namespace {
constexpr double kLongWindowS = 300.0;  // retention horizon for slices/focus
constexpr double kRateWindowS = 60.0;   // momentum window
constexpr double kRevisionWindowS = 120.0; // deletion-ratio window

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }
} // namespace

// Burst continuity survives pauses up to the grace window: reaching for a
// word is part of the hike, not the end of it.
void FlowEstimator::ingest_insert(double now_s, uint32_t chars) {
    slices_.push_back({now_s, chars, 0});
    if (last_activity_s_ < 0.0 || now_s - last_activity_s_ > cfg_.grace_seconds)
        burst_start_s_ = now_s;
    last_activity_s_ = now_s;
}

void FlowEstimator::ingest_delete(double now_s, uint32_t chars) {
    slices_.push_back({now_s, 0, chars});
    if (last_activity_s_ < 0.0 || now_s - last_activity_s_ > cfg_.grace_seconds)
        burst_start_s_ = now_s;
    last_activity_s_ = now_s;
}

void FlowEstimator::note_focus_loss(double now_s) {
    focus_losses_.push_back(now_s);
}

void FlowEstimator::trim(double now_s) {
    while (!slices_.empty() && now_s - slices_.front().t > kLongWindowS)
        slices_.pop_front();
    while (!focus_losses_.empty() && now_s - focus_losses_.front() > kLongWindowS)
        focus_losses_.pop_front();
}

double FlowEstimator::window_sum(double now_s, double window_s, bool inserts) const {
    double sum = 0.0;
    for (auto it = slices_.rbegin(); it != slices_.rend(); ++it) {
        if (now_s - it->t > window_s) break;
        sum += inserts ? it->ins : it->del;
    }
    return sum;
}

AmbientState FlowEstimator::tick(double now_s, bool focused, bool gate_ok) {
    trim(now_s);

    const double ins60 = window_sum(now_s, kRateWindowS, true);
    const double del60 = window_sum(now_s, kRateWindowS, false);
    const double ins120 = window_sum(now_s, kRevisionWindowS, true);
    const double del120 = window_sum(now_s, kRevisionWindowS, false);

    const double idle = (last_activity_s_ < 0.0) ? 1e9 : now_s - last_activity_s_;
    const bool in_burst = burst_start_s_ >= 0.0 &&
        idle <= (focused ? cfg_.grace_seconds : cfg_.burst_gap_seconds);
    const double burst =
        in_burst ? (now_s - burst_start_s_) - std::max(0.0, idle - cfg_.burst_gap_seconds)
                 : 0.0;

    const double net_cpm = std::max(0.0, ins60 - del60) * (60.0 / kRateWindowS);
    const double turnover = ins120 + del120;
    const double deletion_ratio = turnover > 0.0 ? del120 / turnover : 0.0;

    // Raw score: momentum and burst persistence carry it; destructive editing
    // and leaving the editor drag it down; idleness beyond grace starves it.
    double raw = 0.55 * clamp01(net_cpm / cfg_.target_net_cpm)
               + 0.45 * clamp01(burst / 60.0);
    raw -= 0.6 * std::max(0.0, deletion_ratio - 0.35);
    if (!focus_losses_.empty() && now_s - focus_losses_.back() < 60.0)
        raw -= 0.25;
    if (idle > (focused ? cfg_.grace_seconds : cfg_.burst_gap_seconds)) raw = 0.0;
    raw = clamp01(raw);

    flow_ = cfg_.ewma_alpha * raw + (1.0 - cfg_.ewma_alpha) * flow_;

    // The FSM. Whole thing. See reward_intent.h for the diagram.
    Regime regime;
    if (idle > cfg_.idle_seconds) {
        regime = Regime::Idle;
    } else {
        const double bar =
            state_.regime == Regime::Flow ? cfg_.flow_exit : cfg_.flow_enter;
        regime = (flow_ >= bar && gate_ok) ? Regime::Flow : Regime::Typing;
    }

    state_.flow = flow_;
    state_.regime = regime;
    state_.net_rate_cpm = net_cpm;
    state_.deletion_ratio = deletion_ratio;
    state_.burst_seconds = std::max(0.0, burst);
    state_.idle_seconds = std::min(idle, 1e6);
    state_.focus_losses = static_cast<uint32_t>(focus_losses_.size());
    state_.gate_ok = gate_ok;
    // repetition/entropy/stall_frac are filled by the engine.
    return state_;
}

} // namespace sbpp
