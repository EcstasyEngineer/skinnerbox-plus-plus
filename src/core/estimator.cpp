// SkinnerBox++ — telemetry aggregation and flow estimation.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "estimator.h"

#include <algorithm>
#include <cmath>

namespace sbpp {

namespace {
constexpr double kLongWindowS = 300.0;  // retention horizon for slices/focus
constexpr double kRateWindowS = 60.0;   // momentum window
constexpr double kRevisionWindowS = 120.0; // deletion-ratio window
constexpr double kIdleZeroS = 10.0;     // idle beyond this starves the raw score

double clamp01(double v) { return std::max(0.0, std::min(1.0, v)); }
} // namespace

void FlowEstimator::ingest_insert(double now_s, uint32_t chars) {
    slices_.push_back({now_s, chars, 0});
    if (last_activity_s_ < 0.0 || now_s - last_activity_s_ > cfg_.burst_gap_seconds)
        burst_start_s_ = now_s;
    last_activity_s_ = now_s;
}

void FlowEstimator::ingest_delete(double now_s, uint32_t chars) {
    slices_.push_back({now_s, 0, chars});
    if (last_activity_s_ < 0.0 || now_s - last_activity_s_ > cfg_.burst_gap_seconds)
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

AmbientState FlowEstimator::tick(double now_s) {
    trim(now_s);

    const double ins60 = window_sum(now_s, kRateWindowS, true);
    const double del60 = window_sum(now_s, kRateWindowS, false);
    const double ins120 = window_sum(now_s, kRevisionWindowS, true);
    const double del120 = window_sum(now_s, kRevisionWindowS, false);

    const double idle = (last_activity_s_ < 0.0) ? 1e9 : now_s - last_activity_s_;
    const bool in_burst = idle <= cfg_.burst_gap_seconds && burst_start_s_ >= 0.0;
    const double burst = in_burst ? now_s - burst_start_s_ : 0.0;

    const double net_cpm = std::max(0.0, ins60 - del60) * (60.0 / kRateWindowS);
    const double turnover = ins120 + del120;
    const double deletion_ratio = turnover > 0.0 ? del120 / turnover : 0.0;

    // Raw score: momentum and burst persistence carry it; destructive editing
    // and leaving the editor drag it down; idleness starves it entirely.
    double raw = 0.55 * clamp01(net_cpm / cfg_.target_net_cpm)
               + 0.45 * clamp01(burst / 60.0);
    raw -= 0.6 * std::max(0.0, deletion_ratio - 0.35);
    if (!focus_losses_.empty() && now_s - focus_losses_.back() < 60.0)
        raw -= 0.25;
    if (idle > kIdleZeroS) raw = 0.0;
    raw = clamp01(raw);

    flow_ = cfg_.ewma_alpha * raw + (1.0 - cfg_.ewma_alpha) * flow_;

    // Regime with hysteresis on the FLOW boundary.
    Regime regime = state_.regime;
    if (idle > cfg_.stall_idle_seconds) {
        regime = Regime::Stall;
    } else if (regime == Regime::Flow) {
        if (flow_ < cfg_.flow_exit) regime = Regime::Drafting;
    } else if (flow_ >= cfg_.flow_enter) {
        regime = Regime::Flow;
    } else if (deletion_ratio > cfg_.editing_deletion_ratio && turnover > 40.0) {
        regime = Regime::Editing;
    } else {
        regime = Regime::Drafting;
    }

    state_ = AmbientState{
        flow_,
        regime,
        net_cpm,
        deletion_ratio,
        burst,
        std::min(idle, 1e6),
        static_cast<uint32_t>(focus_losses_.size()),
    };
    return state_;
}

} // namespace sbpp
