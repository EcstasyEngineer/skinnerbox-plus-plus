// SkinnerBox++ — in-editor visual adapter (Notepad++/Scintilla).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

#include "../core/adapter.h"
#include "../core/config.h"
#include "../npp/PluginInterface.h"

namespace sbpp {

class IntifaceAdapter;

// Tonic: the caret-line background warms from the theme's base color toward a
// soft gold as flow rises — environment weather, updated every tick, heavily
// smoothed upstream. Phasic: a delivered reward briefly blooms the same color
// brighter, then decays back to tonic. Status bar shows the numbers that
// matter: flow, FSM state, current hardware output, connection status.
//
// Fail-open: shutdown() restores the captured base color and caret-line
// visibility exactly as found.
class NppVisualAdapter : public IOutputAdapter {
public:
    // hw may be null (Intiface disabled); the status bar then shows hw:off.
    NppVisualAdapter(const NppData& npp, const Config& cfg,
                     const IntifaceAdapter* hw);

    const char* name() const override { return "visual"; }
    void ambient(const AmbientState& state) override;
    void deliver(const RewardIntent& intent) override;
    void shutdown() override;

private:
    void apply_color(COLORREF color);
    void set_statusbar(const AmbientState& state);

    NppData npp_;
    const Config& cfg_;
    const IntifaceAdapter* hw_;
    COLORREF base_color_ = 0;
    int base_caretline_visible_ = 0;
    COLORREF last_applied_ = 0xFFFFFFFF;
    unsigned long long bloom_until_ms_ = 0;
    double displayed_t_ = 0.0; // smoothed tint position, no square waves
    std::wstring last_status_;
    // Plain-language explanation of the last reward, shown in the status bar
    // so a reward is never unexplained.
    std::wstring message_;
    unsigned long long message_until_ms_ = 0;
};

} // namespace sbpp
