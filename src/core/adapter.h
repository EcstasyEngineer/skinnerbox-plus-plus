// SkinnerBox++ — output adapter interface.
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include "reward_intent.h"

namespace sbpp {

// One reward channel. The engine calls ambient() every tick (~1 Hz) and
// deliver() on discrete reward events. Implementations must be cheap and
// non-blocking on the caller's thread, and must restore any state they touched
// in shutdown() — the engine guarantees shutdown() is called on unload, and a
// crash-safe adapter fails toward "off / neutral".
//
// deliver() is also called for withheld intents (intent.withheld == true).
// Only logging/telemetry adapters should act on those; sensory adapters must
// return without output, because withheld events exist to measure what happens
// when a qualifying moment goes unrewarded.
class IOutputAdapter {
public:
    virtual ~IOutputAdapter() = default;
    virtual const char* name() const = 0;
    virtual void ambient(const AmbientState& state) = 0;
    virtual void deliver(const RewardIntent& intent) = 0;
    virtual void shutdown() = 0;
};

} // namespace sbpp
