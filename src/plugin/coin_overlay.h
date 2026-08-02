// SkinnerBox++ — coin reward overlay (Notepad++/Scintilla).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

#include "../core/config.h"
#include "../core/reward_intent.h"
#include "../npp/PluginInterface.h"

namespace sbpp {

class Sfx;

// The delivery mechanism for phasic rewards: a coin SPAWNS AHEAD of the
// caret — about coin_lead_seconds of typing away at the current rate, just
// short of where the line will carry you — and is COLLECTED when typing
// reaches its document position. Anticipation at spawn, payoff mid-behavior
// at collect (the only way to reach a coin is to keep writing), like running
// into a coin in a platformer.
//
// Two tiers, matching RewardClass:
//   yellow (RegularityCoin) — small pop + the two-note coin ding
//   red    (MicroReward)    — bigger pop, warmer chime, plus a floating
//                             affirmation ("good girl", …) from the INI list
//
// The window is a click-through, never-activated, color-keyed layered popup
// (WS_EX_TRANSPARENT alone still lets alt-tab focus it — WS_EX_NOACTIVATE is
// load-bearing). Screen-fixed once spawned; scrolling while a coin is
// pending leaves it visually misplaced until collect/expiry (known v0
// limitation — collection is checked against the document position, which
// scrolling cannot fake).
//
// An uncollected coin fades out after coin_expire_seconds and pays nothing:
// collecting a stale coin on return from a stall would reinforce the
// stall/return cycle (same design law that killed stall-recovery rewards).
class CoinOverlay {
public:
    CoinOverlay(const NppData& npp, const Config& cfg,
                std::vector<std::wstring> affirmations, const Sfx* sfx);
    ~CoinOverlay();

    // Spawn a coin ahead of the caret. cpm sizes the lead; a red coin
    // replaces a pending yellow, a yellow never replaces a pending red.
    void spawn(RewardClass kind, double net_cpm);

    // Caret advanced by typing (host forwards insert events). Collects the
    // pending coin once the caret reaches its document position. Clicking
    // past a coin does NOT collect it — only typed advancement calls this.
    void on_typed_to(long long caret_pos);

    // ~1 Hz upkeep: expiry.
    void tick();

    // Buffer/tab switched: a pending coin's document position no longer
    // means anything — drop it silently so it cannot false-collect.
    void cancel_pending();

    void shutdown();

private:
    enum class Phase { Hidden, Pending, Popping, FadingOut };

    void collect();
    void begin_fade();
    void hide();
    void place_window(int x, int y, int w, int h);
    void paint(HDC dc, const RECT& rc);
    void animate();

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);

    NppData npp_;
    const Config& cfg_;
    std::vector<std::wstring> affirmations_;
    const Sfx* sfx_; // may be null (sound disabled)

    HWND hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr; // the plugin DLL (owns wnd_proc + the class)
    HFONT font_ = nullptr;
    Phase phase_ = Phase::Hidden;
    RewardClass kind_ = RewardClass::RegularityCoin;
    long long target_pos_ = 0;          // document position that collects
    unsigned long long spawned_ms_ = 0; // for expiry
    unsigned long long anim_start_ms_ = 0;
    std::wstring affirmation_;          // chosen at collect (red tier)
    unsigned rng_ = 0;                  // cheap LCG for affirmation pick
};

} // namespace sbpp
