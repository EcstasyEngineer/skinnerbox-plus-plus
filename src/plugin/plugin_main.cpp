// SkinnerBox++ — Notepad++ plugin host.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.
//
// Thin shell: owns the Notepad++ plugin ABI, the 1 Hz timer, and config/log
// paths. All flow logic lives in src/core (editor-independent); all outputs
// are IOutputAdapter implementations. Keep it that way — this file should
// never grow opinions about flow or rewards.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

#include "../npp/PluginInterface.h"
#include "../npp/Scintilla.h"
#include "../core/config.h"
#include "../core/engine.h"
#include "../adapters/audio_adapter.h"
#include "../adapters/log_adapter.h"
#include "../adapters/raw_log.h"
#include "npp_visual_adapter.h"

#include <string>

namespace {

const wchar_t kPluginName[] = L"SkinnerBox++";
constexpr int kNbFunc = 10;

NppData g_npp;
FuncItem g_funcs[kNbFunc];
sbpp::Config g_cfg;
std::unique_ptr<sbpp::FlowEngine> g_engine;
std::unique_ptr<sbpp::RawLog> g_rawlog;
UINT_PTR g_timer = 0;
bool g_enabled = false;
bool g_focus_was_here = true;
std::wstring g_ini_path;
std::wstring g_log_dir;

double now_s() { return static_cast<double>(GetTickCount64()) / 1000.0; }

// ---------------------------------------------------------------- config ---

double read_ini_double(const wchar_t* section, const wchar_t* key, double def) {
    wchar_t buf[64], defbuf[64];
    swprintf(defbuf, 64, L"%g", def);
    GetPrivateProfileStringW(section, key, defbuf, buf, 64, g_ini_path.c_str());
    return _wtof(buf);
}

void write_ini_double(const wchar_t* section, const wchar_t* key, double v) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%g", v);
    WritePrivateProfileStringW(section, key, buf, g_ini_path.c_str());
}

void load_config() {
    // Round-trip every key so a fresh install materializes a complete,
    // editable INI with the shipped defaults.
    sbpp::Config d; // defaults
    g_cfg.target_net_cpm = read_ini_double(L"flow", L"target_net_cpm", d.target_net_cpm);
    g_cfg.ewma_alpha = read_ini_double(L"flow", L"ewma_alpha", d.ewma_alpha);
    g_cfg.flow_enter = read_ini_double(L"flow", L"flow_enter", d.flow_enter);
    g_cfg.flow_exit = read_ini_double(L"flow", L"flow_exit", d.flow_exit);
    g_cfg.editing_deletion_ratio =
        read_ini_double(L"flow", L"editing_deletion_ratio", d.editing_deletion_ratio);
    g_cfg.stall_idle_seconds =
        read_ini_double(L"flow", L"stall_idle_seconds", d.stall_idle_seconds);
    g_cfg.burst_gap_seconds =
        read_ini_double(L"flow", L"burst_gap_seconds", d.burst_gap_seconds);
    g_cfg.mean_reward_interval_s =
        read_ini_double(L"policy", L"mean_reward_interval_s", d.mean_reward_interval_s);
    g_cfg.min_cooldown_s = read_ini_double(L"policy", L"min_cooldown_s", d.min_cooldown_s);
    g_cfg.min_flow_hold_s = read_ini_double(L"policy", L"min_flow_hold_s", d.min_flow_hold_s);
    g_cfg.withhold_probability =
        read_ini_double(L"policy", L"withhold_probability", d.withhold_probability);
    g_cfg.recovery_chars = read_ini_double(L"policy", L"recovery_chars", d.recovery_chars);
    g_cfg.recovery_window_s =
        read_ini_double(L"policy", L"recovery_window_s", d.recovery_window_s);
    g_cfg.grace_seconds = read_ini_double(L"flow", L"grace_seconds", d.grace_seconds);
    g_cfg.slop_repetition_max =
        read_ini_double(L"gate", L"slop_repetition_max", d.slop_repetition_max);
    g_cfg.slop_entropy_min =
        read_ini_double(L"gate", L"slop_entropy_min", d.slop_entropy_min);
    g_cfg.slop_stall_frac_max =
        read_ini_double(L"gate", L"slop_stall_frac_max", d.slop_stall_frac_max);
    g_cfg.gate_min_chars = read_ini_double(L"gate", L"gate_min_chars", d.gate_min_chars);
    g_cfg.visual_enabled = read_ini_double(L"channels", L"visual", 1) != 0;
    g_cfg.audio_enabled = read_ini_double(L"channels", L"audio", 1) != 0;
    g_cfg.statusbar_enabled = read_ini_double(L"channels", L"statusbar", 1) != 0;
    g_cfg.bloom_ms =
        static_cast<uint32_t>(read_ini_double(L"channels", L"bloom_ms", d.bloom_ms));
    g_cfg.raw_log_enabled = read_ini_double(L"telemetry", L"raw_log", 0) != 0;
    g_cfg.capture_text = read_ini_double(L"telemetry", L"capture_text", 0) != 0;
    g_enabled = read_ini_double(L"general", L"enabled", 1) != 0;
}

