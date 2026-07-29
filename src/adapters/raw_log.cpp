// SkinnerBox++ — opt-in raw telemetry log.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "raw_log.h"

#include <share.h>

namespace sbpp {

RawLog::RawLog(const std::wstring& path, bool capture_text)
    : capture_text_(capture_text) {
    file_ = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
}

RawLog::~RawLog() {
    if (file_) std::fclose(file_);
}

void RawLog::line(const std::string& json) {
    if (!file_) return;
    std::fwrite(json.data(), 1, json.size(), file_);
    std::fwrite("\n", 1, 1, file_);
    std::fflush(file_);
}

void RawLog::append_escaped(std::string& out, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c)); // UTF-8 passthrough
        }
    }
}

void RawLog::event(double t, const char* type, long long pos, long long len,
                   const char* text_utf8, size_t text_len) {
    if (!file_) return;
    char head[128];
    snprintf(head, sizeof(head), "{\"t\":%.2f,\"ev\":\"%s\",\"pos\":%lld,\"len\":%lld",
             t, type, pos, len);
    std::string out = head;
    if (capture_text_ && text_utf8 && text_len > 0) {
        out += ",\"text\":\"";
        append_escaped(out, text_utf8, text_len);
        out += "\"";
    }
    out += "}";
    line(out);
}

void RawLog::tick(double t, const AmbientState& s) {
    if (!file_) return;
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"t\":%.2f,\"ev\":\"tick\",\"flow\":%.4f,\"regime\":\"%s\","
             "\"cpm\":%.1f,\"del\":%.4f,\"burst\":%.1f,\"idle\":%.1f,\"focus\":%u,"
             "\"rep\":%.3f,\"ent\":%.2f,\"stall\":%.3f,\"bpc\":%.2f,\"gate\":%s}",
             t, s.flow, regime_name(s.regime), s.net_rate_cpm, s.deletion_ratio,
             s.burst_seconds, s.idle_seconds, s.focus_losses,
             s.repetition, s.entropy, s.stall_frac, s.bigram_bpc,
             s.gate_ok ? "true" : "false");
    line(buf);
}

} // namespace sbpp
