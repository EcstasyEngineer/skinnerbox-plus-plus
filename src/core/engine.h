// SkinnerBox++ — engine: telemetry -> estimator -> policy -> adapters.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <memory>
#include <vector>

#include "adapter.h"
#include "config.h"
#include "content.h"
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

    // text may be null (counts-only host); when present it feeds the lexical
    // content facets that gate rewards.
    void on_insert(double now_s, uint32_t chars, const char* text = nullptr,
                   size_t text_len = 0) {
        estimator_.ingest_insert(now_s, chars);
        if (text) content_.add_text(text, text_len);
    }
    void on_delete(double now_s, uint32_t chars) { estimator_.ingest_delete(now_s, chars); }
    void on_focus_loss(double now_s) { estimator_.note_focus_loss(now_s); }

    void tick(double now_s, bool focused = true) {
        const double dt = last_tick_s_ < 0.0 ? 1.0 : now_s - last_tick_s_;
        last_tick_s_ = now_s;
        AmbientState state = estimator_.tick(now_s, focused);
        const ContentFacets cf = content_.facets();
        state.repetition = cf.repetition;
        state.entropy = cf.entropy;
        state.stall_frac = cf.stall_frac;
        // Must-pass conjunction: no content evidence means no reward (fails
        // closed), and any slop signal fails it. Momentum alone can never
        // qualify a reward again.
        state.gate_ok = cf.window_chars >= cfg_.gate_min_chars &&
                        cf.repetition <= cfg_.slop_repetition_max &&
                        cf.entropy >= cfg_.slop_entropy_min &&
                        cf.stall_frac <= cfg_.slop_stall_frac_max &&
                        cf.tail_stall_run < 6 &&
                        cf.tail_max_token < 3;
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
    ContentWindow content_;
    std::vector<std::unique_ptr<IOutputAdapter>> adapters_;
    double last_tick_s_ = -1.0;
};

} // namespace sbpp
