#pragma once

#include "LinkHub.h"

/** PreDrop's parameter IDs, shared by the processor and the parts it owns. */
namespace ParamID
{
    inline constexpr const char* amount      = "amount";
    inline constexpr const char* mix         = "mix";
    inline constexpr const char* outputTrim  = "outputTrim";
    inline constexpr const char* reverbDepth = "reverbDepth";
    inline constexpr const char* delayDepth  = "delayDepth";
    inline constexpr const char* riserDepth  = "riserDepth";
    inline constexpr const char* hpfDepth    = "hpfDepth";
    inline constexpr const char* role        = "role";
    inline constexpr const char* trackHpf    = "trackHpf";
    inline constexpr const char* trackReverb = "trackReverb";
    inline constexpr const char* trackDelay  = "trackDelay";

    /** The parameter holding a linked track's level for `effect`. */
    inline const char* trackLevel (LinkHub::Effect effect) noexcept
    {
        switch (effect)
        {
            case LinkHub::Effect::reverb: return trackReverb;
            case LinkHub::Effect::delay:  return trackDelay;
            case LinkHub::Effect::hpf:    break;
        }
        return trackHpf;
    }
}
