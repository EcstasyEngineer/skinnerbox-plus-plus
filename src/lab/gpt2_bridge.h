// SkinnerBox++ — IGpt2Lab bridge over Gpt2LabClient.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include "../core/engine.h"
#include "gpt2_client.h"

namespace sbpp {

// Stride logic mirrors experiments/corpus.py: rescore after ~300 new typed
// chars, or on a slow timer if the window is already full enough. One score
// in flight at a time (client drops extras).
class Gpt2Bridge : public IGpt2Lab {
public:
    static constexpr uint32_t kStrideChars = 300;
    static constexpr uint32_t kMinChars = 80;
    static constexpr double kMinIntervalS = 5.0;

    explicit Gpt2Bridge(Gpt2LabClient& client) : client_(client) {}

    void on_typed_chars(uint32_t n) override { chars_since_score_ += n; }

    void maybe_request(double now_s, const std::string& window_text) override {
        if (!client_.host_ready()) return;
        if (window_text.size() < kMinChars) return;
        const bool stride_hit = chars_since_score_ >= kStrideChars;
        const bool timer_hit =
            last_request_s_ < 0.0 || (now_s - last_request_s_) >= kMinIntervalS;
        if (!stride_hit && !timer_hit) return;
        if (!stride_hit && chars_since_score_ == 0 && last_request_s_ >= 0.0)
            return;
        client_.request_score(window_text);
        chars_since_score_ = 0;
        last_request_s_ = now_s;
    }

    void poll_into(AmbientState& state) override {
        Gpt2Score sc;
        if (client_.take_result(sc) && sc.ok) {
            have_ = true;
            mean_ = sc.mean_bits;
            band_ = sc.band_dist;
            ms_ = sc.ms;
        }
        state.gpt2_ready = have_;
        state.gpt2_mean_bits = mean_;
        state.gpt2_band_dist = band_;
        state.gpt2_score_ms = ms_;
    }

private:
    Gpt2LabClient& client_;
    uint32_t chars_since_score_ = 0;
    double last_request_s_ = -1.0;
    bool have_ = false;
    double mean_ = 0.0;
    double band_ = 0.0;
    double ms_ = 0.0;
};

} // namespace sbpp
