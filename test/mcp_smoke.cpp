// SkinnerBox++ — MCP adapter smoke test.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.
//
// Drives McpAdapter against a device backend (run yours in mock mode) and
// checks the safety invariants that must hold regardless of backend behavior:
// withheld intents actuate nothing, intensity never exceeds the configured
// ceiling, and shutdown stops output.
//
// Usage: mcp_smoke.exe [endpoint]   (default http://127.0.0.1:9102/mcp)

#include <cstdio>
#include <thread>

#include "../src/adapters/mcp_adapter.h"

int main(int argc, char** argv) {
    sbpp::McpAdapter::Settings s;
    s.endpoint = argc > 1 ? argv[1] : "http://127.0.0.1:9102/mcp";
    s.max_intensity = 0.20;
    s.max_seconds = 2;
    sbpp::McpAdapter mcp(s);

    auto intent = [](sbpp::RewardClass k, double dose, bool withheld) {
        sbpp::RewardIntent i;
        i.kind = k;
        i.confidence = 1.0;
        i.dose = dose;
        i.max_duration_ms = 8000;
        i.reason = "smoke";
        i.withheld = withheld;
        return i;
    };

    printf("1. withheld micro_reward (must produce NO device call)\n");
    mcp.deliver(intent(sbpp::RewardClass::MicroReward, 0.9, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    printf("2. low-dose micro_reward -> ACKNOWLEDGE_LOW at <= %.2f\n",
           s.max_intensity * 0.5);
    mcp.deliver(intent(sbpp::RewardClass::MicroReward, 0.3, false));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    printf("3. high-dose micro_reward -> REWARD_MEDIUM clamped to %.2f\n",
           s.max_intensity);
    mcp.deliver(intent(sbpp::RewardClass::MicroReward, 1.0, false));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    printf("4. recovery_reward\n");
    mcp.deliver(intent(sbpp::RewardClass::RecoveryReward, 0.6, false));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    printf("5. shutdown (must send stop)\n");
    mcp.shutdown();
    printf("connected=%d last_error=%s\n", mcp.connected() ? 1 : 0,
           mcp.last_error().c_str());
    printf("\nNow check the backend log: expect 4 tools/call entries (one\n"
           "initialize, no call for step 1), intensities <= %.2f, and a final\n"
           "stop. Any intensity above the ceiling is a FAILURE.\n",
           s.max_intensity);
    return 0;
}
