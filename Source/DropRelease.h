#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>

#include "EffectSettings.h"
#include "MacroMapping.h"
#include "RiserVoice.h"

/**
    DropRelease: the downlifter, what PreDrop plays after the drop.

    It notices the drop: Amount falling fast from a build-up, from 50% or more
    to 10% or less within a beat. A slower turn down is left alone. Then, for
    the downlifter's LENGTH (in bars at the host's tempo and meter):

      - the riser's noise turns around, sweeping down from where the build-up
        left it to FALL TO, at LEVEL (50%: the riser's own level), and fades out;
      - the reverb and delay stop taking in sound but ring on over the drop
        at their build-up level times REVERB and DELAY (100%: all of it, 0%:
        cut off at the drop), fading out with it.

    The high-pass is left to Amount, so the drop lands with its low end.
    Turning Amount back up past 10% ends the downlifter early. Plain
    arithmetic, once per block on the audio thread.
*/
class DropRelease
{
public:
    static constexpr float highAmount = 0.5f;   // a build-up reaches at least this
    static constexpr float lowAmount  = 0.1f;   // and a drop falls to at most this
    static constexpr double fallbackBeatSeconds = 0.5;   // within a beat at 120 BPM with no tempo

    /** Watches Amount for the drop and moves an active downlifter on by a
        block. Before the block's targets are taken. */
    void update (float amount, const MacroMapping::Depths& depths, const EffectSettings& settings,
                 const HostTransport& transport, double sampleRate, int numSamples) noexcept
    {
        if (! settings.isOn (EffectSettings::Id::dropOn))
        {
            reset();
            return;
        }

        if (remaining > 0)
            remaining = amount > lowAmount ? 0 : std::max (0, remaining - numSamples);

        const double bpm = transport.bpm > 0.0 ? transport.bpm : EffectSettings::fallbackBpm;
        if (amount >= highAmount)
        {
            armed     = true;
            peak      = std::max (peak, amount);
            sinceHigh = 0;
            return;
        }
        if (! armed)
            return;

        sinceHigh += numSamples;
        const double withinSamples = (transport.bpm > 0.0 ? 60.0 / bpm : fallbackBeatSeconds) * sampleRate;
        if (amount <= lowAmount && (double) sinceHigh <= withinSamples + numSamples)
            start (depths, settings, transport.barLength, bpm, sampleRate);
        if (amount <= lowAmount || (double) sinceHigh > withinSamples + numSamples)
        {
            armed = false;
            peak  = 0.0f;
        }
    }

    bool isActive() const noexcept { return remaining > 0; }

    /** How far through the downlifter the end of this block is (0..1). */
    float progress() const noexcept
    {
        return length > 0 ? 1.0f - (float) remaining / (float) length : 1.0f;
    }

    /** While active: takes the targets Amount gives and puts in the
        downlifter's, for the end of this block. */
    void shape (MacroMapping::Targets& t) const noexcept
    {
        if (! isActive())
            return;

        const float x    = progress();
        const float fade = (1.0f - x) * (1.0f - x);
        t.reverbSize    = from.reverbSize;
        t.reverbWet     = from.reverbWet * fade;
        t.reverbLevel   = from.reverbLevel * fade;
        t.delayFeedback = from.delayFeedback;
        t.delayWet      = from.delayWet * fade;
        t.delayLevel    = from.delayLevel * fade;
        t.riserLevel    = noiseLevel * fade;
        t.riserCutoffHz = from.riserCutoffHz * std::pow (fallToHz / from.riserCutoffHz, x);
    }

    void reset() noexcept
    {
        armed     = false;
        peak      = 0.0f;
        sinceHigh = 0;
        remaining = 0;
        length    = 0;
    }

private:
    void start (const MacroMapping::Depths& depths, const EffectSettings& settings, double barLength,
                double bpm, double sampleRate) noexcept
    {
        // From where the build-up peaked: its reverb and delay (as much of
        // them as REVERB and DELAY keep), its riser sweep, and the noise at
        // LEVEL in place of the riser's depth.
        from = MacroMapping::mapMacro (peak, depths, settings);
        const float keepReverb = settings[EffectSettings::Id::dropReverb];
        const float keepDelay  = settings[EffectSettings::Id::dropDelay];
        from.reverbWet   *= keepReverb;
        from.reverbLevel *= keepReverb;
        from.delayWet    *= keepDelay;
        from.delayLevel  *= keepDelay;
        auto asNoise  = depths;
        asNoise.riser = settings[EffectSettings::Id::dropLevel];
        noiseLevel    = MacroMapping::mapMacro (peak, asNoise, settings).riserLevel;
        fallToHz      = std::min (settings[EffectSettings::Id::dropEnd], from.riserCutoffHz);

        const double quarters = settings.dropBars() * (barLength > 0.0 ? barLength : 4.0);
        length    = std::max (1, (int) std::lround (quarters * 60.0 / bpm * sampleRate));
        remaining = length;
    }

    // Watching for the drop.
    bool  armed     = false;   // Amount has been up at a build-up's level
    float peak      = 0.0f;    // the highest it got
    int   sinceHigh = 0;       // samples since it was last that high

    // The downlifter on its way.
    int   length     = 0;      // in samples
    int   remaining  = 0;
    MacroMapping::Targets from;
    float noiseLevel = 0.0f;
    float fallToHz   = 150.0f;
};
