// SkinnerBox++ — lexical content facets (anti-slop gate inputs).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "content.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <vector>

#include "bigram_en.h"

namespace sbpp {

namespace {
// a-z -> 0-25, everything else folds to 26 (space).
inline int bigram_idx(char c) {
    const unsigned char u = static_cast<unsigned char>(std::tolower(
        static_cast<unsigned char>(c)));
    return (u >= 'a' && u <= 'z') ? u - 'a' : 26;
}
} // namespace

void ContentWindow::add_text(const char* utf8, size_t len) {
    // Latin/ASCII-oriented gate: the window is a byte stream. Multi-byte UTF-8
    // is stored as raw bytes (histograms/tokenization are not codepoint-aware).
    // Product stance for v0: English typing sessions. Non-ASCII writing will
    // make facets noisier; decode codepoints before facets if that changes.
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

    // English char-bigram cost (mean bits/char): the keyboard-mash detector.
    // Char entropy can't see mash — "sdlfkja" is diverse — but its bigrams
    // are wildly implausible English. Space-space pairs are skipped so runs
    // of whitespace/punctuation don't dilute the average.
    {
        int prev = -1;
        double bits = 0.0;
        uint32_t n_pairs = 0;
        for (char c : buf_) {
            const int idx = bigram_idx(c);
            if (prev >= 0 && !(prev == 26 && idx == 26)) {
                bits += kBigramBits[prev][idx];
                ++n_pairs;
            }
            prev = idx;
        }
        if (n_pairs > 0) f.bigram_bpc = bits / n_pairs;
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
            // >= 4: "the"/"and" legitimately land 3+ times in an 80-char tail
            // of normal prose; the target is "duper duper duper", not English.
            if (tcur.size() >= 4)
                f.tail_max_token = std::max(f.tail_max_token, ++tail_counts[tcur]);
            tcur.clear();
        }
    }
    if (tcur.size() >= 4)
        f.tail_max_token = std::max(f.tail_max_token, ++tail_counts[tcur]);
    return f;
}

} // namespace sbpp
