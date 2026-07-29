// SkinnerBox++ — engine: telemetry -> estimator -> policy -> adapters.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <memory>
#include <vector>

#include "adapter.h"
#include "config.h"
#include "estimator.h"
#include "policy.h"

namespace sbpp {

// The whole closed loop, editor-independent. The host (Notepad++ plugin today,
// anything else tomorrow) forwards raw editor events and a ~1 Hz tick; the
// engine fans resulting ambient state and reward intents out to its adapters.
class FlowEngine {
public:
    FlowEngine(const Config& cfg, uint64_t seed)
        : cfg_(cfg), estimator_(cfg_), policy_(cfg_, seed) {}

    void add_adapter(std::unique_ptr<IOutputAdapter> adapter) {
        adapters_.push_back(std::move(adapter));
    }

    void on_insert(double now_s, uint32_t chars) { estimator_.ingest_insert(now_s, chars); }
    void on_delete(double now_s, uint32_t chars) { estimator_.ingest_delete(now_s, chars); }
    void on_focus_loss(double now_s) { estimator_.note_focus_loss(now_s); }

    void tick(double now_s) {
        const double dt = last_tick_s_ < 0.0 ? 1.0 : now_s - last_tick_s_;
        last_tick_s_ = now_s;
        const AmbientState state = estimator_.tick(now_s);
        for (auto& a : adapters_) a->ambient(state);
        if (auto intent = policy_.tick(now_s, dt, state)) {
            for (auto& a : adapters_) a->deliver(*intent);
        }
    }

    void shutdown() {
        RewardIntent bye;
        bye.kind = RewardClass::SessionSummary;
        bye.confidence = 1.0;
        bye.dose = 0.0;
        bye.max_duration_ms = 0;
        bye.reason = "session_end";
        bye.withheld = false;
        for (auto& a : adapters_) a->deliver(bye);
        for (auto& a : adapters_) a->shutdown();
        adapters_.clear();
    }

    const AmbientState& state() const { return estimator_.state(); }

private:
    Config cfg_;
    FlowEstimator estimator_;
    RewardPolicy policy_;
    std::vector<std::unique_ptr<IOutputAdapter>> adapters_;
    double last_tick_s_ = -1.0;
};

} // namespace sbpp
