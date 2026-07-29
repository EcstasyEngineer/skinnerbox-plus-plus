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
        last_error_ = "connect failed (is Intiface Central running?)";
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
    // Walk each "DeviceIndex": n and pair it with the first feature range that
    // follows. v4 advertises ranges as "Value":[min,max] under Output.Vibrate.
    size_t pos = 0;
    while (true) {
        const size_t d = json.find("\"DeviceIndex\":", pos);
        if (d == std::string::npos) break;
        Device dev;
        dev.index = std::atoi(json.c_str() + d + 14);
        const size_t vib = json.find("\"Vibrate\"", d);
        if (vib != std::string::npos) {
            const size_t val = json.find("\"Value\"", vib);
            const size_t lb = json.find('[', val);
            const size_t comma = json.find(',', lb);
            if (lb != std::string::npos && comma != std::string::npos)
                dev.max_value = std::atoi(json.c_str() + comma + 1);
            double fi = 0;
            if (find_number(json, "FeatureIndex", fi, d))
                dev.feature = static_cast<int>(fi);
            if (dev.max_value > 0) devices_.push_back(dev);
        }
        pos = d + 14;
    }
}

void IntifaceAdapter::deliver(const RewardIntent& intent) {
    if (intent.withheld) return; // counterfactual: never actuate
    if (intent.kind == RewardClass::SessionSummary) return;
    const double dose = intent.dose;
    std::thread([this, dose] {
        std::lock_guard<std::mutex> lk(mu_);
        if (!connect() || devices_.empty()) return;
        // Dose maps into the device's own step range, clamped by our ceiling.
        // Buttplug has no server-side cap, so this clamp is the only one.
        for (const Device& d : devices_) {
            const double frac = std::min(dose, 1.0) * cfg_.max_intensity;
            int step = static_cast<int>(frac * d.max_value + 0.5);
            step = std::max(1, std::min(step,
                static_cast<int>(cfg_.max_intensity * d.max_value + 0.5)));
            char cmd[224];
            snprintf(cmd, sizeof(cmd),
                     "[{\"OutputCmd\":{\"Id\":%d,\"DeviceIndex\":%d,"
                     "\"FeatureIndex\":%d,\"Command\":{\"Vibrate\":{\"Value\":%d}}}}]",
                     next_id_++, d.index, d.feature, step);
            send(cmd);
        }
        // We own the stop: OutputCmd has no duration, so a value would persist
        // forever if we didn't explicitly zero it.
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.buzz_ms));
        for (const Device& d : devices_) {
            char cmd[224];
            snprintf(cmd, sizeof(cmd),
                     "[{\"OutputCmd\":{\"Id\":%d,\"DeviceIndex\":%d,"
                     "\"FeatureIndex\":%d,\"Command\":{\"Vibrate\":{\"Value\":0}}}}]",
                     next_id_++, d.index, d.feature);
            send(cmd);
        }
    }).detach();
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
