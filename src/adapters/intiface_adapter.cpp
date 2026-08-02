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
    connect_thread_ = std::thread([this] {
        std::lock_guard<std::mutex> lk(mu_);
        if (running_.load()) connect_unlocked();
    });
}

IntifaceAdapter::~IntifaceAdapter() { shutdown(); }

bool IntifaceAdapter::alive(uint64_t epoch) const {
    return running_.load() && epoch_.load() == epoch;
}

bool IntifaceAdapter::sleep_alive(uint32_t ms, uint64_t epoch) const {
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (!alive(epoch)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return alive(epoch);
}

void IntifaceAdapter::join_ping() {
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ping_ = true;
        t = std::move(ping_thread_);
    }
    if (t.joinable()) t.join();
}

bool IntifaceAdapter::connect_unlocked() {
    if (connected_) return true;
    if (!running_.load()) return false;

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
    if (!send_unlocked(hs)) {
        close_unlocked();
        return false;
    }
    const std::string info = recv_unlocked();
    if (info.find("ServerInfo") == std::string::npos) {
        last_error_ = "no ServerInfo (protocol mismatch?): " + info.substr(0, 160);
        close_unlocked();
        return false;
    }
    double ping = 0;
    if (find_number(info, "MaxPingTime", ping) && ping > 0)
        max_ping_ms_ = static_cast<uint32_t>(ping);

    char dl[96];
    snprintf(dl, sizeof(dl), "[{\"RequestDeviceList\":{\"Id\":%d}}]", next_id_++);
    if (send_unlocked(dl)) parse_device_list(recv_unlocked());

    connected_ = true;
    // Dead-man's switch: keep pinging so the server holds the connection, and
    // so that if this process dies the server stops devices by itself.
    // Caller must have joined any previous ping thread (join_ping) first —
    // assigning to a joinable std::thread is terminate.
    if (max_ping_ms_ > 0 && running_.load()) {
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
        if (stop_ping_ || !connected_ || !running_.load()) return;
        char p[64];
        snprintf(p, sizeof(p), "[{\"Ping\":{\"Id\":%d}}]", next_id_++);
        if (!send_unlocked(p)) return;
    }
}

bool IntifaceAdapter::send_unlocked(const std::string& json) {
    if (!websocket_) return false;
    const DWORD rc = WinHttpWebSocketSend(
        websocket_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<void*>(static_cast<const void*>(json.data())),
        static_cast<DWORD>(json.size()));
    if (rc != NO_ERROR) last_error_ = "websocket send failed";
    return rc == NO_ERROR;
}

// Session-level WinHttpSetTimeouts owns the wait; no per-call override.
std::string IntifaceAdapter::recv_unlocked() {
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

bool IntifaceAdapter::ensure_connected(uint64_t epoch) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!alive(epoch)) return false;
        if (connected_) return true;
    }
    // Previous close may have left a joinable ping thread; join before
    // connect_unlocked assigns a new one.
    join_ping();
    std::lock_guard<std::mutex> lk(mu_);
    if (!alive(epoch)) return false;
    if (connected_) return true;
    return connect_unlocked();
}

bool IntifaceAdapter::set_level_unlocked(double level, bool& io_ok) {
    if (!connected_ || devices_.empty()) return false;
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
        io_ok = send_unlocked(cmd) && io_ok;
    }
    current_output_ = io_ok ? out : 0.0;
    return true;
}

void IntifaceAdapter::ambient(const AmbientState& state) {
    if (!cfg_.flow_vibe) return;
    if (envelope_busy_.load()) return; // reward owns the device; retry next tick
    const double level =
        state.regime == Regime::Flow ? cfg_.flow_vibe_level : 0.0;
    std::unique_lock<std::mutex> lk(mu_, std::try_to_lock);
    if (!lk.owns_lock()) return;
    if (envelope_busy_.load()) return;
    if (level == tonic_level_) return;
    if (level == 0.0 && !connected_) { tonic_level_ = 0.0; return; }
    if (!running_.load()) return;
    // May need re-connect after a dead socket; can't join ping under mu_.
    const bool need_connect = !connected_;
    lk.unlock();
    if (need_connect) {
        if (!ensure_connected(epoch_.load())) return;
    }
    lk.lock();
    if (envelope_busy_.load() || !running_.load()) return;
    if (!connected_ && !connect_unlocked()) return;
    if (devices_.empty()) return;
    bool io_ok = true;
    if (!set_level_unlocked(level, io_ok)) return;
    if (io_ok) {
        tonic_level_ = level;
        if (level == 0.0) current_output_ = 0.0;
    } else {
        close_unlocked(); // dead socket: reconnect on a later tick / next deliver
        tonic_level_ = 0.0;
        current_output_ = 0.0;
    }
}

