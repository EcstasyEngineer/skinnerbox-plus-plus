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

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>

#include "../npp/PluginInterface.h"
#include "../npp/Scintilla.h"
#include "../core/config.h"
#include "../core/engine.h"
#include "../adapters/log_adapter.h"
#include "../adapters/intiface_adapter.h"
#include "../adapters/raw_log.h"
#include "../lab/gpt2_client.h"
#include "../lab/gpt2_bridge.h"
#include "npp_visual_adapter.h"

namespace {

const wchar_t kPluginName[] = L"SkinnerBox++";
constexpr int kNbFunc = 10;

NppData g_npp;
FuncItem g_funcs[kNbFunc];
sbpp::Config g_cfg;
std::unique_ptr<sbpp::FlowEngine> g_engine;
std::unique_ptr<sbpp::RawLog> g_rawlog;
sbpp::IntifaceAdapter* g_intiface = nullptr; // owned by the engine
std::unique_ptr<sbpp::Gpt2LabClient> g_gpt2;
std::unique_ptr<sbpp::Gpt2Bridge> g_gpt2_bridge;
UINT_PTR g_timer = 0;
bool g_enabled = false;
bool g_focus_was_here = true;
std::wstring g_ini_path;
std::wstring g_log_dir;
std::wstring g_intiface_url;
std::wstring g_lab_python; // empty → auto-detect
std::wstring g_lab_host;   // empty → auto-detect gpt2_lab_host.py
HINSTANCE g_hmod = nullptr;

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

// Felt dials (issue #14) compile onto raw policy/hardware keys. Call after
// reading both the felt key and the raw key so load and any runtime toggle
// share one path. min_flow_hold_s stays independent: eligibility latency,
// not reward density.
void compile_felt_dials(double generosity, double strength) {
    g_cfg.min_cooldown_s = std::max(1.0, g_cfg.min_cooldown_s);
    if (generosity > 0.0)
        g_cfg.mean_reward_interval_s =
            std::max(600.0 / generosity, g_cfg.min_cooldown_s);
    g_cfg.mean_reward_interval_s =
        std::max(g_cfg.mean_reward_interval_s, g_cfg.min_cooldown_s);
    if (strength >= 0.0)
        g_cfg.intiface_max_intensity = std::min(1.0, std::max(0.0, strength));
    g_cfg.intiface_max_intensity =
        std::min(1.0, std::max(0.0, g_cfg.intiface_max_intensity));
}

double generosity_from_mean() {
    return 600.0 / std::max(g_cfg.mean_reward_interval_s, 1.0);
}

void load_config() {
    sbpp::Config d; // defaults
    g_enabled = read_ini_double(L"general", L"enabled", 1) != 0;
    g_cfg.target_net_cpm = read_ini_double(L"flow", L"target_net_cpm", d.target_net_cpm);
    g_cfg.ewma_alpha = read_ini_double(L"flow", L"ewma_alpha", d.ewma_alpha);
    g_cfg.flow_enter = read_ini_double(L"flow", L"flow_enter", d.flow_enter);
    g_cfg.flow_exit = read_ini_double(L"flow", L"flow_exit", d.flow_exit);
    g_cfg.burst_gap_seconds =
        read_ini_double(L"flow", L"burst_gap_seconds", d.burst_gap_seconds);
    g_cfg.grace_seconds = read_ini_double(L"flow", L"grace_seconds", d.grace_seconds);
    g_cfg.idle_seconds = read_ini_double(L"flow", L"idle_seconds", d.idle_seconds);
    g_cfg.slop_repetition_max =
        read_ini_double(L"gate", L"slop_repetition_max", d.slop_repetition_max);
    g_cfg.slop_entropy_min =
        read_ini_double(L"gate", L"slop_entropy_min", d.slop_entropy_min);
    g_cfg.slop_stall_frac_max =
        read_ini_double(L"gate", L"slop_stall_frac_max", d.slop_stall_frac_max);
    g_cfg.slop_bigram_bpc_max =
        read_ini_double(L"gate", L"slop_bigram_bpc_max", d.slop_bigram_bpc_max);
    g_cfg.gate_min_chars = read_ini_double(L"gate", L"gate_min_chars", d.gate_min_chars);
    g_cfg.vi_reward_enabled =
        read_ini_double(L"policy", L"vi_reward", d.vi_reward_enabled ? 1 : 0) != 0;
    g_cfg.min_flow_hold_s = read_ini_double(L"policy", L"min_flow_hold_s", d.min_flow_hold_s);
    g_cfg.mean_reward_interval_s =
        read_ini_double(L"policy", L"mean_reward_interval_s", d.mean_reward_interval_s);
    g_cfg.min_cooldown_s = read_ini_double(L"policy", L"min_cooldown_s", d.min_cooldown_s);
    // generosity canonical when present (>0); same persist contract as strength.
    const double generosity = read_ini_double(L"policy", L"generosity", 0);
    g_cfg.visual_enabled = read_ini_double(L"channels", L"visual", 1) != 0;
    g_cfg.statusbar_enabled = read_ini_double(L"channels", L"statusbar", 1) != 0;
    g_cfg.bloom_ms =
        static_cast<uint32_t>(read_ini_double(L"channels", L"bloom_ms", d.bloom_ms));
    g_cfg.bloom_lift = read_ini_double(L"channels", L"bloom_lift", d.bloom_lift);
    g_cfg.message_ms = static_cast<uint32_t>(
        read_ini_double(L"channels", L"message_ms", d.message_ms));
    g_cfg.intiface_enabled =
        read_ini_double(L"intiface", L"enabled", d.intiface_enabled ? 1 : 0) != 0;
    g_cfg.intiface_max_intensity = read_ini_double(
        L"intiface", L"max_intensity", d.intiface_max_intensity);
    const double strength = read_ini_double(L"intiface", L"strength", -1);
    g_cfg.intiface_ms = static_cast<uint32_t>(
        read_ini_double(L"intiface", L"buzz_ms", d.intiface_ms));
    g_cfg.intiface_flow_vibe =
        read_ini_double(L"intiface", L"flow_vibe", d.intiface_flow_vibe ? 1 : 0) != 0;
    g_cfg.intiface_flow_vibe_level = read_ini_double(
        L"intiface", L"flow_vibe_level", d.intiface_flow_vibe_level);
    {
        wchar_t buf[256];
        GetPrivateProfileStringW(L"intiface", L"url", L"ws://127.0.0.1:12345",
                                 buf, 256, g_ini_path.c_str());
        g_intiface_url = buf;
    }
    g_cfg.debug_telemetry = read_ini_double(L"telemetry", L"debug", 0) != 0;
    g_cfg.advanced_debug =
        read_ini_double(L"telemetry", L"advanced_debug", 0) != 0;
    // Mutually exclusive lab arms — advanced wins if both somehow set.
    if (g_cfg.advanced_debug && g_cfg.debug_telemetry)
        g_cfg.debug_telemetry = false;
    {
        wchar_t buf[MAX_PATH];
        GetPrivateProfileStringW(L"lab", L"python", L"", buf, MAX_PATH,
                                 g_ini_path.c_str());
        g_lab_python = buf;
        GetPrivateProfileStringW(L"lab", L"host", L"", buf, MAX_PATH,
                                 g_ini_path.c_str());
        g_lab_host = buf;
    }
    compile_felt_dials(generosity, strength);
}

void persist_config() {
    write_ini_double(L"general", L"enabled", g_enabled ? 1 : 0);
    write_ini_double(L"flow", L"target_net_cpm", g_cfg.target_net_cpm);
    write_ini_double(L"flow", L"ewma_alpha", g_cfg.ewma_alpha);
    write_ini_double(L"flow", L"flow_enter", g_cfg.flow_enter);
    write_ini_double(L"flow", L"flow_exit", g_cfg.flow_exit);
    write_ini_double(L"flow", L"burst_gap_seconds", g_cfg.burst_gap_seconds);
    write_ini_double(L"flow", L"grace_seconds", g_cfg.grace_seconds);
    write_ini_double(L"flow", L"idle_seconds", g_cfg.idle_seconds);
    write_ini_double(L"gate", L"slop_repetition_max", g_cfg.slop_repetition_max);
    write_ini_double(L"gate", L"slop_entropy_min", g_cfg.slop_entropy_min);
    write_ini_double(L"gate", L"slop_stall_frac_max", g_cfg.slop_stall_frac_max);
    write_ini_double(L"gate", L"slop_bigram_bpc_max", g_cfg.slop_bigram_bpc_max);
    write_ini_double(L"gate", L"gate_min_chars", g_cfg.gate_min_chars);
    write_ini_double(L"policy", L"vi_reward", g_cfg.vi_reward_enabled ? 1 : 0);
    write_ini_double(L"policy", L"min_flow_hold_s", g_cfg.min_flow_hold_s);
    write_ini_double(L"policy", L"mean_reward_interval_s", g_cfg.mean_reward_interval_s);
    write_ini_double(L"policy", L"min_cooldown_s", g_cfg.min_cooldown_s);
    // Felt dials are persisted alongside their compiled raw keys so the INI
    // never holds two disagreeing sources of truth (issue #14).
    write_ini_double(L"policy", L"generosity", generosity_from_mean());
    write_ini_double(L"channels", L"visual", g_cfg.visual_enabled ? 1 : 0);
    write_ini_double(L"channels", L"statusbar", g_cfg.statusbar_enabled ? 1 : 0);
    write_ini_double(L"channels", L"bloom_ms", g_cfg.bloom_ms);
    write_ini_double(L"channels", L"bloom_lift", g_cfg.bloom_lift);
    write_ini_double(L"channels", L"message_ms", g_cfg.message_ms);
    write_ini_double(L"intiface", L"enabled", g_cfg.intiface_enabled ? 1 : 0);
    write_ini_double(L"intiface", L"max_intensity", g_cfg.intiface_max_intensity);
    write_ini_double(L"intiface", L"strength", g_cfg.intiface_max_intensity);
    write_ini_double(L"intiface", L"buzz_ms", g_cfg.intiface_ms);
    write_ini_double(L"intiface", L"flow_vibe", g_cfg.intiface_flow_vibe ? 1 : 0);
    write_ini_double(L"intiface", L"flow_vibe_level", g_cfg.intiface_flow_vibe_level);
    WritePrivateProfileStringW(L"intiface", L"url", g_intiface_url.c_str(),
                               g_ini_path.c_str());
    write_ini_double(L"telemetry", L"debug", g_cfg.debug_telemetry ? 1 : 0);
    write_ini_double(L"telemetry", L"advanced_debug",
                     g_cfg.advanced_debug ? 1 : 0);
    WritePrivateProfileStringW(L"lab", L"python", g_lab_python.c_str(),
                               g_ini_path.c_str());
    WritePrivateProfileStringW(L"lab", L"host", g_lab_host.c_str(),
                               g_ini_path.c_str());
}

// Resolve GPT-2 lab host paths.
// Prefer: INI → %APPDATA%\...\SkinnerBoxPP-lab\ (tools\setup_lab.ps1) →
// plugin-dir\lab\ → walk-up to experiments\ (dev checkout).
std::wstring module_dir() {
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(g_hmod, path, MAX_PATH);
    std::wstring p(path);
    const auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

bool file_exists_w(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring config_lab_dir() {
    // Same tree as the INI (NPPM_GETPLUGINSCONFIGDIR\SkinnerBoxPP-lab).
    if (g_ini_path.empty()) return {};
    const auto slash = g_ini_path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return g_ini_path.substr(0, slash) + L"\\SkinnerBoxPP-lab";
}

std::wstring resolve_lab_host() {
    if (!g_lab_host.empty() && file_exists_w(g_lab_host)) return g_lab_host;
    const std::wstring cfg_host = config_lab_dir() + L"\\gpt2_lab_host.py";
    if (file_exists_w(cfg_host)) return cfg_host;
    const std::wstring dir = module_dir();
    const std::wstring candidates[] = {
        dir + L"\\lab\\gpt2_lab_host.py",
        dir + L"\\..\\..\\experiments\\gpt2_lab_host.py",
    };
    std::wstring walk = dir;
    for (int i = 0; i < 6; ++i) {
        const std::wstring try_p = walk + L"\\experiments\\gpt2_lab_host.py";
        if (file_exists_w(try_p)) return try_p;
        const auto slash = walk.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        walk = walk.substr(0, slash);
    }
    for (const auto& c : candidates)
        if (file_exists_w(c)) return c;
    return {};
}

std::wstring resolve_lab_python(const std::wstring& host) {
    if (!g_lab_python.empty() && file_exists_w(g_lab_python)) return g_lab_python;
    // setup_lab.ps1 may leave a venv pointer next to the host scripts.
    const std::wstring cfg_py = config_lab_dir() + L"\\python.exe";
    if (file_exists_w(cfg_py)) return cfg_py;
    if (!host.empty()) {
        const auto slash = host.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            const std::wstring base = host.substr(0, slash);
            // SkinnerBoxPP-lab\ has no venv; experiments\ does.
            const std::wstring vpy = base + L"\\.venv\\Scripts\\python.exe";
            if (file_exists_w(vpy)) return vpy;
            // Sibling: config\SkinnerBoxPP-lab → walk to repo is uncommon;
            // prefer INI. Also try host's parent parent experiments layout.
        }
    }
    const std::wstring dir = module_dir();
    const std::wstring local = dir + L"\\lab\\python.exe";
    if (file_exists_w(local)) return local;
    return L"py";
}

void stop_gpt2_lab() {
    if (g_engine) g_engine->set_gpt2_lab(nullptr);
    g_gpt2_bridge.reset();
    if (g_gpt2) {
        g_gpt2->stop();
        g_gpt2.reset();
    }
}

// Parse {"present":true|false} from a host check/download reply.
bool lab_json_present(const std::string& line, bool& present) {
    const auto p = line.find("\"present\":");
    if (p == std::string::npos) return false;
    auto i = p + 10;
    while (i < line.size() && line[i] == ' ') ++i;
    if (line.compare(i, 4, "true") == 0) {
        present = true;
        return true;
    }
    if (line.compare(i, 5, "false") == 0) {
        present = false;
        return true;
    }
    return false;
}

// Resolve host/python, ensure GPT-2 is cached (prompt to download if not).
// Returns false if the user declines or something fails — caller must leave
// advanced_debug off. Weights are never shipped with the plugin.
bool ensure_lab_model() {
    const std::wstring host = resolve_lab_host();
    if (host.empty()) {
        MessageBoxW(g_npp._nppHandle,
                    L"Advanced debug (GPT-2 lab) could not find gpt2_lab_host.py.\n\n"
                    L"Set [lab] host= in the INI to the full path of\n"
                    L"experiments\\gpt2_lab_host.py, and [lab] python= to the\n"
                    L"venv python that has torch+transformers.",
                    L"SkinnerBox++", MB_OK | MB_ICONWARNING);
        return false;
    }
    const std::wstring py = resolve_lab_python(host);
    std::string resp, err;
    if (!sbpp::Gpt2LabClient::oneshot(py, host, "{\"op\":\"check\"}", resp, err,
                                      60000)) {
        wchar_t msg[512];
        swprintf(msg, 512,
                 L"Could not check for the GPT-2 lab model.\n\npython: %s\n"
                 L"host: %s\n\n%hs",
                 py.c_str(), host.c_str(),
                 err.empty() ? "unknown error" : err.c_str());
        MessageBoxW(g_npp._nppHandle, msg, L"SkinnerBox++",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    bool present = false;
    if (!lab_json_present(resp, present)) {
        wchar_t msg[384];
        swprintf(msg, 384, L"Unexpected check response from lab host:\n\n%hs",
                 resp.c_str());
        MessageBoxW(g_npp._nppHandle, msg, L"SkinnerBox++",
                    MB_OK | MB_ICONWARNING);
        return false;
    }
    if (present) return true;

    const int choice = MessageBoxW(
        g_npp._nppHandle,
        L"Advanced debug needs GPT-2 124M (~500 MB) for offline surprisal "
        L"scoring.\n\n"
        L"The model is NOT shipped with SkinnerBox++. Download it once from "
        L"Hugging Face into your local cache?\n\n"
        L"• One-time download, stays on this machine\n"
        L"• Never uploaded; never used for rewards\n"
        L"• Requires network + the lab Python venv (torch)\n\n"
        L"Download now?",
        L"SkinnerBox++ — download GPT-2 lab model?",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (choice != IDYES) return false;

    MessageBoxW(g_npp._nppHandle,
                L"Downloading GPT-2. This can take a few minutes.\n\n"
                L"Notepad++ may look busy until the download finishes — "
                L"that is expected.",
                L"SkinnerBox++", MB_OK | MB_ICONINFORMATION);

    // 0 = no timeout; first HF pull can be slow on a thin link.
    if (!sbpp::Gpt2LabClient::oneshot(py, host, "{\"op\":\"download\"}", resp,
                                      err, 0)) {
        wchar_t msg[512];
        swprintf(msg, 512, L"GPT-2 download failed.\n\n%hs",
                 err.empty() ? "unknown error" : err.c_str());
        MessageBoxW(g_npp._nppHandle, msg, L"SkinnerBox++",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    present = false;
    if (!lab_json_present(resp, present) || !present) {
        std::string detail = resp;
        const auto ep = resp.find("\"error\":\"");
        if (ep != std::string::npos) {
            detail = resp.substr(ep + 9);
            const auto end = detail.find('"');
            if (end != std::string::npos) detail = detail.substr(0, end);
        }
        wchar_t msg[512];
        swprintf(msg, 512, L"GPT-2 download did not complete.\n\n%hs",
                 detail.c_str());
        MessageBoxW(g_npp._nppHandle, msg, L"SkinnerBox++",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    MessageBoxW(g_npp._nppHandle,
                L"GPT-2 is ready. Advanced debug will use the local cache only.",
                L"SkinnerBox++", MB_OK | MB_ICONINFORMATION);
    return true;
}

void start_gpt2_lab() {
    stop_gpt2_lab();
    if (!g_cfg.advanced_debug) return;
    // Model must already be cached (ensure_lab_model ran when arming LAB).
    // If INI had advanced_debug=1 from a previous session but the cache was
    // wiped, re-prompt rather than silently hitting the network.
    if (!ensure_lab_model()) {
        g_cfg.advanced_debug = false;
        persist_config();
        return;
    }
    const std::wstring host = resolve_lab_host();
    const std::wstring py = resolve_lab_python(host);
    g_gpt2 = std::make_unique<sbpp::Gpt2LabClient>();
    if (!g_gpt2->start(py, host)) {
        std::string err = g_gpt2->last_error();
        g_gpt2.reset();
        wchar_t msg[512];
        swprintf(msg, 512,
                 L"Failed to start GPT-2 lab host.\n\npython: %s\nhost: %s\n\n%hs",
                 py.c_str(), host.c_str(),
                 err.empty() ? "CreateProcess failed" : err.c_str());
        MessageBoxW(g_npp._nppHandle, msg, L"SkinnerBox++",
                    MB_OK | MB_ICONWARNING);
        g_cfg.advanced_debug = false;
        persist_config();
        return;
    }
    g_gpt2_bridge = std::make_unique<sbpp::Gpt2Bridge>(*g_gpt2);
    if (g_engine) g_engine->set_gpt2_lab(g_gpt2_bridge.get());
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
    {
        auto log = std::make_unique<sbpp::LogAdapter>(base + L".jsonl");
        log->log_config(generosity_from_mean(), g_cfg.mean_reward_interval_s,
                        g_cfg.min_flow_hold_s, g_cfg.intiface_max_intensity);
        g_engine->add_adapter(std::move(log));
    }
    // REC arm: full raw text event stream. Exclusive with advanced_debug.
    if (g_cfg.debug_telemetry && !g_cfg.advanced_debug)
        g_rawlog = std::make_unique<sbpp::RawLog>(base + L".raw.jsonl",
                                                  /*capture_text=*/true);
    if (g_cfg.intiface_enabled) {
        sbpp::IntifaceAdapter::Settings is;
        is.url.assign(g_intiface_url.begin(), g_intiface_url.end());
        is.max_intensity = g_cfg.intiface_max_intensity;
        is.buzz_ms = g_cfg.intiface_ms;
        is.flow_vibe = g_cfg.intiface_flow_vibe;
        is.flow_vibe_level = g_cfg.intiface_flow_vibe_level;
        auto hw = std::make_unique<sbpp::IntifaceAdapter>(is);
        g_intiface = hw.get();
        g_engine->add_adapter(std::move(hw));
    }
    if (g_cfg.visual_enabled || g_cfg.statusbar_enabled)
        g_engine->add_adapter(
            std::make_unique<sbpp::NppVisualAdapter>(g_npp, g_cfg, g_intiface));
    if (g_cfg.advanced_debug) start_gpt2_lab();
    g_timer = SetTimer(nullptr, 0, 1000, timer_proc);
    g_focus_was_here = true;
}

void stop_engine() {
    if (g_timer) {
        KillTimer(nullptr, g_timer);
        g_timer = 0;
    }
    stop_gpt2_lab();
    g_intiface = nullptr; // engine shutdown destroys it
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

void sync_lab_menu_checks() {
    SendMessage(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, g_funcs[4]._cmdID,
                g_cfg.debug_telemetry ? TRUE : FALSE);
    SendMessage(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, g_funcs[5]._cmdID,
                g_cfg.advanced_debug ? TRUE : FALSE);
}

void cmd_toggle_debug() {
    // REC arm. Mutually exclusive with advanced (LAB) debug.
    g_cfg.debug_telemetry = !g_cfg.debug_telemetry;
    if (g_cfg.debug_telemetry) g_cfg.advanced_debug = false;
    persist_config();
    if (g_engine) {
        stop_engine();
        start_engine();
    }
    sync_lab_menu_checks();
}

void cmd_toggle_advanced() {
    // LAB arm: GPT-2 surprisal numbers. Exclusive with REC raw text log.
    // Turning ON requires a local model cache — prompt to download if missing.
    if (!g_cfg.advanced_debug) {
        if (!ensure_lab_model()) {
            g_cfg.advanced_debug = false;
            persist_config();
            sync_lab_menu_checks();
            return;
        }
        g_cfg.advanced_debug = true;
        g_cfg.debug_telemetry = false;
    } else {
        g_cfg.advanced_debug = false;
    }
    persist_config();
    if (g_engine) {
        stop_engine();
        start_engine();
    } else if (g_enabled && g_cfg.advanced_debug) {
        // Engine was off; user only armed LAB — still fine, starts with enable.
    }
    sync_lab_menu_checks();
}

void cmd_toggle_flow_vibe() {
    g_cfg.intiface_flow_vibe = !g_cfg.intiface_flow_vibe;
    persist_config();
    if (g_engine) { // the Intiface adapter snapshots settings at engine start
        stop_engine();
        start_engine();
    }
    SendMessage(g_npp._nppHandle, NPPM_SETMENUITEMCHECK, g_funcs[6]._cmdID,
                g_cfg.intiface_flow_vibe ? TRUE : FALSE);
}

void cmd_intiface_test() {
    if (!g_intiface) {
        MessageBoxW(g_npp._nppHandle,
                    L"Intiface channel is not active.\n\nEnable it in the INI "
                    L"([intiface] enabled=1) and reload config, with the "
                    L"engine enabled.",
                    L"SkinnerBox++", MB_OK | MB_ICONWARNING);
        return;
    }
    // A direct adapter poke, bypassing the policy: this answers "is the
    // hardware path alive", nothing about flow.
    sbpp::RewardIntent test;
    test.kind = sbpp::RewardClass::MicroReward;
    test.confidence = 1.0;
    test.dose = 0.5;
    test.max_duration_ms = g_cfg.intiface_ms;
    test.reason = "manual_test_buzz";
    g_intiface->deliver(test);
    wchar_t msg[256];
    if (g_intiface->connected()) {
        swprintf(msg, 256,
                 L"Test buzz sent (%zu device(s), dose 0.5, capped at %.0f%%).",
                 g_intiface->device_count(),
                 g_cfg.intiface_max_intensity * 100.0);
    } else {
        const std::string err = g_intiface->last_error();
        swprintf(msg, 256,
                 L"Not connected — attempting now (watch the status bar).\n\n"
                 L"Last error: %hs",
                 err.empty() ? "(none)" : err.c_str());
    }
    MessageBoxW(g_npp._nppHandle, msg, L"SkinnerBox++ — Intiface",
                MB_OK | MB_ICONINFORMATION);
}

void cmd_intiface_reconnect() {
    if (!g_intiface) return;
    g_intiface->reconnect();
    wchar_t msg[256];
    if (g_intiface->connected()) {
        swprintf(msg, 256, L"Reconnected: %zu vibrating device(s).",
                 g_intiface->device_count());
    } else {
        const std::string err = g_intiface->last_error();
        swprintf(msg, 256, L"Reconnect FAILED: %hs",
                 err.empty() ? "(unknown)" : err.c_str());
    }
    MessageBoxW(g_npp._nppHandle, msg, L"SkinnerBox++ — Intiface",
                MB_OK | MB_ICONINFORMATION);
}

void cmd_about() {
    MessageBoxW(g_npp._nppHandle,
                L"SkinnerBox++\n\n"
                L"An operant conditioning chamber for your editor: measures "
                L"typing momentum and content entropy, and reinforces FLOW "
                L"with in-editor warmth and Intiface hardware rewards.\n\n"
                L"Session logs are metadata-only (no document text) unless "
                L"Debug telemetry (REC) is armed.\n\n"
                L"Advanced debug (LAB) streams GPT-2 surprisal numbers for "
                L"offline quality work — never into the reward policy.\n\n"
                L"github.com/EcstasyEngineer/skinnerbox-plus-plus",
                L"About SkinnerBox++", MB_OK | MB_ICONINFORMATION);
}

} // namespace

// ------------------------------------------------------------ plugin ABI ---

BOOL APIENTRY DllMain(HMODULE hmod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_hmod = hmod;
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
    auto set = [](FuncItem& item, const wchar_t* name, PFUNCPLUGINCMD fn,
                  bool checked) {
        wcsncpy_s(item._itemName, name, _TRUNCATE);
        item._pFunc = fn;
        item._init2Check = checked;
        item._pShKey = nullptr;
    };
    set(g_funcs[0], L"Enable SkinnerBox++", cmd_toggle, g_enabled);
    set(g_funcs[1], L"Reload config", cmd_reload_config, false);
    set(g_funcs[2], L"Open config", cmd_open_config, false);
    set(g_funcs[3], L"Open session logs", cmd_open_logs, false);
    set(g_funcs[4], L"Debug telemetry REC (records typed text)",
        cmd_toggle_debug, g_cfg.debug_telemetry);
    set(g_funcs[5], L"Advanced debug LAB (GPT-2 numbers)", cmd_toggle_advanced,
        g_cfg.advanced_debug);
    set(g_funcs[6], L"Intiface: vibe while in FLOW", cmd_toggle_flow_vibe,
        g_cfg.intiface_flow_vibe);
    set(g_funcs[7], L"Intiface: test buzz", cmd_intiface_test, false);
    set(g_funcs[8], L"Intiface: reconnect", cmd_intiface_reconnect, false);
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
        case NPPN_BUFFERACTIVATED:
            // Tab switches: segment the event stream by document so offline
            // reconstruction never interleaves positions across buffers.
            if (g_rawlog)
                g_rawlog->event(now_s(), "buf",
                                static_cast<long long>(n->nmhdr.idFrom), 0,
                                nullptr, 0);
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
