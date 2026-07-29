// SkinnerBox++ — phasic audio chime adapter (Windows).
// This file is part of SkinnerBox++, released under the GNU GPL v3 or later.

#include "audio_adapter.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

namespace sbpp {

void AudioAdapter::deliver(const RewardIntent& intent) {
    if (intent.withheld) return; // counterfactual: qualified, but stays silent
    switch (intent.kind) {
        case RewardClass::MicroReward:
        case RewardClass::RecoveryReward:
            PlaySoundW(L"SystemAsterisk", nullptr,
                       SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
            break;
        case RewardClass::SessionSummary:
            break;
    }
}

} // namespace sbpp
