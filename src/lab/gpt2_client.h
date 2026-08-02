// SkinnerBox++ — async GPT-2 lab client (advanced_debug).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.
//
// Talks to a long-lived lab host over stdin/stdout (newline JSON). The first
// host is experiments/gpt2_lab_host.py (same math as gpt2_lab.py). A future
// native ORT binary can speak the same protocol without plugin changes.
// Never blocks the editor thread on model inference.

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace sbpp {

struct Gpt2Score {
    bool ok = false;
    double mean_bits = 0.0;
    double std_bits = 0.0;
    double band_dist = 0.0;
    double ms = 0.0;
    int n_tok = 0;
    std::string error;
};

class Gpt2LabClient {
public:
    Gpt2LabClient() = default;
    ~Gpt2LabClient() { stop(); }

    Gpt2LabClient(const Gpt2LabClient&) = delete;
    Gpt2LabClient& operator=(const Gpt2LabClient&) = delete;

    // python_exe: full path or "py". host_script: gpt2_lab_host.py path.
    bool start(const std::wstring& python_exe, const std::wstring& host_script);
    void stop();

    bool running() const { return running_.load(); }
    bool host_ready() const { return host_ready_.load(); }
    std::string last_error() const;

    // Non-blocking: queues at most one in-flight score. Drops if busy.
    void request_score(const std::string& text);

    // If a result arrived since last take, copy it and return true.
    bool take_result(Gpt2Score& out);

    // One-shot RPC for check/download (blocks the caller). Used only when
    // arming LAB so the plugin can prompt before any network fetch.
    // timeout_ms: 0 = wait forever (download can be slow on first run).
    static bool oneshot(const std::wstring& python_exe,
                        const std::wstring& host_script,
                        const std::string& request_line,
                        std::string& response_line,
                        std::string& error_out,
                        unsigned timeout_ms = 120000);

private:
    void reader_main();
    bool write_line(const std::string& line);

    std::atomic<bool> running_{false};
    std::atomic<bool> host_ready_{false};
    std::atomic<bool> busy_{false};
    mutable std::mutex err_mu_;
    std::string last_error_;
    std::mutex result_mu_;
    Gpt2Score pending_;
    bool has_pending_ = false;
    std::thread reader_;
    void* process_ = nullptr;      // HANDLE
    void* stdin_wr_ = nullptr;     // HANDLE
    void* stdout_rd_ = nullptr;    // HANDLE
};

} // namespace sbpp