void persist_config() {
    write_ini_double(L"general", L"enabled", g_enabled ? 1 : 0);
    write_ini_double(L"flow", L"target_net_cpm", g_cfg.target_net_cpm);
    write_ini_double(L"flow", L"ewma_alpha", g_cfg.ewma_alpha);
    write_ini_double(L"flow", L"flow_enter", g_cfg.flow_enter);
    write_ini_double(L"flow", L"flow_exit", g_cfg.flow_exit);
    write_ini_double(L"flow", L"editing_deletion_ratio", g_cfg.editing_deletion_ratio);
    write_ini_double(L"flow", L"stall_idle_seconds", g_cfg.stall_idle_seconds);
    write_ini_double(L"flow", L"burst_gap_seconds", g_cfg.burst_gap_seconds);
    write_ini_double(L"policy", L"mean_reward_interval_s", g_cfg.mean_reward_interval_s);
    write_ini_double(L"policy", L"min_cooldown_s", g_cfg.min_cooldown_s);
    write_ini_double(L"policy", L"min_flow_hold_s", g_cfg.min_flow_hold_s);
    write_ini_double(L"policy", L"withhold_probability", g_cfg.withhold_probability);
    write_ini_double(L"policy", L"recovery_chars", g_cfg.recovery_chars);
    write_ini_double(L"policy", L"recovery_window_s", g_cfg.recovery_window_s);
    write_ini_double(L"flow", L"grace_seconds", g_cfg.grace_seconds);
    write_ini_double(L"gate", L"slop_repetition_max", g_cfg.slop_repetition_max);
    write_ini_double(L"gate", L"slop_entropy_min", g_cfg.slop_entropy_min);
    write_ini_double(L"gate", L"slop_stall_frac_max", g_cfg.slop_stall_frac_max);
    write_ini_double(L"gate", L"gate_min_chars", g_cfg.gate_min_chars);
    write_ini_double(L"channels", L"visual", g_cfg.visual_enabled ? 1 : 0);
    write_ini_double(L"channels", L"audio", g_cfg.audio_enabled ? 1 : 0);
    write_ini_double(L"channels", L"statusbar", g_cfg.statusbar_enabled ? 1 : 0);
    write_ini_double(L"channels", L"bloom_ms", g_cfg.bloom_ms);
    write_ini_double(L"telemetry", L"raw_log", g_cfg.raw_log_enabled ? 1 : 0);
    write_ini_double(L"telemetry", L"capture_text", g_cfg.capture_text ? 1 : 0);
}

// --------------------------------------------------------------- session ---

std::wstring session_base_name() {
    CreateDirectoryW(g_log_dir.c_str(), nullptr);
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t name[64];
    wcsftime(name, 64, L"session-%Y%m%d-%H%M%S", &tm);
    return g_log_dir + L"\\" + name;
}

void CALLBACK timer_proc(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_engine) return;
    const double t = now_s();
    // Focus tracking: count the transition out of the Notepad++ window.
    HWND fg = GetForegroundWindow();
    const bool here =
        fg && GetAncestor(fg, GA_ROOT) == GetAncestor(g_npp._nppHandle, GA_ROOT);
    if (g_focus_was_here && !here) g_engine->on_focus_loss(t);
    g_focus_was_here = here;
    g_engine->tick(t, here);
    if (g_rawlog) g_rawlog->tick(t, g_engine->state());
}

void start_engine() {
    if (g_engine) return;
    const uint64_t seed = GetTickCount64() ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    const std::wstring base = session_base_name();
    g_engine = std::make_unique<sbpp::FlowEngine>(g_cfg, seed);
    g_engine->add_adapter(std::make_unique<sbpp::LogAdapter>(base + L".jsonl"));
    if (g_cfg.raw_log_enabled)
        g_rawlog = std::make_unique<sbpp::RawLog>(base + L".raw.jsonl",
                                                  g_cfg.capture_text);
    if (g_cfg.visual_enabled || g_cfg.statusbar_enabled)
        g_engine->add_adapter(std::make_unique<sbpp::NppVisualAdapter>(g_npp, g_cfg));
    if (g_cfg.audio_enabled)
        g_engine->add_adapter(std::make_unique<sbpp::AudioAdapter>());
    g_timer = SetTimer(nullptr, 0, 1000, timer_proc);
    g_focus_was_here = true;
}

