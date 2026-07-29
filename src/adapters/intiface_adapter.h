// SkinnerBox++ — Intiface / Buttplug v4 output adapter.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

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
// Withheld intents never produce device traffic.
class IntifaceAdapter : public IOutputAdapter {
public:
    struct Settings {
        std::string url = "ws://127.0.0.1:12345";
        double max_intensity = 0.30; // fraction of each device's advertised max
        uint32_t buzz_ms = 1200;
    };

    explicit IntifaceAdapter(const Settings& s);
    ~IntifaceAdapter() override;

    const char* name() const override { return "intiface"; }
    void ambient(const AmbientState&) override {} // tonic is not sent to hardware
    void deliver(const RewardIntent& intent) override;
    void shutdown() override;

    bool connected() const { return connected_; }
    const std::string& last_error() const { return last_error_; }
    size_t device_count() const { return devices_.size(); }

private:
    struct Device {
        int index = 0;
        int feature = 0;
        int max_value = 0; // upper bound of the feature's advertised range
        std::string name;
    };

    bool connect();          // handshake + device enumeration
    bool send(const std::string& json);
    std::string recv(uint32_t timeout_ms);
    void close();
    void parse_device_list(const std::string& json);
    void ping_loop();

    Settings cfg_;
    std::mutex mu_;
    void* session_ = nullptr;   // HINTERNET
    void* connection_ = nullptr; // HINTERNET
    void* websocket_ = nullptr;  // HINTERNET (WebSocket handle)
    bool connected_ = false;
    bool stop_ping_ = false;
    uint32_t max_ping_ms_ = 0;
    std::thread ping_thread_;
    std::vector<Device> devices_;
    std::string last_error_;
    int next_id_ = 1;
};

} // namespace sbpp
