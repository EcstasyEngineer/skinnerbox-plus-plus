// SkinnerBox++ — JSONL session log adapter.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "log_adapter.h"

#include <share.h>
#include <ctime>

namespace sbpp {

namespace {
constexpr int kAmbientEveryNTicks = 30; // ~one feature snapshot per 30 s
}

LogAdapter::LogAdapter(const std::wstring& path) {
    // _SH_DENYNO: the live session log stays readable by other processes.
    file_ = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
    if (file_) write_line("{\"event\":\"session_start\",\"ts\":\"" + timestamp() + "\"}");
}

LogAdapter::~LogAdapter() { shutdown(); }

void LogAdapter::log_config(double generosity, double mean_s, double hold_s,
                            double strength) {
    if (!file_) return;
    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"config\",\"ts\":\"%s\",\"generosity\":%.2f,"
             "\"mean_reward_interval_s\":%.1f,\"min_flow_hold_s\":%.1f,"
             "\"strength\":%.2f}",
             timestamp().c_str(), generosity, mean_s, hold_s, strength);
    write_line(buf);
}

std::string LogAdapter::timestamp() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

void LogAdapter::write_line(const std::string& json) {
    if (!file_) return;
    std::fwrite(json.data(), 1, json.size(), file_);
    std::fwrite("\n", 1, 1, file_);
    std::fflush(file_);
}

void LogAdapter::ambient(const AmbientState& s) {
    if (!file_) return;
    // Regime transitions always log; full snapshots are downsampled.
    if (!regime_logged_once_ || s.regime != last_regime_) {
        regime_logged_once_ = true;
        last_regime_ = s.regime;
        // gate_fail rides along: a FLOW->TYPING drop at high flow is almost
        // always a gate blip, and the 30 s snapshots never see those.
        char buf[224];
        snprintf(buf, sizeof(buf),
                 "{\"event\":\"regime\",\"ts\":\"%s\",\"regime\":\"%s\","
                 "\"flow\":%.3f,\"gate_fail\":\"%s\"}",
                 timestamp().c_str(), regime_name(s.regime), s.flow,
                 s.gate_fail ? s.gate_fail : "");
        write_line(buf);
    }
    if (++ambient_downsample_ < kAmbientEveryNTicks) return;
    ambient_downsample_ = 0;
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"snapshot\",\"ts\":\"%s\",\"flow\":%.3f,\"regime\":\"%s\","
             "\"net_cpm\":%.1f,\"del_ratio\":%.3f,\"burst_s\":%.1f,"
             "\"idle_s\":%.1f,\"focus_losses\":%u,"
             "\"repetition\":%.3f,\"entropy\":%.2f,\"stall_frac\":%.3f,"
             "\"bigram_bpc\":%.2f,\"gate_ok\":%s,\"gate_fail\":\"%s\"}",
             timestamp().c_str(), s.flow, regime_name(s.regime), s.net_rate_cpm,
             s.deletion_ratio, s.burst_seconds, s.idle_seconds, s.focus_losses,
             s.repetition, s.entropy, s.stall_frac, s.bigram_bpc,
             s.gate_ok ? "true" : "false", s.gate_fail ? s.gate_fail : "");
    write_line(buf);
}

void LogAdapter::deliver(const RewardIntent& i) {
    if (!file_) return;
    char buf[288];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"reward\",\"ts\":\"%s\",\"kind\":\"%s\",\"reason\":\"%s\","
             "\"confidence\":%.3f,\"dose\":%.3f,\"max_duration_ms\":%u}",
             timestamp().c_str(), reward_class_name(i.kind), i.reason.c_str(),
             i.confidence, i.dose, i.max_duration_ms);
    write_line(buf);
}

void LogAdapter::shutdown() {
    if (file_) {
        write_line("{\"event\":\"session_end\",\"ts\":\"" + timestamp() + "\"}");
        std::fclose(file_);
        file_ = nullptr;
    }
}

} // namespace sbpp
