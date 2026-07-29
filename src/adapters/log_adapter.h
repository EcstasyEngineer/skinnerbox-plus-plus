// SkinnerBox++ — JSONL session log adapter.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <cstdio>
#include <ctime>
#include <string>

#include "../core/adapter.h"

namespace sbpp {

// Metadata-only recorder: behavioral features, regimes, and reward events.
// Never any document text. One JSONL file per session; this is both the
// Phase-1 telemetry record and the counterfactual (withheld-reward) dataset.
class LogAdapter : public IOutputAdapter {
public:
    // path: full path of the session .jsonl file to create/append.
    explicit LogAdapter(const std::wstring& path);
    ~LogAdapter() override;

    // Record the session's experiment arm at the top of the log.
    void note_session_arm(bool block_mode, bool withheld);

    const char* name() const override { return "log"; }
    void ambient(const AmbientState& state) override;
    void deliver(const RewardIntent& intent) override;
    void shutdown() override;

private:
    void write_line(const std::string& json);
    static std::string timestamp();

    FILE* file_ = nullptr;
    int ambient_downsample_ = 0;
    Regime last_regime_ = Regime::Drafting;
    bool regime_logged_once_ = false;
    // Pause dataset: entry time of the current PAUSED/STALL episode, if any.
    std::time_t pause_started_ = 0;
    Regime pause_kind_ = Regime::Drafting;
};

} // namespace sbpp
