// SkinnerBox++ — opt-in raw telemetry log.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <cstdio>
#include <string>

#include "../core/reward_intent.h"

namespace sbpp {

// Fine-grained per-event/per-tick stream for offline analysis and estimator
// debugging: every user edit event and every 1 Hz engine tick, monotonic
// timestamps. With capture_text enabled it ALSO records the inserted/deleted
// text itself — that is document content. One INI switch arms the whole
// channel (debug_telemetry); capture_text is chosen by the host when the
// log is opened (plugin always enables it when debug is on).
class RawLog {
public:
    RawLog(const std::wstring& path, bool capture_text);
    ~RawLog();

    // type is "ins" or "del"; text may be null (or ignored without capture_text).
    void event(double t, const char* type, long long pos, long long len,
               const char* text_utf8, size_t text_len);
    void tick(double t, const AmbientState& s);

    // JSON string-escape helper (shared with the label writer in the host).
    static void append_escaped(std::string& out, const char* s, size_t n);

private:
    void line(const std::string& json);

    FILE* file_ = nullptr;
    bool capture_text_;
};

} // namespace sbpp
