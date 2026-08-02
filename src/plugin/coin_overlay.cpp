// SkinnerBox++ — coin reward overlay (Notepad++/Scintilla).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "coin_overlay.h"

#include <algorithm>

#include "../adapters/sfx.h"
#include "../npp/Scintilla.h"

namespace sbpp {

namespace {

const wchar_t kClass[] = L"SbppCoinOverlay";
constexpr COLORREF kKey = RGB(255, 0, 255);   // color-keyed background
constexpr int kCoinPx = 26;                    // coin diameter
constexpr int kWinW = 240, kWinH = 64;
constexpr BYTE kAlpha = 235;                   // semi-transparent, per spec
constexpr unsigned kPopMs = 900;               // collect animation length
constexpr unsigned kFadeMs = 700;              // expiry fade length
constexpr UINT_PTR kAnimTimer = 1;

HWND current_scintilla(const NppData& npp) {
    int which = 0;
    SendMessage(npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0,
                reinterpret_cast<LPARAM>(&which));
    return which == 0 ? npp._scintillaMainHandle : npp._scintillaSecondHandle;
}

} // namespace

CoinOverlay::CoinOverlay(const NppData& npp, const Config& cfg,
                         std::vector<std::wstring> affirmations,
                         const Sfx* sfx,
                         std::function<void(RewardClass)> on_collect)
    : npp_(npp), cfg_(cfg), affirmations_(std::move(affirmations)), sfx_(sfx),
      on_collect_(std::move(on_collect)) {
    rng_ = static_cast<unsigned>(GetTickCount64() | 1);
    // Register against the DLL that owns wnd_proc, NOT the host exe: a class
    // registered under notepad++.exe would outlive plugin unload with a
    // procedure pointing into freed code. Paired with UnregisterClassW in
    // shutdown().
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&wnd_proc), &hinst_);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hinst_;
    wc.lpszClassName = kClass;
    wc.hbrBackground = nullptr; // we paint everything
    RegisterClassW(&wc); // idempotent: re-register fails harmlessly
    // Click-through reward sprite. WS_EX_NOACTIVATE is load-bearing:
    // WS_EX_TRANSPARENT alone still lets alt-tab/taskbar activate it.
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClass, L"", WS_POPUP, 0, 0, kWinW, kWinH, npp_._nppHandle, nullptr,
        hinst_, nullptr);
    if (hwnd_) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(this));
        SetLayeredWindowAttributes(hwnd_, kKey, kAlpha,
                                   LWA_ALPHA | LWA_COLORKEY);
        // Antialiased, NOT ClearType: subpixel rendering against the magenta
        // color key leaves colored fringing around glyph edges.
        font_ = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            VARIABLE_PITCH, L"Segoe UI");
    }
}

CoinOverlay::~CoinOverlay() { shutdown(); }

void CoinOverlay::shutdown() {
    hide();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    // No dangling class after the DLL unloads. Only one overlay exists at a
    // time, so unregistering here is always safe.
    if (hinst_) {
        UnregisterClassW(kClass, hinst_);
        hinst_ = nullptr;
    }
}

