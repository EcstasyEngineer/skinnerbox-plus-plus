// SkinnerBox++ — lexical content facets (anti-slop gate inputs).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace sbpp {

struct ContentFacets {
    double repetition = 0.0;   // repeated-token mass in the recent window (0-1)
    double entropy = 0.0;      // character entropy (bits) of the recent window
    double stall_frac = 0.0;   // "uuuh"-class token fraction (0-1)
    uint32_t window_chars = 0; // how much text the estimates are based on
    // Tail facets over the most recent ~80 chars: dilute filler hides in a
    // long window but is loud at the moment it's typed.
    uint32_t tail_stall_run = 0;  // longest consecutive run of {u,h,m} chars
    uint32_t tail_max_token = 0;  // max occurrences of one token in the tail
};

// Rolling window over recently *typed* text (append-only stream of inserted
// characters; deletions don't rewrite history — the gate asks "what has the
// writer been producing", not "what does the document say"). Editor- and
// OS-independent.
class ContentWindow {
public:
    explicit ContentWindow(size_t max_chars = 600) : max_chars_(max_chars) {}

    void add_text(const char* utf8, size_t len);
    ContentFacets facets() const;

private:
    size_t max_chars_;
    std::deque<char> buf_;
};

} // namespace sbpp
