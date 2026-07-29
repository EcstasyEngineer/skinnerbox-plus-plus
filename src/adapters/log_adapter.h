// SkinnerBox++ — JSONL session log adapter.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <cstdio>
#include <string>

#include "../core/adapter.h"

namespace sbpp {

// Metadata-only recorder: behavioral features, regimes, and reward events.
// Never any document text. One JSONL file per session.
class LogAdapter : public IOutputAdapter {
public:
    // path: full path of the session .jsonl file to create/append.
    explicit LogAdapter(const std::wstring& path);
    ~LogAdapter() override;

    const char* name() const override { return "log"; }
    void ambient(const AmbientState& state) override;
    void deliver(const RewardIntent& intent) override;
    void shutdown() override;

private:
    void write_line(const std::string& json);
    static std::string timestamp();

    FILE* file_ = nullptr;
    int ambient_downsample_ = 0;
    Regime last_regime_ = Regime::Idle;
    bool regime_logged_once_ = false;
};

} // namespace sbpp