void stop_engine() {
    if (g_timer) {
        KillTimer(nullptr, g_timer);
        g_timer = 0;
    }
    if (g_engine) {
        g_engine->shutdown();
        g_engine.reset();
    }
    g_rawlog.reset();
}

// ------------------------------------------------------------ menu items ---

void cmd_toggle() {
    g_enabled = !g_enabled;
    if (g_enabled) {
        load_config();
        g_enabled = true; // toggle wins over whatever the INI said
        start_engine();
    } else {
        stop_engine();
    }
    persist_config();
    SendMessage(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, g_funcs[0]._cmdID,
                g_enabled ? TRUE : FALSE);
}

void cmd_reload_config() {
    const bool was_running = g_engine != nullptr;
    stop_engine();
    load_config();
    if (was_running && g_enabled) start_engine();
    SendMessage(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, g_funcs[0]._cmdID,
                g_enabled ? TRUE : FALSE);
}

void cmd_open_config() {
    persist_config(); // make sure the file exists with current values
    SendMessage(g_npp._nppHandle, NPPM_DOOPEN, 0,
                reinterpret_cast<LPARAM>(g_ini_path.c_str()));
}

void cmd_open_logs() {
    CreateDirectoryW(g_log_dir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", g_log_dir.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

void restart_engine_if_running() {
    if (g_engine) {
        stop_engine();
        start_engine();
    }
}

void cmd_toggle_rawlog() {
    g_cfg.raw_log_enabled = !g_cfg.raw_log_enabled;
    persist_config();
    restart_engine_if_running();
    SendMessage(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, g_funcs[4]._cmdID,
                g_cfg.raw_log_enabled ? TRUE : FALSE);
}

void cmd_toggle_capture() {
    g_cfg.capture_text = !g_cfg.capture_text;
    persist_config();
    restart_engine_if_running();
    SendMessage(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, g_funcs[5]._cmdID,
                g_cfg.capture_text ? TRUE : FALSE);
}

// Self-label shortcuts: append to labels.jsonl with current engine state and
// (only when text capture is on) the labeled text — selection if present,
// otherwise the ~240 chars before the caret.
void write_label(const char* label) {
    CreateDirectoryW(g_log_dir.c_str(), nullptr);
    FILE* f = _wfsopen((g_log_dir + L"\\labels.jsonl").c_str(), L"ab", _SH_DENYNO);
    if (!f) return;
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm);
    const sbpp::AmbientState s =
        g_engine ? g_engine->state() : sbpp::AmbientState{};
    std::string out = "{\"ts\":\"";
    out += ts;
    out += "\",\"label\":\"";
    out += label;
    char num[160];
    snprintf(num, sizeof(num),
             "\",\"flow\":%.3f,\"regime\":\"%s\",\"gate_ok\":%s", s.flow,
             sbpp::regime_name(s.regime), s.gate_ok ? "true" : "false");
    out += num;
    if (g_cfg.capture_text) {
        int which = 0;
        SendMessage(g_npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0,
                    reinterpret_cast<LPARAM>(&which));
        HWND sci = which == 0 ? g_npp._scintillaMainHandle
                              : g_npp._scintillaSecondHandle;
        const LRESULT selStart = SendMessage(sci, SCI_GETSELECTIONSTART, 0, 0);
        const LRESULT selEnd = SendMessage(sci, SCI_GETSELECTIONEND, 0, 0);
        LRESULT a = selStart, b = selEnd;
        if (a == b) { // no selection: trailing context before the caret
            b = SendMessage(sci, SCI_GETCURRENTPOS, 0, 0);
            a = b > 240 ? b - 240 : 0;
        }
        if (b > a && b - a < 2000) {
            std::string text(static_cast<size_t>(b - a), '\0');
            Sci_TextRangeFull tr;
            tr.chrg.cpMin = static_cast<Sci_Position>(a);
            tr.chrg.cpMax = static_cast<Sci_Position>(b);
            tr.lpstrText = text.data();
            SendMessage(sci, SCI_GETTEXTRANGEFULL, 0,
                        reinterpret_cast<LPARAM>(&tr));
            out += ",\"text\":\"";
            sbpp::RawLog::append_escaped(out, text.c_str(), text.size());
            out += "\"";
        }
    }
    out += "}\n";
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
}

void cmd_mark_good() { write_label("good"); }
void cmd_mark_meh() { write_label("meh"); }
void cmd_mark_slop() { write_label("slop"); }

void cmd_about() {
    MessageBoxW(g_npp._nppHandle,
                L"SkinnerBox++\n\n"
                L"An operant conditioning chamber for your editor: measures "
                L"writing flow from typing telemetry and reinforces it with "
                L"ambient and phasic rewards.\n\n"
                L"Session logs are metadata-only (no document text).\n\n"
                L"github.com/EcstasyEngineer/skinnerbox-plus-plus",
                L"About SkinnerBox++", MB_OK | MB_ICONINFORMATION);
}

} // namespace

