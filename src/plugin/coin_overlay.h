// SkinnerBox++ — coin reward overlay (Notepad++/Scintilla).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
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
// Collection semantics: the coin collects only when a KEYSTROKE-SIZED
// insert (≤ 8 bytes) CROSSES its document offset — pos < target ≤ pos+len.
// Clicking past the coin and typing there does not collect (the range never
// crosses); a paste crossing it does not collect (too big to be typing);
// only typing that carries the caret through the coin's position pays. The
// target is a fixed byte offset — a net-forward-production meter — not an
// anchor into specific text: edits elsewhere shrink or grow the remaining
// gap rather than moving the coin.
//
// The window is a click-through, never-activated, color-keyed layered popup
// (WS_EX_TRANSPARENT alone still lets alt-tab focus it — WS_EX_NOACTIVATE is
// load-bearing). Screen-fixed once spawned; scrolling while a coin is
// pending leaves it visually misplaced until collect/expiry (known v0
// limitation). It is TOPMOST, so it hides the moment the host window loses
// focus — a reward sprite must never float over other applications.
//
// An uncollected coin fades out after coin_expire_seconds and pays nothing:
// collecting a stale coin on return from a stall would reinforce the
// stall/return cycle (same design law that killed stall-recovery rewards).
class CoinOverlay {
public:
    // on_collect fires at the moment of collection (the payoff moment) so
    // the owner can run its phasic effects (bloom, message) in sync.
    CoinOverlay(const NppData& npp, const Config& cfg,
                std::vector<std::wstring> affirmations, const Sfx* sfx,
                std::function<void(RewardClass)> on_collect);
    ~CoinOverlay();

    // Spawn a coin ahead of the caret. cpm sizes the lead; a red coin
    // replaces a pending yellow, a yellow never replaces a pending red.
    void spawn(RewardClass kind, double net_cpm);

    // Host forwards every user insert (document position + byte length).
    // Applies the crossing rule above.
    void on_typed(long long pos, long long len);

    // ~1 Hz upkeep: expiry.
    void tick();

    // Buffer/tab switched: a pending coin's document position no longer
    // means anything — drop it silently so it cannot false-collect.
    void cancel_pending();

    // Host focus tracking: the topmost sprite hides whenever the editor is
    // not the foreground window (screen-share safety, and a coin over some
    // other app is meaningless anyway).
    void on_host_focus(bool focused);

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
    std::function<void(RewardClass)> on_collect_;

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
