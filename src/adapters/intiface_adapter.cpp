// SkinnerBox++ — Intiface / Buttplug v4 output adapter.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "intiface_adapter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace sbpp {

namespace {

constexpr DWORD kTimeoutMs = 3000;

// Minimal scalar extractor for our own known-shape traffic. Buttplug messages
// are small and we only need a handful of numbers; a real JSON parser would be
// the right call if this ever needs to read arbitrary server output.
bool find_number(const std::string& s, const char* key, double& out,
                 size_t from = 0) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = s.find(k, from);
    if (p == std::string::npos) return false;
    out = std::atof(s.c_str() + p + k.size());
    return true;
}

} // namespace

IntifaceAdapter::IntifaceAdapter(const Settings& s) : cfg_(s) {
    // Connect eagerly on a worker so plugin startup never blocks on a socket.
    std::thread([this] {
        std::lock_guard<std::mutex> lk(mu_);
        connect();
    }).detach();
}

IntifaceAdapter::~IntifaceAdapter() { shutdown(); }

bool IntifaceAdapter::connect() {
    if (connected_) return true;

    std::wstring wurl(cfg_.url.begin(), cfg_.url.end());
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{}, path[512]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 511;
    // WinHttpCrackUrl doesn't know ws://; normalize to http:// for parsing.
    std::wstring parseable = wurl;
    if (parseable.rfind(L"ws://", 0) == 0)
        parseable = L"http://" + parseable.substr(5);
    else if (parseable.rfind(L"wss://", 0) == 0)
        parseable = L"https://" + parseable.substr(6);
    if (!WinHttpCrackUrl(parseable.c_str(), 0, 0, &uc)) {
        last_error_ = "bad url";
        return false;
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET ses = WinHttpOpen(L"SkinnerBoxPP/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        last_error_ = "WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(ses, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) {
        // Intiface Central serves ONE client at a time: the plugin and the
        // console demo can't both hold it.
        last_error_ = "connect failed (Intiface not running, or another "
                      "client — plugin or demo — already connected?)";
        WinHttpCloseHandle(ses);
        return false;
    }
    HINTERNET req = WinHttpOpenRequest(con, L"GET", path[0] ? path : L"/",
                                       nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req ||
        !WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        last_error_ = "websocket upgrade failed";
        if (req) WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);
    if (!ws) {
        last_error_ = "WebSocketCompleteUpgrade failed";
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);
        return false;
    }
    session_ = ses;
    connection_ = con;
    websocket_ = ws;

    // Handshake. v4 renamed the old MessageVersion to explicit major/minor.
    char hs[192];
    snprintf(hs, sizeof(hs),
             "[{\"RequestServerInfo\":{\"Id\":%d,\"ClientName\":\"SkinnerBox++\","
             "\"ProtocolVersionMajor\":4,\"ProtocolVersionMinor\":0}}]",
             next_id_++);
    if (!send(hs)) {
        close();
        return false;
    }
    const std::string info = recv(kTimeoutMs);
    if (info.find("ServerInfo") == std::string::npos) {
        last_error_ = "no ServerInfo (protocol mismatch?): " + info.substr(0, 160);
        close();
        return false;
    }
    double ping = 0;
    if (find_number(info, "MaxPingTime", ping) && ping > 0)
        max_ping_ms_ = static_cast<uint32_t>(ping);

    char dl[96];
    snprintf(dl, sizeof(dl), "[{\"RequestDeviceList\":{\"Id\":%d}}]", next_id_++);
    if (send(dl)) parse_device_list(recv(kTimeoutMs));

    connected_ = true;
    // Dead-man's switch: keep pinging so the server holds the connection, and
    // so that if this process dies the server stops devices by itself.
    if (max_ping_ms_ > 0) {
        stop_ping_ = false;
        ping_thread_ = std::thread([this] { ping_loop(); });
    }
    return true;
}

void IntifaceAdapter::ping_loop() {
    const uint32_t interval = std::max<uint32_t>(200, max_ping_ms_ / 2);
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_ping_ || !connected_) return;
        char p[64];
        snprintf(p, sizeof(p), "[{\"Ping\":{\"Id\":%d}}]", next_id_++);
        if (!send(p)) return;
    }
}