// ------------------------------------------------------------ plugin ABI ---

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) stop_engine();
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode() { return TRUE; }

extern "C" __declspec(dllexport) void setInfo(NppData data) {
    g_npp = data;
    wchar_t config_dir[MAX_PATH] = L"";
    SendMessage(g_npp._nppHandle, NPPM_GETPLUGINSCONFIGDIR, MAX_PATH,
                reinterpret_cast<LPARAM>(config_dir));
    g_ini_path = std::wstring(config_dir) + L"\\SkinnerBoxPP.ini";
    g_log_dir = std::wstring(config_dir) + L"\\SkinnerBoxPP-logs";
    load_config();
}

extern "C" __declspec(dllexport) const TCHAR* getName() { return kPluginName; }

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* count) {
    static ShortcutKey kGood{true, true, false, 'G'};
    static ShortcutKey kMeh{true, true, false, 'M'};
    static ShortcutKey kSlop{true, true, false, 'B'};
    auto set = [](FuncItem& item, const wchar_t* name, PFUNCPLUGINCMD fn,
                  bool checked, ShortcutKey* key = nullptr) {
        wcsncpy_s(item._itemName, name, _TRUNCATE);
        item._pFunc = fn;
        item._init2Check = checked;
        item._pShKey = key;
    };
    set(g_funcs[0], L"Enable SkinnerBox++", cmd_toggle, g_enabled);
    set(g_funcs[1], L"Reload config", cmd_reload_config, false);
    set(g_funcs[2], L"Open config", cmd_open_config, false);
    set(g_funcs[3], L"Open session logs", cmd_open_logs, false);
    set(g_funcs[4], L"Raw telemetry log", cmd_toggle_rawlog, g_cfg.raw_log_enabled);
    set(g_funcs[5], L"Capture text in raw log", cmd_toggle_capture, g_cfg.capture_text);
    set(g_funcs[6], L"Mark: that was good", cmd_mark_good, false, &kGood);
    set(g_funcs[7], L"Mark: merely functional", cmd_mark_meh, false, &kMeh);
    set(g_funcs[8], L"Mark: slop", cmd_mark_slop, false, &kSlop);
    set(g_funcs[9], L"About", cmd_about, false);
    *count = kNbFunc;
    return g_funcs;
}

extern "C" __declspec(dllexport) void beNotified(SCNotification* n) {
    if (!n) return;
    switch (n->nmhdr.code) {
        case NPPN_READY:
            persist_config(); // materialize the INI on first run
            if (g_enabled) start_engine();
            break;
        case NPPN_SHUTDOWN:
            stop_engine();
            break;
        case SCN_MODIFIED: {
            if (!g_engine) break;
            // Only user-performed edits; clamp so file loads and giant pastes
            // don't masquerade as a torrent of typing.
            if (!(n->modificationType & SC_PERFORMED_USER)) break;
            const double t = now_s();
            const auto chars =
                static_cast<uint32_t>(n->length > 256 ? 256 : n->length);
            if (n->modificationType & SC_MOD_INSERTTEXT) {
                g_engine->on_insert(t, chars, n->text, chars);
                if (g_rawlog)
                    g_rawlog->event(t, "ins", n->position, n->length, n->text,
                                    static_cast<size_t>(n->length));
            } else if (n->modificationType & SC_MOD_DELETETEXT) {
                g_engine->on_delete(t, chars);
                if (g_rawlog)
                    g_rawlog->event(t, "del", n->position, n->length, n->text,
                                    static_cast<size_t>(n->length));
            }
            break;
        }
        default:
            break;
    }
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM) {
    return TRUE;
}
