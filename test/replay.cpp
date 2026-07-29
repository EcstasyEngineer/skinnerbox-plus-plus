// SkinnerBox++ — offline replay harness / adversarial acceptance test.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.
//
// Replays a session-*.raw.jsonl (with capture_text) through the real engine
// and reports what the policy would have done. Acceptance criterion for the
// content gate: the deliberate-filler stretches of the recorded adversarial
// session must show gate_ok=false, and no reward may fire while gated.
//
// Build (from repo root, after build.bat):  see build.bat /test note.
// Usage: replay.exe <raw.jsonl>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

#include "../src/core/config.h"
#include "../src/core/engine.h"

namespace {

// Minimal field extractors for our own known-shape JSONL. Not a JSON parser.
bool get_num(const std::string& l, const char* key, double& out) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = l.find(k);
    if (p == std::string::npos) return false;
    out = std::atof(l.c_str() + p + k.size());
    return true;
}

bool get_str(const std::string& l, const char* key, std::string& out) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t p = l.find(k);
    if (p == std::string::npos) return false;
    std::string r;
    for (size_t i = p + k.size(); i < l.size(); ++i) {
        char c = l[i];
        if (c == '\\' && i + 1 < l.size()) {
            const char n = l[++i];
            if (n == 'u') { // \u00XX control escape
                if (i + 4 < l.size()) {
                    r.push_back(static_cast<char>(
                        std::strtol(l.substr(i + 3, 2).c_str(), nullptr, 16)));
                    i += 4;
                }
            } else {
                r.push_back(n);
            }
        } else if (c == '"') {
            break;
        } else {
            r.push_back(c);
        }
    }
    out = r;
    return true;
}

class CapturePolicyOutput : public sbpp::IOutputAdapter {
public:
    const char* name() const override { return "capture"; }
    void ambient(const sbpp::AmbientState& s) override {
        ticks++;
        if (!s.gate_ok) gated_ticks++;
        if (s.regime == sbpp::Regime::Flow && !s.gate_ok) flow_while_gated++;
    }
    void deliver(const sbpp::RewardIntent& i) override {
        printf("REWARD t+%us kind=%s reason=%s dose=%.2f\n", ticks,
               sbpp::reward_class_name(i.kind), i.reason.c_str(), i.dose);
        rewards++;
    }
    void shutdown() override {}
    unsigned ticks = 0, gated_ticks = 0, flow_while_gated = 0, rewards = 0;
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: replay <session.raw.jsonl>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    sbpp::Config cfg; // shipped defaults
    sbpp::FlowEngine engine(cfg, 12345);
    auto cap_owned = std::make_unique<CapturePolicyOutput>();
    CapturePolicyOutput* cap = cap_owned.get();
    engine.add_adapter(std::move(cap_owned));

    std::string line;
    while (std::getline(in, line)) {
        double t = 0;
        if (!get_num(line, "t", t)) continue;
        std::string ev;
        if (!get_str(line, "ev", ev)) continue;
        if (ev == "tick") {
            engine.tick(t, true);
        } else if (ev == "ins") {
            double len = 0;
            get_num(line, "len", len);
            std::string text;
            get_str(line, "text", text);
            const auto chars =
                static_cast<uint32_t>(len > 256 ? 256 : len);
            engine.on_insert(t, chars, text.empty() ? nullptr : text.c_str(),
                             text.size());
        } else if (ev == "del") {
            double len = 0;
            get_num(line, "len", len);
            engine.on_delete(t, static_cast<uint32_t>(len > 256 ? 256 : len));
        }
    }
    printf("\nticks=%u gated=%u (%.0f%%) flow_while_gated=%u rewards=%u\n",
           cap->ticks, cap->gated_ticks,
           cap->ticks ? 100.0 * cap->gated_ticks / cap->ticks : 0.0,
           cap->flow_while_gated, cap->rewards);
    return 0;
}
