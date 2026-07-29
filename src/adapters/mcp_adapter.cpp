// SkinnerBox++ — MCP output adapter (hardware rewards via a device backend).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "mcp_adapter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <thread>

namespace sbpp {

namespace {

constexpr DWORD kTimeoutMs = 2500;

struct Url {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
    bool ok = false;
};

Url parse_url(const std::string& s) {
    Url u;
    std::wstring w(s.begin(), s.end());
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof(c);
    wchar_t host[256]{}, path[512]{};
    c.lpszHostName = host;
    c.dwHostNameLength = 255;
    c.lpszUrlPath = path;
    c.dwUrlPathLength = 511;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &c)) return u;
    u.host = host;
    u.path = path[0] ? path : L"/";
    u.port = c.nPort;
    u.https = c.nScheme == INTERNET_SCHEME_HTTPS;
    u.ok = true;
    return u;
}

std::string header_value(HINTERNET req, const wchar_t* name) {
    DWORD len = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, nullptr, &len,
                        WINHTTP_NO_HEADER_INDEX);
    if (len == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return "";
    std::wstring buf(len / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, buf.data(), &len,
                             WINHTTP_NO_HEADER_INDEX))
        return "";
    std::string out;
    for (wchar_t ch : buf)
        if (ch) out.push_back(static_cast<char>(ch));
    return out;
}

} // namespace

McpAdapter::~McpAdapter() { shutdown(); }

std::string McpAdapter::post(const std::string& body, const char* mcp_session) {
    const Url u = parse_url(cfg_.endpoint);
    if (!u.ok) {
        last_error_ = "bad endpoint";
        return "";
    }
    HINTERNET ses = WinHttpOpen(L"SkinnerBoxPP/1.0",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) {
        last_error_ = "WinHttpOpen failed";
        return "";
    }
    WinHttpSetTimeouts(ses, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    std::string out;
    if (HINTERNET con = WinHttpConnect(ses, u.host.c_str(), u.port, 0)) {
        if (HINTERNET req = WinHttpOpenRequest(
                con, L"POST", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, u.https ? WINHTTP_FLAG_SECURE : 0)) {
            std::wstring headers =
                L"Content-Type: application/json\r\n"
                L"Accept: application/json, text/event-stream\r\n";
            if (mcp_session && *mcp_session) {
                headers += L"Mcp-Session-Id: ";
                headers += std::wstring(mcp_session, mcp_session + strlen(mcp_session));
                headers += L"\r\n";
            }
            if (WinHttpSendRequest(req, headers.c_str(),
                                   static_cast<DWORD>(headers.size()),
                                   const_cast<char*>(body.data()),
                                   static_cast<DWORD>(body.size()),
                                   static_cast<DWORD>(body.size()), 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                if (session_id_.empty()) {
                    const std::string sid = header_value(req, L"Mcp-Session-Id");
                    if (!sid.empty()) session_id_ = sid;
                }
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(req, &avail) && avail) {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(req, chunk.data(), avail, &read)) break;
                    out.append(chunk, 0, read);
                }
            } else {
                last_error_ = "request failed";
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    } else {
        last_error_ = "connect failed";
    }
    WinHttpCloseHandle(ses);
    return out;
}

bool McpAdapter::ensure_session() {
    if (connected_) return true;
    const std::string init =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
        "\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"skinnerbox-plus-plus\",\"version\":\"1.0\"}}}";
    const std::string resp = post(init);
    if (resp.find("\"result\"") == std::string::npos) return false;
    post("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
         session_id_.c_str());
    connected_ = true;
    return true;
}

void McpAdapter::call_preset(const char* preset, double intensity,
                             uint32_t seconds) {
    // Client-side clamp; the backend enforces its own independent ceiling.
    const double amp = std::min(std::max(intensity, 0.0), cfg_.max_intensity);
    const uint32_t secs = std::min(seconds, cfg_.max_seconds);
    char body[512];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\",\"params\":"
             "{\"name\":\"vibrate\",\"arguments\":{\"intensity\":%.3f,"
             "\"channel\":%d,\"seconds\":%u}},\"_preset\":\"%s\"}",
             ++next_id_, amp, cfg_.channel, secs, preset);
    post(body, session_id_.c_str());
}

void McpAdapter::deliver(const RewardIntent& intent) {
    if (intent.withheld) return; // counterfactual: never actuate
    const char* preset = nullptr;
    double intensity = 0.0;
    switch (intent.kind) {
        case RewardClass::MicroReward:
            preset = intent.dose < 0.5 ? "ACKNOWLEDGE_LOW" : "REWARD_MEDIUM";
            intensity = intent.dose < 0.5 ? cfg_.max_intensity * 0.5
                                          : cfg_.max_intensity;
            break;
        case RewardClass::RecoveryReward:
            preset = "REWARD_MEDIUM";
            intensity = cfg_.max_intensity;
            break;
        case RewardClass::SessionSummary:
            return; // no hardware on session end; shutdown() stops output
    }
    // Network off the caller's thread: the editor must never block on a device.
    std::thread([this, preset, intensity]() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!ensure_session()) return;
        call_preset(preset, intensity, cfg_.max_seconds);
    }).detach();
}

void McpAdapter::shutdown() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!connected_) return;
    char body[160];
    snprintf(body, sizeof(body),
             "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"stop\",\"arguments\":{}}}",
             ++next_id_);
    post(body, session_id_.c_str());
    connected_ = false;
}

} // namespace sbpp