bool IntifaceAdapter::send(const std::string& json) {
    if (!websocket_) return false;
    const DWORD rc = WinHttpWebSocketSend(
        websocket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<void*>(static_cast<const void*>(json.data())),
        static_cast<DWORD>(json.size()));
    if (rc != NO_ERROR) last_error_ = "websocket send failed";
    return rc == NO_ERROR;
}

std::string IntifaceAdapter::recv(uint32_t) {
    if (!websocket_) return "";
    std::string out;
    char buf[4096];
    DWORD read = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
    for (int i = 0; i < 8; ++i) { // bounded: don't spin on a chatty server
        if (WinHttpWebSocketReceive(websocket_, buf, sizeof(buf), &read, &type) !=
            NO_ERROR)
            break;
        out.append(buf, read);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            break; // complete message
    }
    return out;
}

void IntifaceAdapter::parse_device_list(const std::string& json) {
    devices_.clear();
    // Intiface v4 serializes object keys alphabetically, so within a device
    // the feature blocks ("DeviceFeatures" -> "FeatureIndex" -> "Output" ->
    // "Vibrate" -> "Value":[min,max]) come BEFORE that device's own
    // "DeviceIndex". Walk each Vibrate output: its FeatureIndex is the
    // nearest one before it, its DeviceIndex the nearest one after.
    size_t pos = 0;
    while (true) {
        const size_t vib = json.find("\"Vibrate\"", pos);
        if (vib == std::string::npos) break;
        pos = vib + 9;
        Device dev;
        const size_t val = json.find("\"Value\"", vib);
        const size_t lb = val == std::string::npos ? val : json.find('[', val);
        const size_t comma = lb == std::string::npos ? lb : json.find(',', lb);
        if (comma == std::string::npos) continue;
        dev.max_value = std::atoi(json.c_str() + comma + 1);
        const size_t f = json.rfind("\"FeatureIndex\":", vib);
        if (f != std::string::npos)
            dev.feature = std::atoi(json.c_str() + f + 15);
        const size_t d = json.find("\"DeviceIndex\":", vib);
        if (d == std::string::npos) continue;
        dev.index = std::atoi(json.c_str() + d + 14);
        if (dev.max_value > 0) devices_.push_back(dev);
    }
}

void IntifaceAdapter::ambient(const AmbientState& state) {
    if (!cfg_.flow_vibe) return;
    const double level =
        state.regime == Regime::Flow ? cfg_.flow_vibe_level : 0.0;
    std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
    if (!lk.owns_lock()) return; // reward envelope in progress; retry next tick
    if (level == tonic_level_) return;
    if (level == 0.0 && !connected_) { tonic_level_ = 0.0; return; }
    if (!connect() || devices_.empty()) return;
    bool io_ok = true;
    double out = 0.0;
    for (const Device& d : devices_) {
        const int cap_step =
            static_cast<int>(cfg_.max_intensity * d.max_value + 0.5);
        int step = static_cast<int>(level * cap_step + 0.5);
        step = std::min(step, cap_step);
        if (level > 0.0) step = std::max(step, 1);
        out = std::max(out, static_cast<double>(step) / d.max_value);
        char cmd[224];
        snprintf(cmd, sizeof(cmd),
                 "[{\"OutputCmd\":{\"Id\":%d,\"DeviceIndex\":%d,"
                 "\"FeatureIndex\":%d,\"Command\":{\"Vibrate\":{\"Value\":%d}}}}]",
                 next_id_++, d.index, d.feature, step);
        io_ok = send(cmd) && io_ok;
    }
    if (io_ok) {
        tonic_level_ = level;
        current_output_ = level > 0.0 ? out : 0.0;
    } else {
        close(); // dead socket: reconnect on a later tick / next deliver
        tonic_level_ = 0.0;
        current_output_ = 0.0;
    }
}

