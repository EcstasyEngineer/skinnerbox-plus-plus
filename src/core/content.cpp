// SkinnerBox++ — lexical content facets (anti-slop gate inputs).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "content.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <vector>

namespace sbpp {

void ContentWindow::add_text(const char* utf8, size_t len) {
    if (!utf8) return;
    for (size_t i = 0; i < len; ++i) buf_.push_back(utf8[i]);
    while (buf_.size() > max_chars_) buf_.pop_front();
}

ContentFacets ContentWindow::facets() const {
    ContentFacets f;
    f.window_chars = static_cast<uint32_t>(buf_.size());
    if (buf_.empty()) return f;

    // Character entropy over the window.
    std::array<uint32_t, 256> hist{};
    for (char c : buf_) hist[static_cast<unsigned char>(c)]++;
    const double n = static_cast<double>(buf_.size());
    for (uint32_t k : hist) {
        if (!k) continue;
        const double p = k / n;
        f.entropy -= p * std::log2(p);
    }

    // Tokenize (lowercased alpha runs).
    std::vector<std::string> toks;
    std::string cur;
    for (char c : buf_) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            toks.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    if (toks.empty()) return f;

    std::map<std::string, uint32_t> counts;
    uint32_t stall = 0;
    for (const auto& t : toks) {
        counts[t]++;
        // "uuuh"-class: 3+ chars drawn entirely from {u,h,m} (uuuh, hmm, umm...)
        if (t.size() >= 3 &&
            t.find_first_not_of("uhm") == std::string::npos)
            stall++;
    }
    f.repetition = 1.0 - static_cast<double>(counts.size()) / toks.size();
    f.stall_frac = static_cast<double>(stall) / toks.size();

    // Tail scan (last 80 chars): longest {u,h,m} char run and max same-token
    // count — catches "uuuuuuuuuuuuuh" and "duper duper duper" as they happen.
    const size_t tail_n = std::min<size_t>(80, buf_.size());
    std::string tail(buf_.end() - tail_n, buf_.end());
    uint32_t run = 0;
    for (char c : tail) {
        const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lc == 'u' || lc == 'h' || lc == 'm') {
            f.tail_stall_run = std::max(f.tail_stall_run, ++run);
        } else {
            run = 0;
        }
    }
    std::map<std::string, uint32_t> tail_counts;
    std::string tcur;
    for (char c : tail) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            tcur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!tcur.empty()) {
            if (tcur.size() >= 3) // ignore stopword-length tokens ("the the")
                f.tail_max_token = std::max(f.tail_max_token, ++tail_counts[tcur]);
            tcur.clear();
        }
    }
    if (tcur.size() >= 3)
        f.tail_max_token = std::max(f.tail_max_token, ++tail_counts[tcur]);
    return f;
}

} // namespace sbpp
