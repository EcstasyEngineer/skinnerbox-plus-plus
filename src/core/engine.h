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

// The whole closed loop, editor-independent. The host (Notepad++ plugin,
// console demo, anything else) forwards raw editor events and a ~1 Hz tick;
// the engine fans resulting ambient state and reward intents out to its
// adapters.
class FlowEngine {
public:
    FlowEngine(const Config& cfg, uint64_t seed)
        : cfg_(cfg), estimator_(cfg_), policy_(cfg_, seed) {}

    void add_adapter(std::unique_ptr<IOutputAdapter> adapter) {
        adapters_.push_back(std::move(adapter));
    }

    // text may be null (counts-only host); when present it feeds the lexical
    // content facets that gate FLOW and rewards.
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
        // Must-pass conjunction: no content evidence means no reward (fails
        // closed), and any slop signal fails it. Momentum alone can never
        // reach FLOW. Evaluated as a first-fail chain so the failing facet
        // can be NAMED to the writer instead of an anonymous "gate x".
        const ContentFacets cf = content_.facets();
        const char* gate_fail =
            cf.window_chars < cfg_.gate_min_chars      ? "thin"    :
            cf.repetition   > cfg_.slop_repetition_max ? "repeats" :
            cf.entropy      < cfg_.slop_entropy_min    ? "flat"    :
            cf.stall_frac   > cfg_.slop_stall_frac_max ? "filler"  :
            cf.bigram_bpc   > cfg_.slop_bigram_bpc_max ? "mash"    :
            cf.tail_stall_run >= 6                     ? "drone"   :
            cf.tail_max_token >= 3                     ? "echo"    : "";
        const bool gate_ok = gate_fail[0] == '\0';
        AmbientState state = estimator_.tick(now_s, focused, gate_ok);
        state.repetition = cf.repetition;
        state.entropy = cf.entropy;
        state.stall_frac = cf.stall_frac;
        state.bigram_bpc = cf.bigram_bpc;
        state.gate_fail = gate_fail;
        for (auto& a : adapters_) a->ambient(state);
        if (auto intent = policy_.tick(now_s, dt, state)) {
            for (auto& a : adapters_) a->deliver(*intent);
        }
    }

    void shutdown() {
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