void IntifaceAdapter::deliver(const RewardIntent& intent) {
    const double dose = intent.dose;
    std::thread([this, dose] {
        std::lock_guard<std::mutex> lk(mu_);
        if (!connect() || devices_.empty()) return;
        // The reward is erogenous — the pleasure IS the reinforcer, so it is
        // shaped like pleasure, not like a notification brick: quick ease-in,
        // a sustain whose LENGTH carries the reward magnitude, gentle
        // ease-out. Peak intensity is fixed at the cap: inside a tight cap,
        // intensity differences aren't reliably discriminable, and
        // habituation is fought with timing, never intensity. "Better flow →
        // more pleasure time."
        const auto sustain_ms = static_cast<uint32_t>(std::min(
            4000.0, cfg_.buzz_ms * (0.5 + std::min(dose, 1.0))));
        bool io_ok = true;
        // level: fraction of the intensity cap, quantized into the device's
        // own step range. Buttplug has no server-side cap; this is the cap.
        auto set_level = [&](double level) {
            double out = 0.0;
            for (const Device& d : devices_) {
                const int cap_step =
                    static_cast<int>(cfg_.max_intensity * d.max_value + 0.5);
                int step = static_cast<int>(level * cap_step + 0.5);
                step = std::min(step, cap_step);
                if (level > 0.0) step = std::max(step, 1);
                out = std::max(out, static_cast<double>(step) / d.max_value);
                char cmd[224];
                snprintf(cmd, sizeof(cmd),
                         "[{\"OutputCmd\":{\"Id\":%d,\"DeviceIndex\":%d,"
                         "\"FeatureIndex\":%d,\"Command\":{\"Vibrate\":{\"Value\":%d}}}}]",
                         next_id_++, d.index, d.feature, step);
                io_ok = send(cmd) && io_ok;
            }
            current_output_ = io_ok ? out : 0.0;
        };
        for (double f : {0.4, 0.7, 1.0}) { // ease-in, ~200 ms to peak
            set_level(f);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(sustain_ms));
        for (double f : {0.66, 0.33}) {    // ease-out, ~300 ms
            set_level(f);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        // We own the stop: OutputCmd has no duration, so a value would persist
        // forever if we didn't explicitly zero it. Ending at zero also resets
        // the cached tonic level, so flow-vibe mode re-asserts its baseline on
        // the next tick instead of trusting a stale cache.
        set_level(0.0);
        tonic_level_ = 0.0;
        // A failed send means the socket is dead (server restart, BLE hiccup).
        // Close now so the next deliver/reconnect re-establishes instead of
        // silently shouting into a corpse.
        if (!io_ok) close();
    }).detach();
}

void IntifaceAdapter::reconnect() {
    std::lock_guard<std::mutex> lk(mu_);
    close();
    connect();
}

void IntifaceAdapter::close() {
    if (websocket_) {
        WinHttpWebSocketClose(websocket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS,
                              nullptr, 0);
        WinHttpCloseHandle(websocket_);
        websocket_ = nullptr;
    }
    if (connection_) {
        WinHttpCloseHandle(connection_);
        connection_ = nullptr;
    }
    if (session_) {
        WinHttpCloseHandle(session_);
        session_ = nullptr;
    }
    connected_ = false;
}

void IntifaceAdapter::shutdown() {
    std::thread ping;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ping_ = true;
        ping = std::move(ping_thread_);
        if (connected_) {
            char cmd[96];
            snprintf(cmd, sizeof(cmd), "[{\"StopAllDevices\":{\"Id\":%d}}]",
                     next_id_++);
            send(cmd); // fail toward off
        }
    }
    if (ping.joinable()) ping.join();
    std::lock_guard<std::mutex> lk(mu_);
    close();
}

} // namespace sbpp
