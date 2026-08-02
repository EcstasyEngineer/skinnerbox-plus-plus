// SkinnerBox++ — in-editor visual adapter (Notepad++/Scintilla).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "npp_visual_adapter.h"

#include <algorithm>
#include <cstdio>

#include "../adapters/intiface_adapter.h"
#include "../adapters/sfx.h"
#include "../npp/Scintilla.h"
#include "coin_overlay.h"

namespace sbpp {

namespace {

COLORREF lerp_color(COLORREF from, BYTE to_r, BYTE to_g, BYTE to_b, double t) {
    t = std::max(0.0, std::min(1.0, t));
    const BYTE r = static_cast<BYTE>(GetRValue(from) + t * (to_r - GetRValue(from)));
    const BYTE g = static_cast<BYTE>(GetGValue(from) + t * (to_g - GetGValue(from)));
    const BYTE b = static_cast<BYTE>(GetBValue(from) + t * (to_b - GetBValue(from)));
    return RGB(r, g, b);
}

// Warm target derived from the theme's own caret-line color: shift hue toward
// gold while keeping the base's luminance class, so the tint reads as "the
// same theme, warmer" on both light and dark profiles.
void warm_target(COLORREF base, BYTE& r, BYTE& g, BYTE& b) {
    const double lum =
        (0.299 * GetRValue(base) + 0.587 * GetGValue(base) + 0.114 * GetBValue(base)) / 255.0;
    if (lum >= 0.5) { // light theme: drift toward a soft gold
        r = 255; g = 228; b = 150;
    } else {          // dark theme: a dim amber, brighter than base but not neon
        r = 120; g = 96; b = 40;
    }
}

} // namespace

NppVisualAdapter::NppVisualAdapter(const NppData& npp, const Config& cfg,
                                   const IntifaceAdapter* hw,
                                   const std::wstring& sounds_dir,
                                   std::vector<std::wstring> affirmations)
    : npp_(npp), cfg_(cfg), hw_(hw) {
    if (cfg_.coins_enabled) {
        if (cfg_.coin_sound) sfx_ = std::make_unique<Sfx>(sounds_dir);
        // Phasic visuals (bloom + explained message) fire at COLLECT, in
        // sync with the pop and the ding — the payoff is one moment.
        coins_ = std::make_unique<CoinOverlay>(
            npp_, cfg_, std::move(affirmations), sfx_.get(),
            [this](RewardClass kind) { on_coin_collect(kind); });
    }
    base_color_ = static_cast<COLORREF>(
        SendMessage(npp_._scintillaMainHandle, SCI_GETCARETLINEBACK, 0, 0));
    base_caretline_visible_ = static_cast<int>(
        SendMessage(npp_._scintillaMainHandle, SCI_GETCARETLINEVISIBLE, 0, 0));
    SendMessage(npp_._scintillaMainHandle, SCI_SETCARETLINEVISIBLE, 1, 0);
    SendMessage(npp_._scintillaSecondHandle, SCI_SETCARETLINEVISIBLE, 1, 0);
}

void NppVisualAdapter::apply_color(COLORREF color) {
    if (color == last_applied_) return;
    last_applied_ = color;
    SendMessage(npp_._scintillaMainHandle, SCI_SETCARETLINEBACK, color, 0);
    SendMessage(npp_._scintillaSecondHandle, SCI_SETCARETLINEBACK, color, 0);
}

void NppVisualAdapter::set_statusbar(const AmbientState& s) {
    if (!cfg_.statusbar_enabled) return;
    // The numbers that matter, no meters: flow, FSM state, what the hardware
    // is outputting right now, and whether it's even connected.
    wchar_t hw[48];
    if (!hw_) {
        swprintf(hw, 48, L"hw off");
    } else if (hw_->connected()) {
        swprintf(hw, 48, L"hw ok (%zu dev)", hw_->device_count());
    } else {
        swprintf(hw, 48, L"hw DISCONNECTED");
    }
    const char* mode = cfg_.debug_telemetry ? "REC  " : "";
    wchar_t text[256];
    if (GetTickCount64() < message_until_ms_ && !message_.empty()) {
        // A reward happened: say what and why, in words, where the eye can
        // find it later — never a flash that interrupts reading.
        swprintf(text, 256, L"%hs%.2f %hs · %s · %s",
                 mode, s.flow, regime_name(s.regime), message_.c_str(), hw);
    } else {
        // Name the failing gate facet ("echo", "repeats", ...) so the writer
        // knows WHAT to change, not just that something is wrong.
        char gate[24] = "";
        if (!s.gate_ok)
            snprintf(gate, sizeof(gate), " (%s)",
                     s.gate_fail && s.gate_fail[0] ? s.gate_fail : "gate x");
        swprintf(text, 256, L"%hs%.2f %hs%hs · out %.2f · %s",
                 mode, s.flow, regime_name(s.regime), gate,
                 hw_ ? hw_->current_output() : 0.0, hw);
    }
    if (last_status_ == text) return;
    last_status_ = text;
    SendMessage(npp_._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE,
                reinterpret_cast<LPARAM>(text));
}

void NppVisualAdapter::ambient(const AmbientState& s) {
    last_cpm_ = s.net_rate_cpm;
    if (coins_) coins_->tick();
    const bool blooming = GetTickCount64() < bloom_until_ms_;
    if (cfg_.visual_enabled) {
        BYTE wr, wg, wb;
        warm_target(base_color_, wr, wg, wb);
        // Tonic tint tops out at 35% toward warm; bloom pushes past it.
        double target = 0.35 * s.flow;
        // A reward lifts the tint slightly ABOVE where it already was, rather
        // than jumping to a fixed bright value: continuous with the ramp, so
        // it reads as "a bit warmer", never as a flash.
        if (blooming) target = std::min(0.50, target + cfg_.bloom_lift);
        // Exponential smoothing: every transition ramps over a few seconds.
        displayed_t_ += 0.30 * (target - displayed_t_);
        apply_color(lerp_color(base_color_, wr, wg, wb, displayed_t_));
    }
    set_statusbar(s);
}

void NppVisualAdapter::deliver(const RewardIntent& intent) {
    // With coins on, delivery = a coin spawns ahead; ALL phasic visuals
    // wait for the collect (see on_coin_collect). An uncollected coin must
    // pay nothing on this channel — praising a reward the writer never
    // reached would decouple the words from the behavior.
    if (coins_) {
        coins_->spawn(intent.kind, last_cpm_);
        return;
    }
    // Coins disabled: the quality tier falls back to immediate bloom +
    // message (the pre-coin behavior). The regularity tier has no
    // coin-less mapping — small and frequent must stay small.
    if (intent.kind == RewardClass::MicroReward) on_coin_collect(intent.kind);
}

void NppVisualAdapter::on_coin_collect(RewardClass kind) {
    if (kind != RewardClass::MicroReward) return; // yellow: pop + ding only
    const unsigned long long now = GetTickCount64();
    // Bloom duration is visual-channel config, not policy intent duration
    // (intent.max_duration_ms is an abstract ceiling for logs/other adapters).
    bloom_until_ms_ = now + cfg_.bloom_ms;
    message_until_ms_ = now + cfg_.message_ms;
    // Name the behavior that earned it, not the internal event name.
    message_ = L"held flow — good stretch";
}

void NppVisualAdapter::on_typed(long long pos, long long len) {
    if (coins_) coins_->on_typed(pos, len);
}

void NppVisualAdapter::on_buffer_switch() {
    if (coins_) coins_->cancel_pending();
}

void NppVisualAdapter::on_host_focus(bool focused) {
    if (coins_) coins_->on_host_focus(focused);
}

NppVisualAdapter::~NppVisualAdapter() = default;

void NppVisualAdapter::shutdown() {
    if (coins_) coins_->shutdown();
    apply_color(base_color_);
    SendMessage(npp_._scintillaMainHandle, SCI_SETCARETLINEVISIBLE,
                base_caretline_visible_, 0);
    SendMessage(npp_._scintillaSecondHandle, SCI_SETCARETLINEVISIBLE,
                base_caretline_visible_, 0);
    if (cfg_.statusbar_enabled) {
        SendMessage(npp_._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE,
                    reinterpret_cast<LPARAM>(L""));
    }
}

} // namespace sbpp
