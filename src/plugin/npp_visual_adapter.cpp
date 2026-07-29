// SkinnerBox++ — in-editor visual adapter (Notepad++/Scintilla).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "npp_visual_adapter.h"

#include <algorithm>
#include <cstdio>

#include "../npp/Scintilla.h"

namespace sbpp {

namespace {

// Warm target the caret line drifts toward at full flow (COLORREF is BGR).
constexpr BYTE kWarmR = 255, kWarmG = 228, kWarmB = 160;

COLORREF lerp_color(COLORREF from, BYTE to_r, BYTE to_g, BYTE to_b, double t) {
    t = std::max(0.0, std::min(1.0, t));
    const BYTE r = static_cast<BYTE>(GetRValue(from) + t * (to_r - GetRValue(from)));
    const BYTE g = static_cast<BYTE>(GetGValue(from) + t * (to_g - GetGValue(from)));
    const BYTE b = static_cast<BYTE>(GetBValue(from) + t * (to_b - GetBValue(from)));
    return RGB(r, g, b);
}

} // namespace

NppVisualAdapter::NppVisualAdapter(const NppData& npp, const Config& cfg)
    : npp_(npp), cfg_(cfg) {
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
    const int filled = static_cast<int>(s.flow * 5.0 + 0.5);
    wchar_t bar[8] = L"";
    for (int i = 0; i < 5; ++i) bar[i] = i < filled ? L'▰' : L'▱';
    bar[5] = L'\0';
    wchar_t text[96];
    swprintf(text, 96, L"SkinnerBox++ %s %.2f %hs", bar, s.flow,
             regime_name(s.regime));
    if (last_status_ == text) return;
    last_status_ = text;
    SendMessage(npp_._nppHandle, NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE,
                reinterpret_cast<LPARAM>(text));
}

void NppVisualAdapter::ambient(const AmbientState& s) {
    if (cfg_.visual_enabled) {
        const bool blooming = GetTickCount64() < bloom_until_ms_;
        // Tonic tint tops out at 35% toward warm; bloom pushes to 75%.
        const double t = blooming ? 0.75 : 0.35 * s.flow;
        apply_color(lerp_color(base_color_, kWarmR, kWarmG, kWarmB, t));
    }
    set_statusbar(s);
}

void NppVisualAdapter::deliver(const RewardIntent& intent) {
    if (intent.withheld || !cfg_.visual_enabled) return;
    if (intent.kind == RewardClass::SessionSummary) return;
    bloom_until_ms_ = GetTickCount64() + intent.max_duration_ms;
    apply_color(lerp_color(base_color_, kWarmR, kWarmG, kWarmB, 0.75));
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
