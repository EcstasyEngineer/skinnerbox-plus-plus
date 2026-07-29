// SkinnerBox++ — phasic audio chime adapter (Windows).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#pragma once

#include "../core/adapter.h"

namespace sbpp {

// Phasic-only channel: a soft system chime on delivered rewards. No tonic
// component in the MVP (real audio muffling/clarity work is a future adapter,
// see docs/output-contract.md).
class AudioAdapter : public IOutputAdapter {
public:
    const char* name() const override { return "audio"; }
    void ambient(const AmbientState&) override {}
    void deliver(const RewardIntent& intent) override;
    void shutdown() override {}
};

} // namespace sbpp
