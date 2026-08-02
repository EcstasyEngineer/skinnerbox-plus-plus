// SkinnerBox++ — async GPT-2 lab client.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "gpt2_client.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace sbpp {

namespace {

// Minimal JSON number / string extractors (host output is simple and trusted).
bool json_get_bool(const std::string& s, const char* key, bool& out) {
    const std::string pat = std::string("\"") + key + "\":";
    auto p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < s.size() && (s[p] == ' ')) ++p;
    if (s.compare(p, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (s.compare(p, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool json_get_number(const std::string& s, const char* key, double& out) {
    const std::string pat = std::string("\"") + key + "\":";
    auto p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < s.size() && s[p] == ' ') ++p;
    char* end = nullptr;
    out = std::strtod(s.c_str() + p, &end);
    return end != s.c_str() + p;
}

bool json_get_string(const std::string& s, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\":\"";
    auto p = s.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    out.clear();
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            ++p;
            out.push_back(s[p++]);
        } else {
            out.push_back(s[p++]);
        }
    }
    return true;
}

std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            o += buf;
        } else {
            o.push_back(static_cast<char>(c));
        }
    }
    return o;
}

bool spawn_host(const std::wstring& python_exe, const std::wstring& host_script,
                HANDLE& process, HANDLE& stdin_wr, HANDLE& stdout_rd) {
    process = nullptr;
    stdin_wr = nullptr;
    stdout_rd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE in_rd = nullptr, in_wr = nullptr;
    HANDLE out_rd = nullptr, out_wr = nullptr;
    if (!CreatePipe(&in_rd, &in_wr, &sa, 0)) return false;
    if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) {
        CloseHandle(in_rd);
        CloseHandle(in_wr);
        return false;
    }
    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE err_wr = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                                OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = in_rd;
    si.hStdOutput = out_wr;
    si.hStdError = err_wr ? err_wr : out_wr;

    std::wstring cmd = L"\"" + python_exe + L"\" -u \"" + host_script + L"\"";
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');
    std::wstring workdir;
    {
        const auto slash = host_script.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            workdir = host_script.substr(0, slash);
    }

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, workdir.empty() ? nullptr : workdir.c_str(), &si, &pi);
    CloseHandle(in_rd);
    CloseHandle(out_wr);
    if (err_wr) CloseHandle(err_wr);
    if (!ok) {
        CloseHandle(in_wr);
        CloseHandle(out_rd);
        return false;
    }
    CloseHandle(pi.hThread);
    process = pi.hProcess;
    stdin_wr = in_wr;
    stdout_rd = out_rd;
    return true;
}

bool read_line_timeout(HANDLE out_rd, HANDLE process, std::string& line,
                       unsigned timeout_ms) {
    line.clear();
    std::string buf;
    char chunk[512];
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (timeout_ms && GetTickCount64() - start > timeout_ms) return false;
        DWORD avail = 0;
        if (!PeekNamedPipe(out_rd, nullptr, 0, nullptr, &avail, nullptr))
            return false;
        if (avail == 0) {
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
                // Drain anything left.
                DWORD n = 0;
                while (ReadFile(out_rd, chunk, sizeof(chunk), &n, nullptr) && n)
                    buf.append(chunk, chunk + n);
                break;
            }
            Sleep(20);
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(out_rd, chunk, sizeof(chunk), &n, nullptr) || n == 0)
            return false;
        buf.append(chunk, chunk + n);
        auto nl = buf.find('\n');
        if (nl != std::string::npos) {
            line = buf.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
    }
    auto nl = buf.find('\n');
    if (nl != std::string::npos) {
        line = buf.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return !line.empty();
    }
    if (!buf.empty()) {
        line = buf;
        return true;
    }
    return false;
}

} // namespace

bool Gpt2LabClient::oneshot(const std::wstring& python_exe,
                            const std::wstring& host_script,
                            const std::string& request_line,
                            std::string& response_line,
                            std::string& error_out,
                            unsigned timeout_ms) {
    response_line.clear();
    error_out.clear();
    HANDLE process = nullptr, in_wr = nullptr, out_rd = nullptr;
    if (!spawn_host(python_exe, host_script, process, in_wr, out_rd)) {
        error_out = "CreateProcess failed for GPT-2 lab host";
        return false;
    }
    std::string payload = request_line;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    DWORD written = 0;
    if (!WriteFile(in_wr, payload.data(), static_cast<DWORD>(payload.size()),
                   &written, nullptr)) {
        error_out = "failed to write request";
        CloseHandle(in_wr);
        CloseHandle(out_rd);
        TerminateProcess(process, 1);
        CloseHandle(process);
        return false;
    }
    // Close stdin after the one request so the host can exit on EOF if it wants.
    const char quit[] = "{\"op\":\"quit\"}\n";
    WriteFile(in_wr, quit, sizeof(quit) - 1, &written, nullptr);
    CloseHandle(in_wr);

    const bool got = read_line_timeout(out_rd, process, response_line, timeout_ms);
    CloseHandle(out_rd);
    WaitForSingleObject(process, 2000);
    TerminateProcess(process, 0);
    CloseHandle(process);
    if (!got) {
        error_out = "timeout or no response from lab host";
        return false;
    }
    return true;
}

