// SkinnerBox++ — in-editor visual adapter (Notepad++/Scintilla).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "npp_visual_adapter.h"

#include <algorithm>
#include <cstdio>

#include "../adapters/intiface_adapter.h"
#include "../npp/Scintilla.h"

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
                                   const IntifaceAdapter* hw)
    : npp_(npp), cfg_(cfg), hw_(hw) {
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
    const char* mode =
        cfg_.advanced_debug ? "LAB  " : (cfg_.debug_telemetry ? "REC  " : "");
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
        if (cfg_.advanced_debug && s.gpt2_ready) {
            swprintf(text, 256,
                     L"%hs%.2f %hs%hs · gpt2 %.1fb d=%.1f · out %.2f · %s",
                     mode, s.flow, regime_name(s.regime), gate,
                     s.gpt2_mean_bits, s.gpt2_band_dist,
                     hw_ ? hw_->current_output() : 0.0, hw);
        } else if (cfg_.advanced_debug) {
            swprintf(text, 256, L"%hs%.2f %hs%hs · gpt2 … · out %.2f · %s",
                     mode, s.flow, regime_name(s.regime), gate,
                     hw_ ? hw_->current_output() : 0.0, hw);
        } else {
            swprintf(text, 256, L"%hs%.2f %hs%hs · out %.2f · %s",
                     mode, s.flow, regime_name(s.regime), gate,
                     hw_ ? hw_->current_output() : 0.0, hw);
        }
    }
    if (last_status_ == text) return;
    last_status_ = text;
    SendMessage(npp_._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE,
                reinterpret_cast<LPARAM>(text));
}

void NppVisualAdapter::ambient(const AmbientState& s) {
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
    const unsigned long long now = GetTickCount64();
    // Bloom duration is visual-channel config, not policy intent duration
    // (intent.max_duration_ms is an abstract ceiling for logs/other adapters).
    bloom_until_ms_ = now + cfg_.bloom_ms;
    message_until_ms_ = now + cfg_.message_ms;
    // Name the behavior that earned it, not the internal event name.
    switch (intent.kind) {
        case RewardClass::MicroReward:
            message_ = L"held flow — good stretch";
            break;
    }
}

void NppVisualAdapter::shutdown() {
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
