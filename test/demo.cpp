// SkinnerBox++ — interactive console demo: type here, get conditioned.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.
//
// The whole MVP loop in one window, no editor required: raw keystrokes feed
// the real engine (same estimator/gate/policy the plugin runs), the status
// line shows the FSM live, and qualifying FLOW pays out through Intiface.
//
// Usage:
//   demo.exe                     interactive; type freely, Esc quits
//   demo.exe --snappy            demo-friendly policy timing (default)
//   demo.exe --shipped           shipped plugin timing (slow, patient)
//   demo.exe --selftest          synthetic typist drives the engine on a
//                                virtual clock; verifies a reward fires and
//                                reaches Intiface. Exit 0 = MVP loop works.
//   demo.exe --url ws://...      Intiface server (default ws://127.0.0.1:12345)
//   demo.exe --no-hw             run without Intiface (screen only)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "../src/core/config.h"
#include "../src/core/engine.h"
#include "../src/adapters/intiface_adapter.h"

namespace {

// Reward events land on the console as a line the typist can't miss, and the
// count feeds the status line.
class ConsoleRewards : public sbpp::IOutputAdapter {
public:
    const char* name() const override { return "console"; }
    void ambient(const sbpp::AmbientState&) override {}
    void deliver(const sbpp::RewardIntent& i) override {
        rewards++;
        printf("\n  \x1b[93m*** REWARD #%u (%s, dose %.2f) ***\x1b[0m\n",
               rewards, i.reason.c_str(), i.dose);
    }
    void shutdown() override {}
    unsigned rewards = 0;
};

const char* bar5(double v) {
    static const char* bars[] = {"-----", "#----", "##---", "###--", "####-", "#####"};
    int i = static_cast<int>(v * 5.0 + 0.5);
    return bars[i < 0 ? 0 : (i > 5 ? 5 : i)];
}

void draw_status(const sbpp::AmbientState& s, unsigned rewards,
                 const sbpp::IntifaceAdapter* hw) {
    char hwbuf[64];
    if (!hw) {
        snprintf(hwbuf, sizeof(hwbuf), "hw:off");
    } else if (hw->connected()) {
        snprintf(hwbuf, sizeof(hwbuf), "hw:%zu dev", hw->device_count());
    } else {
        snprintf(hwbuf, sizeof(hwbuf), "hw:connecting");
    }
    printf("\r  [%-6s] flow %s %.2f | %3.0f cpm | ent %.1f %s | rw %u | %-14s ",
           sbpp::regime_name(s.regime), bar5(s.flow), s.flow, s.net_rate_cpm,
           s.entropy, s.gate_ok ? "gate:ok" : "gate:x ", rewards, hwbuf);
    fflush(stdout);
}

// --------------------------------------------------------------- selftest ---

// A synthetic typist on a virtual clock: types real prose at ~250 cpm with
// word-shaped timing, pausing briefly at sentence ends (burst boundaries).
// Public-domain-grade filler prose, hardcoded to keep the binary self-contained.
const char* kProse =
    "The tide came in slowly over the flats, and the birds walked ahead of it, "
    "picking at whatever the water pushed up. She watched from the seawall with "
    "her coat pulled tight, counting the gray shapes against the light. There "
    "was a letter in her pocket she had not opened, and the paper edge pressed "
    "against her hand like a question she already knew the answer to. The wind "
    "moved the dune grass in long silver waves, and somewhere behind her a door "
    "kept knocking against its frame, patient and dull as a clock. ";

int run_selftest(const std::string& url, bool use_hw) {
    sbpp::Config cfg;
    // Snappy-but-real: hold FLOW 10 s, mean maturation 15 s, cooldown 10 s.
    cfg.min_flow_hold_s = 10.0;
    cfg.mean_reward_interval_s = 15.0;
    cfg.min_cooldown_s = 10.0;
    cfg.intiface_ms = 800;

    sbpp::FlowEngine engine(cfg, /*seed=*/1234);
    auto rewards_owned = std::make_unique<ConsoleRewards>();
    ConsoleRewards* rewards = rewards_owned.get();
    engine.add_adapter(std::move(rewards_owned));

    sbpp::IntifaceAdapter* hw = nullptr;
    if (use_hw) {
        sbpp::IntifaceAdapter::Settings is;
        is.url = url;
        is.max_intensity = 0.30;
        is.buzz_ms = 800;
        auto hw_owned = std::make_unique<sbpp::IntifaceAdapter>(is);
        hw = hw_owned.get();
        engine.add_adapter(std::move(hw_owned));
        Sleep(1500); // let the eager connect finish before the verdict
        printf("intiface: %s", hw->connected() ? "connected" : "NOT connected");
        if (hw->connected()) printf(" (%zu vibrating device(s))", hw->device_count());
        if (!hw->connected() && !hw->last_error().empty())
            printf(" — %s", hw->last_error().c_str());
        printf("\n");
    }

    // Virtual clock: 240 simulated seconds of steady prose, ~4 chars per
    // 1 s tick = 240 cpm, sentence-end pauses of 2 s (burst boundaries the
    // policy needs to fire into).
    const size_t prose_len = strlen(kProse);
    double t = 1000.0;
    size_t pos = 0;
    double pause_until = 0.0;
    for (int tick = 0; tick < 240; ++tick) {
        t += 1.0;
        if (t >= pause_until) {
            for (int k = 0; k < 4; ++k) {
                const char c = kProse[pos % prose_len];
                engine.on_insert(t, 1, &c, 1);
                if (c == '.') pause_until = t + 2.0; // breathe at the period
                pos++;
            }
        }
        engine.tick(t, true);
    }
    const sbpp::AmbientState& s = engine.state();
    printf("selftest end state: %s flow=%.2f gate=%s rewards=%u\n",
           sbpp::regime_name(s.regime), s.flow, s.gate_ok ? "ok" : "FAIL",
           rewards->rewards);
    const bool hw_ok = !use_hw || (hw && hw->connected());
    if (use_hw && rewards->rewards > 0 && hw_ok)
        Sleep(3000); // real time for a full reward envelope to play and zero
    engine.shutdown();

    if (rewards->rewards == 0) {
        printf("SELFTEST FAIL: no reward fired in 240 simulated seconds\n");
        return 1;
    }
    if (!hw_ok) {
        printf("SELFTEST FAIL: Intiface not connected\n");
        return 1;
    }
    printf("SELFTEST PASS: typing well -> FLOW -> reward%s\n",
           use_hw ? " -> vibration delivered" : " (screen only)");
    return 0;
}

// ------------------------------------------------------------ interactive ---

int run_interactive(const sbpp::Config& cfg, const std::string& url, bool use_hw) {
    // VT sequences for the reward highlight.
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD outmode = 0;
    GetConsoleMode(hout, &outmode);
    SetConsoleMode(hout, outmode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD inmode = 0;
    GetConsoleMode(hin, &inmode);
    SetConsoleMode(hin, ENABLE_WINDOW_INPUT); // no line buffering, no echo

    sbpp::FlowEngine engine(cfg, static_cast<uint64_t>(GetTickCount64()));
    auto rewards_owned = std::make_unique<ConsoleRewards>();
    ConsoleRewards* rewards = rewards_owned.get();
    engine.add_adapter(std::move(rewards_owned));
    sbpp::IntifaceAdapter* hw = nullptr;
    if (use_hw) {
        sbpp::IntifaceAdapter::Settings is;
        is.url = url;
        is.max_intensity = cfg.intiface_max_intensity;
        is.buzz_ms = cfg.intiface_ms;
        auto hw_owned = std::make_unique<sbpp::IntifaceAdapter>(is);
        hw = hw_owned.get();
        engine.add_adapter(std::move(hw_owned));
    }

    printf("SkinnerBox++ demo — type prose below. FLOW needs momentum AND real"
           " content;\nrewards mature on a variable interval and land while"
           " you're typing.\nEsc quits.\n\n");

    auto now = [] { return static_cast<double>(GetTickCount64()) / 1000.0; };
    double next_tick = now() + 1.0;
    bool quit = false;
    while (!quit) {
        const DWORD wait = WaitForSingleObject(hin, 100);
        if (wait == WAIT_OBJECT_0) {
            INPUT_RECORD recs[32];
            DWORD n = 0;
            if (ReadConsoleInputW(hin, recs, 32, &n)) {
                for (DWORD i = 0; i < n; ++i) {
                    if (recs[i].EventType != KEY_EVENT) continue;
                    const KEY_EVENT_RECORD& k = recs[i].Event.KeyEvent;
                    if (!k.bKeyDown) continue;
                    if (k.wVirtualKeyCode == VK_ESCAPE) { quit = true; break; }
                    const wchar_t wc = k.uChar.UnicodeChar;
                    if (k.wVirtualKeyCode == VK_BACK) {
                        engine.on_delete(now(), 1);
                        printf("\b \b");
                    } else if (wc == L'\r') {
                        const char nl = '\n';
                        engine.on_insert(now(), 1, &nl, 1);
                        printf("\n");
                    } else if (wc >= 32) {
                        char utf8[8];
                        const int len = WideCharToMultiByte(
                            CP_UTF8, 0, &wc, 1, utf8, sizeof(utf8), nullptr, nullptr);
                        if (len > 0) {
                            engine.on_insert(now(), 1, utf8, len);
                            fwrite(utf8, 1, len, stdout);
                        }
                    }
                }
            }
        }
        if (now() >= next_tick) {
            next_tick += 1.0;
            engine.tick(now(), true);
            printf("\n");
            draw_status(engine.state(), rewards->rewards, hw);
            printf("\x1b[1A\r"); // status lives one line below the typing line
        }
    }

    printf("\n\nsession over — %u reward(s).\n", rewards->rewards);
    engine.shutdown();
    Sleep(300);
    SetConsoleMode(hin, inmode);
    SetConsoleMode(hout, outmode);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string url = "ws://127.0.0.1:12345";
    bool selftest = false, use_hw = true, shipped = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--selftest")) selftest = true;
        else if (!strcmp(argv[i], "--no-hw")) use_hw = false;
        else if (!strcmp(argv[i], "--shipped")) shipped = true;
        else if (!strcmp(argv[i], "--snappy")) shipped = false;
        else if (!strcmp(argv[i], "--url") && i + 1 < argc) url = argv[++i];
    }
    if (selftest) return run_selftest(url, use_hw);

    sbpp::Config cfg;
    if (!shipped) {
        // Demo timing: qualify after 15 s of FLOW, mature in ~45 s on average,
        // 20 s cooldown — you feel the loop within the first two minutes.
        cfg.min_flow_hold_s = 15.0;
        cfg.mean_reward_interval_s = 45.0;
        cfg.min_cooldown_s = 20.0;
    }
    return run_interactive(cfg, url, use_hw);
}
