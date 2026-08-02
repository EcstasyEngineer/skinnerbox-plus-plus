// SkinnerBox++ — headless unit tests for core invariants (no WinHTTP / N++).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.
//
//   unit.exe   exit 0 = all pass

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../src/core/config.h"
#include "../src/core/engine.h"
#include "../src/core/content.h"

namespace {

int g_fails = 0;

void expect(bool cond, const char* msg) {
    if (!cond) {
        printf("FAIL: %s\n", msg);
        ++g_fails;
    }
}

// Captures every ambient + reward the engine fans out. The engine sends
// ambient() before any deliver() on the same tick, so ambients.back() at
// deliver time is the state the reward fired under.
class CaptureAdapter : public sbpp::IOutputAdapter {
public:
    const char* name() const override { return "capture"; }
    void ambient(const sbpp::AmbientState& s) override { ambients.push_back(s); }
    void deliver(const sbpp::RewardIntent& i) override {
        rewards.push_back(i);
        reward_states.push_back(ambients.empty() ? sbpp::AmbientState{}
                                                 : ambients.back());
    }
    void shutdown() override {}

    std::vector<sbpp::AmbientState> ambients;
    std::vector<sbpp::RewardIntent> rewards;
    std::vector<sbpp::AmbientState> reward_states; // state at delivery
};

const char* kProse =
    "The tide came in slowly over the flats, and the birds walked ahead of it, "
    "picking at whatever the water pushed up. She watched from the seawall with "
    "her coat pulled tight, counting the gray shapes against the light. There "
    "was a letter in her pocket she had not opened, and the paper edge pressed "
    "against her hand like a question she already knew the answer to. The wind "
    "moved the dune grass in long silver waves, and somewhere behind her a door "
    "kept knocking against its frame, patient and dull as a clock. ";

void feed_prose(sbpp::FlowEngine& engine, double& t, size_t& pos, int chars) {
    const size_t n = std::strlen(kProse);
    for (int i = 0; i < chars; ++i) {
        const char c = kProse[pos % n];
        engine.on_insert(t, 1, &c, 1);
        ++pos;
    }
}

void test_gate_fail_names() {
    printf("test_gate_fail_names...\n");
    sbpp::Config cfg;
    cfg.gate_min_chars = 80;
    sbpp::FlowEngine engine(cfg, 1);
    auto cap = std::make_unique<CaptureAdapter>();
    CaptureAdapter* c = cap.get();
    engine.add_adapter(std::move(cap));

    double t = 1000.0;
    // Thin: not enough text
    engine.tick(t, true);
    expect(!c->ambients.empty(), "thin: ambient emitted");
    expect(!c->ambients.back().gate_ok, "thin: gate fails");
    expect(std::string(c->ambients.back().gate_fail) == "thin", "thin: named thin");

    // Mash-like keyboard garbage after filling window
    const char* mash = "sdlfkjasdlfkjasdlfkjasdlfkjasdlfkjasdlfkjasdlfkjasdlfkja"
                       "sdlfkjasdlfkjasdlfkjasdlfkjasdlfkjasdlfkja";
    engine.on_insert(t, static_cast<uint32_t>(std::strlen(mash)), mash,
                     std::strlen(mash));
    engine.tick(t + 1.0, true);
    expect(!c->ambients.back().gate_ok, "mash: gate fails");
    // first-fail chain: after thin is satisfied, mash/flat/etc.
    const char* fail = c->ambients.back().gate_fail;
    expect(fail && fail[0], "mash: non-empty fail tag");
}

void test_state_facets_match_adapter() {
    printf("test_state_facets_match_adapter...\n");
    sbpp::Config cfg;
    cfg.gate_min_chars = 40;
    cfg.min_flow_hold_s = 9999; // no rewards
    sbpp::FlowEngine engine(cfg, 2);
    auto cap = std::make_unique<CaptureAdapter>();
    CaptureAdapter* c = cap.get();
    engine.add_adapter(std::move(cap));

    double t = 2000.0;
    size_t pos = 0;
    for (int i = 0; i < 30; ++i) {
        t += 1.0;
        feed_prose(engine, t, pos, 8);
        engine.tick(t, true);
    }
    expect(!c->ambients.empty(), "facets: got ambients");
    const auto& a = c->ambients.back();
    const auto& s = engine.state();
    expect(s.entropy == a.entropy, "facets: engine.state entropy matches adapter");
    expect(s.repetition == a.repetition, "facets: repetition matches");
    expect(s.bigram_bpc == a.bigram_bpc, "facets: bigram_bpc matches");
    expect(s.gate_ok == a.gate_ok, "facets: gate_ok matches");
    expect(std::string(s.gate_fail ? s.gate_fail : "") ==
               std::string(a.gate_fail ? a.gate_fail : ""),
           "facets: gate_fail matches");
    expect(s.entropy > 0.0, "facets: entropy nonzero on prose");
}

void test_no_flow_without_gate() {
    printf("test_no_flow_without_gate...\n");
    sbpp::Config cfg;
    cfg.gate_min_chars = 5000; // impossible
    cfg.flow_enter = 0.01;
    cfg.ewma_alpha = 0.5;
    sbpp::FlowEngine engine(cfg, 3);
    auto cap = std::make_unique<CaptureAdapter>();
    CaptureAdapter* c = cap.get();
    engine.add_adapter(std::move(cap));

    double t = 3000.0;
    size_t pos = 0;
    for (int i = 0; i < 40; ++i) {
        t += 1.0;
        feed_prose(engine, t, pos, 20);
        engine.tick(t, true);
    }
    for (const auto& a : c->ambients) {
        if (a.regime == sbpp::Regime::Flow) {
            expect(false, "no FLOW when gate cannot pass");
            break;
        }
    }
    // The QUALITY tier can never fire without the gate. The regularity tier
    // is gate-free by design (volume, not quality) and may coin here.
    for (const auto& r : c->rewards)
        expect(r.kind != sbpp::RewardClass::MicroReward,
               "no quality reward when gate cannot pass");
    expect(!engine.state().gate_ok, "state gate_ok false");
}

void test_no_reward_while_gated() {
    printf("test_no_reward_while_gated...\n");
    sbpp::Config cfg;
    cfg.min_flow_hold_s = 1.0;
    cfg.mean_reward_interval_s = 1.0;
    cfg.min_cooldown_s = 1.0;
    cfg.gate_min_chars = 40;
    sbpp::FlowEngine engine(cfg, 4);
    auto cap = std::make_unique<CaptureAdapter>();
    CaptureAdapter* c = cap.get();
    engine.add_adapter(std::move(cap));

    double t = 4000.0;
    size_t pos = 0;
    // Build FLOW with prose
    for (int i = 0; i < 50; ++i) {
        t += 1.0;
        feed_prose(engine, t, pos, 10);
        engine.tick(t, true);
    }
    const size_t rewards_before = c->rewards.size();

    (void)rewards_before;
    // Inject mash to break gate mid-session. The 600-char window blends, so
    // the gate takes a few mash ticks to fail — FLOW (and legitimate
    // rewards) may briefly persist. The hard invariant is per-delivery:
    // every quality reward fires under FLOW with the gate green.
    const char* mash =
        "asdfasdfasdfasdfasdfasdfasdfasdfasdfasdfasdfasdfasdfasdfasdf";
    bool saw_gate_fail = false;
    for (int i = 0; i < 20; ++i) {
        t += 1.0;
        engine.on_insert(t, static_cast<uint32_t>(std::strlen(mash)), mash,
                         std::strlen(mash));
        engine.tick(t, true);
        if (!c->ambients.back().gate_ok) {
            saw_gate_fail = true;
            expect(c->ambients.back().regime != sbpp::Regime::Flow,
                   "gated: not FLOW");
        }
    }
    expect(saw_gate_fail, "gated: mash actually broke the gate");
    for (size_t i = 0; i < c->rewards.size(); ++i) {
        if (c->rewards[i].kind != sbpp::RewardClass::MicroReward) continue;
        expect(c->reward_states[i].regime == sbpp::Regime::Flow &&
                   c->reward_states[i].gate_ok,
               "gated: every quality reward fired in FLOW with gate ok");
    }
}

void test_eligibility_forfeit_on_leave_flow() {
    printf("test_eligibility_forfeit_on_leave_flow...\n");
    sbpp::Config cfg;
    cfg.min_flow_hold_s = 5.0;
    cfg.mean_reward_interval_s = 2.0;
    cfg.min_cooldown_s = 1.0;
    cfg.gate_min_chars = 40;
    cfg.idle_seconds = 5.0;
    cfg.grace_seconds = 2.0;
    sbpp::FlowEngine engine(cfg, 5);
    auto cap = std::make_unique<CaptureAdapter>();
    CaptureAdapter* c = cap.get();
    engine.add_adapter(std::move(cap));

    double t = 5000.0;
    size_t pos = 0;
    for (int i = 0; i < 40; ++i) {
        t += 1.0;
        feed_prose(engine, t, pos, 12);
        engine.tick(t, true);
    }
    // Idle long enough to leave FLOW/TYPING into IDLE
    for (int i = 0; i < 20; ++i) {
        t += 1.0;
        engine.tick(t, true);
    }
    expect(engine.state().regime == sbpp::Regime::Idle ||
               engine.state().regime == sbpp::Regime::Typing,
           "forfeit: left sustained FLOW after idle");
    // Resume typing: hold clock must restart (no instant reward from old
    // eligibility). With min_flow_hold_s=5, first few ticks after re-enter
    // must not reward solely from prior eligibility.
    size_t quality0 = 0;
    for (const auto& r : c->rewards)
        if (r.kind == sbpp::RewardClass::MicroReward) ++quality0;
    for (int i = 0; i < 3; ++i) {
        t += 1.0;
        feed_prose(engine, t, pos, 12);
        engine.tick(t, true);
    }
    size_t quality1 = 0;
    for (const auto& r : c->rewards)
        if (r.kind == sbpp::RewardClass::MicroReward) ++quality1;
    expect(quality1 == quality0,
           "forfeit: no quality reward in first 3s after re-enter");
}

void test_regularity_coins() {
    printf("test_regularity_coins...\n");
    sbpp::Config cfg;
    cfg.gate_min_chars = 5000;          // gate can never pass
    cfg.coin_yellow_interval_chars = 100;
    cfg.min_flow_hold_s = 9999;         // quality tier out of the picture
    sbpp::FlowEngine engine(cfg, 6);
    auto cap = std::make_unique<CaptureAdapter>();
    CaptureAdapter* c = cap.get();
    engine.add_adapter(std::move(cap));

    double t = 6000.0;
    size_t pos = 0;
    // 30 ticks x 12 chars = 360 chars typed while active -> coins at every
    // 100-char watermark after the first tick arms the marker.
    for (int i = 0; i < 30; ++i) {
        t += 1.0;
        feed_prose(engine, t, pos, 12);
        engine.tick(t, true);
    }
    size_t coins = 0;
    for (const auto& r : c->rewards) {
        if (r.kind == sbpp::RewardClass::RegularityCoin) ++coins;
        expect(r.kind != sbpp::RewardClass::MicroReward,
               "coins: no quality reward possible here");
    }
    expect(coins >= 2 && coins <= 4, "coins: ~3 yellow coins over 360 chars");

    // Idle: no coins mature while not producing.
    const size_t before_idle = coins;
    for (int i = 0; i < 60; ++i) {
        t += 1.0;
        engine.tick(t, true);
    }
    coins = 0;
    for (const auto& r : c->rewards)
        if (r.kind == sbpp::RewardClass::RegularityCoin) ++coins;
    expect(coins == before_idle, "coins: none while idle");

    // Disabled: no coins at all.
    sbpp::Config off = cfg;
    off.coins_enabled = false;
    sbpp::FlowEngine e2(off, 7);
    auto cap2 = std::make_unique<CaptureAdapter>();
    CaptureAdapter* c2 = cap2.get();
    e2.add_adapter(std::move(cap2));
    t = 7000.0;
    pos = 0;
    for (int i = 0; i < 30; ++i) {
        t += 1.0;
        feed_prose(e2, t, pos, 12);
        e2.tick(t, true);
    }
    expect(c2->rewards.empty(), "coins: disabled means silent");
}

void test_content_thin_closed() {
    printf("test_content_thin_closed...\n");
    sbpp::ContentWindow w(600);
    auto f = w.facets();
    expect(f.window_chars == 0, "empty window");
    w.add_text("hi", 2);
    f = w.facets();
    expect(f.window_chars == 2, "short window count");
}

} // namespace

int main() {
    test_gate_fail_names();
    test_state_facets_match_adapter();
    test_no_flow_without_gate();
    test_no_reward_while_gated();
    test_eligibility_forfeit_on_leave_flow();
    test_regularity_coins();
    test_content_thin_closed();

    if (g_fails == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_fails);
    return 1;
}
