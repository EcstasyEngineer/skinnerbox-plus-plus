// SkinnerBox++ — MCP output adapter (hardware rewards via a device backend).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <mutex>
#include <string>

#include "../core/adapter.h"

namespace sbpp {

// Speaks Streamable-HTTP MCP JSON-RPC (initialize / tools/call) to a local
// device backend and maps RewardIntents onto named presets. See
// docs/output-contract.md.
//
// This adapter never sends amplitudes, waveforms, or durations of its own
// choosing: a preset name resolves LOCALLY to a bounded call whose ceiling is
// configured here and independently enforced by the backend. It also:
//   - never actuates a withheld intent,
//   - refuses to raise intensity beyond the configured per-preset ceiling,
//   - always passes a bounded auto-stop duration (never open-ended output),
//   - sends a stop on shutdown (fail toward off),
//   - runs every request on a worker thread with a short timeout so the
//     editor's UI thread never blocks on the network,
//   - stays disarmed until explicitly armed for the session.
class McpAdapter : public IOutputAdapter {
public:
    struct Settings {
        std::string endpoint;     // e.g. "http://127.0.0.1:9102/mcp"
        double max_intensity = 0.30; // hard client-side ceiling (0-1)
        uint32_t max_seconds = 3;    // auto-stop bound per delivery
        int channel = 0;
    };

    explicit McpAdapter(const Settings& s) : cfg_(s) {}
    ~McpAdapter() override;

    const char* name() const override { return "mcp"; }
    void ambient(const AmbientState&) override {} // tonic is not streamed
    void deliver(const RewardIntent& intent) override;
    void shutdown() override;

    // True once initialize succeeded at least once.
    bool connected() const { return connected_; }
    const std::string& last_error() const { return last_error_; }

private:
    // Fire-and-forget POST of one JSON-RPC request; returns body or "".
    std::string post(const std::string& body, const char* mcp_session = nullptr);
    bool ensure_session();
    void call_preset(const char* preset, double intensity, uint32_t seconds);

    Settings cfg_;
    // Deliveries run on detached worker threads and shutdown() can arrive from
    // the UI thread; one lock serializes all backend traffic and shared state.
    std::mutex mu_;
    bool connected_ = false;
    std::string session_id_;
    std::string last_error_;
    int next_id_ = 1;
};

} // namespace sbpp
