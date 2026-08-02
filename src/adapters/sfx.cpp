// SkinnerBox++ — reinforcement SFX engine.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "sfx.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdio>

namespace sbpp {

namespace {

constexpr uint32_t kSampleRate = 44100;
// Peak amplitude well under full scale: reinforcement cues sit inside the
// writer's audio mix, they don't dominate it (peak-headroom rule borrowed
// from the trance audio engine).
constexpr double kPeak = 0.35;

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

// Wrap mono 16-bit PCM in a minimal RIFF header (what PlaySound(SND_MEMORY)
// wants).
std::vector<uint8_t> wav_from_pcm(const std::vector<int16_t>& pcm) {
    std::vector<uint8_t> v;
    const uint32_t data_bytes = static_cast<uint32_t>(pcm.size() * 2);
    v.reserve(44 + data_bytes);
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    put_u32(v, 36 + data_bytes);
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(v, 16);
    put_u16(v, 1);                    // PCM
    put_u16(v, 1);                    // mono
    put_u32(v, kSampleRate);
    put_u32(v, kSampleRate * 2);      // byte rate
    put_u16(v, 2);                    // block align
    put_u16(v, 16);                   // bits
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    put_u32(v, data_bytes);
    for (const int16_t s : pcm) put_u16(v, static_cast<uint16_t>(s));
    return v;
}

struct Partial {
    double freq_hz;
    double amp;       // relative
    double decay_s;   // exponential time constant
    double delay_s;   // onset offset (two-note cues)
};

// Additive bell: soft 4 ms attack (no click), exponential decay per partial.
std::vector<uint8_t> synth(const Partial* parts, size_t n_parts,
                           double total_s) {
    const size_t n = static_cast<size_t>(total_s * kSampleRate);
    std::vector<int16_t> pcm(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        double x = 0.0;
        for (size_t p = 0; p < n_parts; ++p) {
            const double tt = t - parts[p].delay_s;
            if (tt < 0.0) continue;
            const double attack = tt < 0.004 ? tt / 0.004 : 1.0;
            x += parts[p].amp * attack * std::exp(-tt / parts[p].decay_s) *
                 std::sin(6.283185307179586 * parts[p].freq_hz * tt);
        }
        // Tail fade so the buffer never ends on a step.
        const double tail = t > total_s - 0.01 ? (total_s - t) / 0.01 : 1.0;
        double s = kPeak * tail * x;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        pcm[i] = static_cast<int16_t>(s * 32767.0);
    }
    return wav_from_pcm(pcm);
}

// The classic two-note coin: B5 grace note into a long E6 ring.
std::vector<uint8_t> synth_coin_yellow() {
    static const Partial parts[] = {
        {987.77, 0.9, 0.045, 0.0},    // B5, short
        {1318.51, 1.0, 0.28, 0.08},   // E6, rings out
        {2637.02, 0.25, 0.12, 0.08},  // E7 shimmer
    };
    return synth(parts, 3, 0.55);
}

// Warmer two-tone chime for the quality tier: lower, richer, longer.
std::vector<uint8_t> synth_coin_red() {
    static const Partial parts[] = {
        {659.26, 1.0, 0.30, 0.0},     // E5
        {987.77, 0.7, 0.28, 0.10},    // B5 a fifth up, slightly late
        {1975.53, 0.20, 0.15, 0.10},  // sparkle
        {1819.0, 0.12, 0.10, 0.0},    // inharmonic bell partial (~2.76 f0)
    };
    return synth(parts, 4, 0.8);
}

std::vector<uint8_t> read_file(const std::wstring& path) {
    std::vector<uint8_t> v;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return v;
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len > 0 && len < 8 * 1024 * 1024) { // sanity: an SFX, not an album
        v.resize(static_cast<size_t>(len));
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    std::fclose(f);
    return v;
}

} // namespace

Sfx::~Sfx() {
    // Stop before the WAV vectors are freed — winmm keeps reading an
    // SND_MEMORY buffer until playback ends, so tearing the engine down
    // within ~1 s of a collect would otherwise be a use-after-free.
    PlaySoundW(nullptr, nullptr, 0);
}

Sfx::Sfx(const std::wstring& sounds_dir) {
    if (!sounds_dir.empty()) {
        yellow_wav_ = read_file(sounds_dir + L"\\coin_yellow.wav");
        red_wav_ = read_file(sounds_dir + L"\\coin_red.wav");
    }
    if (yellow_wav_.empty()) yellow_wav_ = synth_coin_yellow();
    if (red_wav_.empty()) red_wav_ = synth_coin_red();
}

void Sfx::play(Cue cue) const {
    const std::vector<uint8_t>& wav =
        cue == Cue::CoinYellow ? yellow_wav_ : red_wav_;
    if (wav.empty()) return;
    // SND_MEMORY needs the buffer alive for the whole playback — members
    // live as long as the adapter, which outlives any 1 s cue.
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

} // namespace sbpp