bool Gpt2LabClient::start(const std::wstring& python_exe,
                          const std::wstring& host_script) {
    stop();
    HANDLE process = nullptr, in_wr = nullptr, out_rd = nullptr;
    if (!spawn_host(python_exe, host_script, process, in_wr, out_rd)) {
        std::lock_guard<std::mutex> lock(err_mu_);
        last_error_ = "CreateProcess failed for GPT-2 lab host";
        return false;
    }
    process_ = process;
    stdin_wr_ = in_wr;
    stdout_rd_ = out_rd;
    running_ = true;
    host_ready_ = false;
    busy_ = false;
    reader_ = std::thread([this] { reader_main(); });

    // Kick a hello; host replies ready after model load (can take a while).
    write_line("{\"op\":\"hello\"}");
    return true;
}

void Gpt2LabClient::stop() {
    if (!running_.load() && !process_) return;
    running_ = false;
    if (stdin_wr_) {
        // Best-effort shutdown line, then close so the host sees EOF.
        write_line("{\"op\":\"quit\"}");
        CloseHandle(static_cast<HANDLE>(stdin_wr_));
        stdin_wr_ = nullptr;
    }
    if (process_) {
        // Don't hang the UI forever on a stuck model load.
        WaitForSingleObject(static_cast<HANDLE>(process_), 3000);
        TerminateProcess(static_cast<HANDLE>(process_), 0);
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (stdout_rd_) {
        CloseHandle(static_cast<HANDLE>(stdout_rd_));
        stdout_rd_ = nullptr;
    }
    if (reader_.joinable()) reader_.join();
    host_ready_ = false;
    busy_ = false;
}

std::string Gpt2LabClient::last_error() const {
    std::lock_guard<std::mutex> lock(err_mu_);
    return last_error_;
}

bool Gpt2LabClient::write_line(const std::string& line) {
    if (!stdin_wr_) return false;
    std::string payload = line;
    payload.push_back('\n');
    DWORD written = 0;
    return WriteFile(static_cast<HANDLE>(stdin_wr_), payload.data(),
                     static_cast<DWORD>(payload.size()), &written, nullptr) != 0;
}

void Gpt2LabClient::request_score(const std::string& text) {
    if (!running_.load() || !host_ready_.load()) return;
    bool expected = false;
    if (!busy_.compare_exchange_strong(expected, true)) return; // one in flight
    std::string line = "{\"op\":\"score\",\"text\":\"";
    line += json_escape(text);
    line += "\"}";
    if (!write_line(line)) {
        busy_ = false;
        std::lock_guard<std::mutex> lock(err_mu_);
        last_error_ = "failed to write score request";
    }
}

bool Gpt2LabClient::take_result(Gpt2Score& out) {
    std::lock_guard<std::mutex> lock(result_mu_);
    if (!has_pending_) return false;
    out = pending_;
    has_pending_ = false;
    return true;
}

void Gpt2LabClient::reader_main() {
    std::string buf;
    char chunk[1024];
    while (running_.load()) {
        DWORD n = 0;
        const BOOL ok =
            ReadFile(static_cast<HANDLE>(stdout_rd_), chunk, sizeof(chunk),
                     &n, nullptr);
        if (!ok || n == 0) break;
        buf.append(chunk, chunk + n);
        for (;;) {
            auto nl = buf.find('\n');
            if (nl == std::string::npos) break;
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            bool ready = false;
            if (json_get_bool(line, "ready", ready) && ready) {
                host_ready_ = true;
            }

            Gpt2Score sc;
            bool ok_flag = false;
            if (json_get_bool(line, "ok", ok_flag)) {
                sc.ok = ok_flag;
                json_get_number(line, "mean", sc.mean_bits);
                json_get_number(line, "std", sc.std_bits);
                json_get_number(line, "band_dist", sc.band_dist);
                json_get_number(line, "ms", sc.ms);
                double ntok = 0;
                if (json_get_number(line, "n_tok", ntok))
                    sc.n_tok = static_cast<int>(ntok);
                json_get_string(line, "error", sc.error);
                // Only score replies free the busy slot (hello/ready don't).
                if (line.find("\"mean\"") != std::string::npos ||
                    line.find("\"error\"") != std::string::npos) {
                    {
                        std::lock_guard<std::mutex> lock(result_mu_);
                        pending_ = sc;
                        has_pending_ = true;
                    }
                    busy_ = false;
                    if (!sc.ok && !sc.error.empty()) {
                        std::lock_guard<std::mutex> lock(err_mu_);
                        last_error_ = sc.error;
                    }
                } else if (!sc.ok && !sc.error.empty()) {
                    std::lock_guard<std::mutex> lock(err_mu_);
                    last_error_ = sc.error;
                    busy_ = false;
                }
            }
        }
    }
    running_ = false;
    host_ready_ = false;
    busy_ = false;
}

} // namespace sbpp