void CoinOverlay::spawn(RewardClass kind, double net_cpm) {
    if (!hwnd_) return;
    // A red coin replaces a pending yellow; a yellow never replaces a red,
    // and never interrupts a running collect animation.
    if (phase_ == Phase::Popping) return;
    if (phase_ == Phase::Pending && kind_ == RewardClass::MicroReward &&
        kind == RewardClass::RegularityCoin)
        return;

    HWND sci = current_scintilla(npp_);
    const auto caret = static_cast<long long>(
        SendMessage(sci, SCI_GETCURRENTPOS, 0, 0));
    // Lead: coin_lead_seconds of typing at the current rate, floored so the
    // coin is never on top of the caret even from a cold start. Positions
    // are Scintilla BYTE offsets while the lead is a char estimate — for
    // multibyte UTF-8 input the coin collects a little early (never late),
    // consistent with the gate's documented Latin/ASCII orientation.
    const double cps = std::max(1.0, net_cpm / 60.0);
    const long long lead_chars = std::max<long long>(
        10, static_cast<long long>(cps * cfg_.coin_lead_seconds));
    target_pos_ = caret + lead_chars;

    const int cx = static_cast<int>(
        SendMessage(sci, SCI_POINTXFROMPOSITION, 0, caret));
    const int cy = static_cast<int>(
        SendMessage(sci, SCI_POINTYFROMPOSITION, 0, caret));
    const long long line = SendMessage(sci, SCI_LINEFROMPOSITION, caret, 0);
    const int line_h = static_cast<int>(
        SendMessage(sci, SCI_TEXTHEIGHT, static_cast<WPARAM>(line), 0));
    int char_w = static_cast<int>(
        SendMessage(sci, SCI_TEXTWIDTH, STYLE_DEFAULT,
                    reinterpret_cast<LPARAM>("n")));
    if (char_w <= 0) char_w = 8;

    RECT client{};
    GetClientRect(sci, &client);
    // Place the coin where the lead lands on this line; if that overshoots
    // the wrap width, drop it to the start of the next visual line — "just
    // enough ahead of the word wrap".
    int x = cx + static_cast<int>(lead_chars) * char_w;
    int y = cy - (kWinH - line_h) / 2;
    if (x > client.right - kCoinPx - 24) {
        x = std::max(24L, client.left + 24L);
        y += line_h;
    }
    POINT pt{x, y};
    ClientToScreen(sci, &pt);

    // State before show: the first WM_PAINT must already draw this coin.
    kind_ = kind;
    phase_ = Phase::Pending;
    spawned_ms_ = GetTickCount64();
    KillTimer(hwnd_, kAnimTimer); // cancel any leftover fade animation
    SetLayeredWindowAttributes(hwnd_, kKey, kAlpha, LWA_ALPHA | LWA_COLORKEY);
    place_window(pt.x, pt.y, kWinW, kWinH);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void CoinOverlay::on_typed(long long pos, long long len) {
    if (phase_ != Phase::Pending) return;
    // Crossing rule: only a keystroke-sized insert whose range carries the
    // caret THROUGH the target pays. An insert entirely past the target
    // (user clicked ahead and typed) never crosses; a paste is too big to
    // be typing and is excluded regardless of where it lands.
    if (len > 0 && len <= 8 && pos < target_pos_ && pos + len >= target_pos_)
        collect();
}

void CoinOverlay::tick() {
    if (phase_ == Phase::Pending &&
        GetTickCount64() - spawned_ms_ >
            static_cast<unsigned long long>(cfg_.coin_expire_seconds * 1000.0))
        begin_fade();
}

void CoinOverlay::cancel_pending() {
    // Document changed under the coin: its target position is meaningless in
    // the new buffer and could false-collect. Vanish, pay nothing.
    if (phase_ == Phase::Pending) hide();
}

void CoinOverlay::on_host_focus(bool focused) {
    // The sprite is TOPMOST: the instant the editor stops being the
    // foreground window it must disappear (never float over another app or
    // a screen-share). Losing focus mid-coin forfeits it — leaving is
    // leaving.
    if (!focused && phase_ != Phase::Hidden) hide();
}

void CoinOverlay::collect() {
    phase_ = Phase::Popping;
    anim_start_ms_ = GetTickCount64();
    if (kind_ == RewardClass::MicroReward && !affirmations_.empty()) {
        rng_ = rng_ * 1664525u + 1013904223u;
        affirmation_ = affirmations_[(rng_ >> 16) % affirmations_.size()];
    } else {
        affirmation_.clear();
    }
    if (sfx_ && cfg_.coin_sound)
        sfx_->play(kind_ == RewardClass::MicroReward ? Sfx::Cue::CoinRed
                                                     : Sfx::Cue::CoinYellow);
    if (on_collect_) on_collect_(kind_);
    if (hwnd_) SetTimer(hwnd_, kAnimTimer, 30, nullptr);
}

void CoinOverlay::begin_fade() {
    phase_ = Phase::FadingOut;
    anim_start_ms_ = GetTickCount64();
    if (hwnd_) SetTimer(hwnd_, kAnimTimer, 30, nullptr);
}

void CoinOverlay::hide() {
    phase_ = Phase::Hidden;
    if (hwnd_) {
        KillTimer(hwnd_, kAnimTimer);
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void CoinOverlay::place_window(int x, int y, int w, int h) {
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CoinOverlay::animate() {
    const unsigned long long now = GetTickCount64();
    const unsigned elapsed = static_cast<unsigned>(now - anim_start_ms_);
    if (phase_ == Phase::Popping) {
        if (elapsed >= kPopMs) {
            hide();
            return;
        }
        // Rise and fade: the whole sprite drifts up while alpha falls off in
        // the back half.
        const double t = static_cast<double>(elapsed) / kPopMs;
        RECT rc{};
        GetWindowRect(hwnd_, &rc);
        if (elapsed % 60 < 30) // ~2 px/frame drift, no per-frame repositioning
            SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, rc.top - 2, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
        const double fade = t < 0.5 ? 1.0 : 1.0 - (t - 0.5) * 2.0;
        SetLayeredWindowAttributes(hwnd_, kKey,
                                   static_cast<BYTE>(kAlpha * fade),
                                   LWA_ALPHA | LWA_COLORKEY);
        InvalidateRect(hwnd_, nullptr, TRUE);
    } else if (phase_ == Phase::FadingOut) {
        if (elapsed >= kFadeMs) {
            hide();
            return;
        }
        const double fade = 1.0 - static_cast<double>(elapsed) / kFadeMs;
        SetLayeredWindowAttributes(hwnd_, kKey,
                                   static_cast<BYTE>(kAlpha * fade),
                                   LWA_ALPHA | LWA_COLORKEY);
    } else {
        hide();
    }
}

void CoinOverlay::paint(HDC dc, const RECT& rc) {
    // Back-buffer to avoid flicker at 30 ms animation frames.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    HBRUSH key = CreateSolidBrush(kKey);
    FillRect(mem, &rc, key);
    DeleteObject(key);

    const bool red = kind_ == RewardClass::MicroReward;
    const bool popping = phase_ == Phase::Popping;
    // Pop: the coin swells ~35% in the first 150 ms of the collect.
    int d = kCoinPx;
    if (popping) {
        const unsigned e = static_cast<unsigned>(GetTickCount64() - anim_start_ms_);
        const double swell = e < 150 ? 1.0 + 0.35 * (e / 150.0) : 1.35;
        d = static_cast<int>(kCoinPx * swell);
    }
    const int cx = 8 + kCoinPx / 2, cy = rc.bottom / 2;

    const COLORREF body = red ? RGB(224, 64, 64) : RGB(244, 196, 48);
    const COLORREF rim = red ? RGB(150, 32, 32) : RGB(180, 132, 16);
    const COLORREF shine = red ? RGB(255, 150, 150) : RGB(255, 240, 170);
    HBRUSH b = CreateSolidBrush(body);
    HPEN p = CreatePen(PS_SOLID, 2, rim);
    HGDIOBJ ob = SelectObject(mem, b);
    HGDIOBJ op = SelectObject(mem, p);
    Ellipse(mem, cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2);
    // Highlight crescent, upper-left.
    HPEN ps = CreatePen(PS_SOLID, 2, shine);
    SelectObject(mem, ps);
    Arc(mem, cx - d / 2 + 4, cy - d / 2 + 4, cx + d / 2 - 4, cy + d / 2 - 4,
        cx - d / 2, cy - d / 2, cx + d / 2, cy - d / 2);
    SelectObject(mem, op);
    DeleteObject(ps);
    DeleteObject(p);
    SelectObject(mem, ob);
    DeleteObject(b);

    // Affirmation rides only on the red collect — words are the potent
    // tier's payload, not wallpaper.
    if (popping && !affirmation_.empty() && font_) {
        HGDIOBJ of = SelectObject(mem, font_);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(60, 20, 20));
        RECT tr{8 + kCoinPx + 10, 0, rc.right, rc.bottom};
        // Cheap outline: offset dark, then bright center — readable on any
        // document background without querying the theme.
        DrawTextW(mem, affirmation_.c_str(), -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOCLIP);
        OffsetRect(&tr, -1, -1);
        SetTextColor(mem, RGB(255, 226, 130));
        DrawTextW(mem, affirmation_.c_str(), -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOCLIP);
        SelectObject(mem, of);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK CoinOverlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp,
                                       LPARAM lp) {
    auto* self = reinterpret_cast<CoinOverlay*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (self) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                self->paint(dc, rc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (self && wp == kAnimTimer) self->animate();
            return 0;
        case WM_ERASEBKGND:
            return 1; // back-buffered in paint()
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace sbpp
