// SkinnerBox++ — Intiface / Buttplug v4 output adapter.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../core/adapter.h"

namespace sbpp {

// Speaks Buttplug protocol v4 over a WebSocket to Intiface Central (default
// ws://127.0.0.1:12345). Preferred over a bespoke backend for distribution:
// Intiface is what users already run, and it owns all the device/BLE support.
//
// Safety differences from a policy-bearing backend, and how they're handled:
//   * Buttplug has NO server-side intensity cap, so the client ceiling here is
//     the only cap. It is applied in the device's own step space.
//   * OutputCmd has no duration field — a value persists until changed. This
//     adapter therefore owns the stop: every buzz is followed by an explicit
//     zero, and shutdown sends StopAllDevices.
//   * The protocol's ping timeout is the dead-man's switch. If MaxPingTime is
//     nonzero we ping at half that interval; if this plugin dies, pings stop
//     and the server stops devices on its own.
//
// Threading: no detached workers. Connect/deliver run on joinable threads
// tracked by this object; shutdown stops accepting work, joins everyone,
// StopAllDevices, then closes sockets. An epoch counter aborts mid-envelope
// work after shutdown/reconnect so late threads cannot re-command devices.
class IntifaceAdapter : public IOutputAdapter {
public:
    struct Settings {
        std::string url = "ws://127.0.0.1:12345";
        double max_intensity = 0.30; // fraction of each device's advertised max
        uint32_t buzz_ms = 2200;     // base sustain of the reward envelope
        bool flow_vibe = false;      // continuous tonic vibe while in FLOW
        double flow_vibe_level = 0.5; // ...at this fraction of the cap
    };

    explicit IntifaceAdapter(const Settings& s);
    ~IntifaceAdapter() override;

    const char* name() const override { return "intiface"; }
    // Tonic: only acts when flow_vibe mode is on — holds a steady level while
    // the FSM is in FLOW. Never blocks the tick thread: if a reward envelope
    // currently owns the device, the update is skipped and re-asserted later.
    void ambient(const AmbientState& state) override;
    void deliver(const RewardIntent& intent) override;
    void shutdown() override;

    // Drop and re-establish the connection (menu action; safe from any thread).
    void reconnect();

    bool connected() const;
    std::string last_error() const;
    size_t device_count() const;
    // Fraction of max_intensity currently commanded to the device(s): nonzero
    // only during a buzz. Feeds the status-bar "out" readout.
    double current_output() const { return current_output_.load(); }

private:
    struct Device {
        int index = 0;
        int feature = 0;
        int max_value = 0; // upper bound of the feature's advertised range
        std::string name;
    };

    // Network connect + device list. Caller must hold mu_. Ping thread must
    // already be joined (not joinable) before this starts a new one.
    bool connect_unlocked();
    bool send_unlocked(const std::string& json);
    std::string recv_unlocked();
    void close_unlocked();
    void parse_device_list(const std::string& json);
    void ping_loop();
    void join_ping();
    bool ensure_connected(uint64_t epoch);
    void run_envelope(double dose, uint64_t epoch);
    bool sleep_alive(uint32_t ms, uint64_t epoch) const;
    bool set_level_unlocked(double level, bool& io_ok);
    bool alive(uint64_t epoch) const;

    Settings cfg_;
    mutable std::mutex mu_;
    std::atomic<bool> running_{true};
    std::atomic<uint64_t> epoch_{0};
    std::atomic<bool> envelope_busy_{false};
    std::atomic<double> current_output_{0.0};
    double tonic_level_ = 0.0; // last tonic level sent (guarded by mu_)
    void* session_ = nullptr;   // HINTERNET
    void* connection_ = nullptr; // HINTERNET
    void* websocket_ = nullptr;  // HINTERNET (WebSocket handle)
    bool connected_ = false;
    bool stop_ping_ = false;
    uint32_t max_ping_ms_ = 0;
    std::thread connect_thread_;
    std::thread ping_thread_;
    std::mutex workers_mu_;
    std::vector<std::thread> workers_;
    std::mutex envelope_mu_; // at most one reward envelope at a time
    std::vector<Device> devices_;
    std::string last_error_;
    int next_id_ = 1;
};

} // namespace sbpp