void IntifaceAdapter::run_envelope(double dose, uint64_t epoch) {
    // Serialize envelopes so concurrent deliver() threads cannot interleave
    // OutputCmd streams. Ambient checks envelope_busy_ and skips.
    std::lock_guard<std::mutex> elk(envelope_mu_);
    if (!alive(epoch)) return;
    if (!ensure_connected(epoch)) return;

    const auto sustain_ms = static_cast<uint32_t>(std::min(
        4000.0, cfg_.buzz_ms * (0.5 + std::min(dose, 1.0))));

    envelope_busy_ = true;
    // RAII-ish zero on every exit path so OutputCmd never sticks after abort.
    auto zero_and_clear = [&] {
        std::lock_guard<std::mutex> lk(mu_);
        bool io_ok = true;
        if (connected_) set_level_unlocked(0.0, io_ok);
        tonic_level_ = 0.0;
        current_output_ = 0.0;
        if (!io_ok) close_unlocked();
        envelope_busy_ = false;
    };

    auto step = [&](double level) -> bool {
        if (!alive(epoch)) return false;
        std::lock_guard<std::mutex> lk(mu_);
        if (!alive(epoch) || !connected_) return false;
        bool io_ok = true;
        if (!set_level_unlocked(level, io_ok)) return false;
        if (!io_ok) {
            close_unlocked();
            return false;
        }
        return true;
    };

    // The reward is erogenous — the pleasure IS the reinforcer, so it is
    // shaped like pleasure, not like a notification brick: quick ease-in,
    // a sustain whose LENGTH carries the reward magnitude, gentle
    // ease-out. Peak intensity is fixed at the cap: inside a tight cap,
    // intensity differences aren't reliably discriminable, and
    // habituation is fought with timing, never intensity. "Better flow →
    // more pleasure time."
    // Mutex is held only around socket send/state — never across sleeps —
    // so the dead-man ping can run during a multi-second envelope.
    for (double f : {0.4, 0.7, 1.0}) { // ease-in, ~200 ms to peak
        if (!step(f)) {
            zero_and_clear();
            return;
        }
        if (!sleep_alive(100, epoch)) {
            zero_and_clear();
            return;
        }
    }
    if (!sleep_alive(sustain_ms, epoch)) {
        zero_and_clear();
        return;
    }
    for (double f : {0.66, 0.33}) { // ease-out, ~300 ms
        if (!step(f)) {
            zero_and_clear();
            return;
        }
        if (!sleep_alive(150, epoch)) {
            zero_and_clear();
            return;
        }
    }
    // We own the stop: OutputCmd has no duration, so a value would persist
    // forever if we didn't explicitly zero it. Ending at zero also resets
    // the cached tonic level, so flow-vibe mode re-asserts its baseline on
    // the next tick instead of trusting a stale cache.
    zero_and_clear();
}

void IntifaceAdapter::deliver(const RewardIntent& intent) {
    if (!running_.load()) return;
    const double dose = intent.dose;
    const uint64_t epoch = epoch_.load();
    std::lock_guard<std::mutex> wlk(workers_mu_);
    if (!running_.load()) return;
    workers_.emplace_back([this, dose, epoch] { run_envelope(dose, epoch); });
}

void IntifaceAdapter::reconnect() {
    // Bump epoch so any in-flight envelope aborts and zeros before we tear
    // down the socket for a fresh handshake.
    epoch_.fetch_add(1);
    join_ping();
    {
        std::lock_guard<std::mutex> lk(mu_);
        close_unlocked();
        if (running_.load()) connect_unlocked();
    }
}

void IntifaceAdapter::close_unlocked() {
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
    if (!running_.exchange(false)) return; // idempotent
    epoch_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ping_ = true;
    }

    if (connect_thread_.joinable()) connect_thread_.join();

    {
        std::lock_guard<std::mutex> wlk(workers_mu_);
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

    join_ping();

    std::lock_guard<std::mutex> lk(mu_);
    if (connected_) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "[{\"StopAllDevices\":{\"Id\":%d}}]",
                 next_id_++);
        send_unlocked(cmd); // fail toward off
    }
    close_unlocked();
    current_output_ = 0.0;
    tonic_level_ = 0.0;
    envelope_busy_ = false;
}

bool IntifaceAdapter::connected() const {
    std::lock_guard<std::mutex> lk(mu_);
    return connected_;
}

std::string IntifaceAdapter::last_error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_error_;
}

size_t IntifaceAdapter::device_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return devices_.size();
}

} // namespace sbpp
