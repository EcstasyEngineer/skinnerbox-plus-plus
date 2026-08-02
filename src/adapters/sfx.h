// SkinnerBox++ — reinforcement SFX engine.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sbpp {

// Tiny dependency-free sound engine for the coin channel. Two cues, both
// REINFORCING by design — bright, short, game-like. Never a system alert:
// the original prototype shipped the Windows error chirp as feedback and it
// read as punishment (see docs/architecture.md design record).
//
// Source per cue, in order: a writer-supplied .wav in the sounds dir
// (coin_yellow.wav / coin_red.wav — drop your own bell there; .ogg users:
// convert once, e.g. `ffmpeg -i bell.ogg bell.wav`), else a synthesized
// bell baked at construction. Playback is winmm PlaySound(SND_MEMORY|
// SND_ASYNC): fire-and-forget on the caller's thread, a new cue replaces a
// still-sounding one. Failures log nowhere and do nothing — sound is
// garnish, never load-bearing.
class Sfx {
public:
    enum class Cue { CoinYellow, CoinRed };

    // sounds_dir may be empty (synth only). No I/O after construction.
    explicit Sfx(const std::wstring& sounds_dir);

    void play(Cue cue) const;

private:
    // In-memory RIFF/WAV images, ready for PlaySound(SND_MEMORY).
    std::vector<uint8_t> yellow_wav_;
    std::vector<uint8_t> red_wav_;
};

} // namespace sbpp
