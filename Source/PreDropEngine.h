#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>

/**
    PreDropEngine

    The DSP heart of the "pre-drop" build-up effect, deliberately kept free of any
    host / plugin plumbing so it can be unit-tested or reused on its own.

    A single macro parameter ("amount", 0..1) progressively engages a chain of
    effects, so a slow turn of one knob takes a steady loop and builds it into the
    tension before a drop:

        amount  0% ............................................. 100%
        HPF     |===================================================|  cut low end
        Reverb         |=========================================|     swell space
        Delay                  |=================================|     washed tail
        Riser                          |=========================|     noise sweep

    Each effect has its own activation window inside the macro range, so they stack
    in one at a time rather than all arriving at once. The mapping from the macro to
    each effect's internal targets lives in mapMacro(), which is a pure function and
    therefore the part most worth unit-testing.
*/
class PreDropEngine
{
public:
    PreDropEngine() = default;

    /** Per-effect targets derived from the macro, already in engineering units. */
    struct Targets
    {
        float hpfCutoffHz   = 20.0f;   // high-pass cutoff
        float reverbWet     = 0.0f;    // 0..1 wet amount
        float reverbSize    = 0.3f;    // 0..1 room size
        float delayWet      = 0.0f;    // 0..1 echo level
        float delayFeedback = 0.0f;    // 0..1 feedback (clamped < 1 for stability)
        float riserLevel    = 0.0f;    // 0..1 noise-sweep gain
        float riserCutoffHz = 200.0f;  // band-pass centre of the riser
    };

    /** Per-effect depth (0..1) scaling how strong each effect gets at full tilt.

        These are independent of the Amount macro, which still owns the build-up
        *timing* (engagement windows). A depth of 1 reproduces the original fixed
        mapping; lowering one pulls that single effect back without touching the
        others. They default to 1 so the engine is unchanged until a user dials. */
    struct Depths
    {
        float hpf    = 1.0f;   // scales the top of the HPF cutoff sweep
        float reverb = 1.0f;   // scales the reverb wet maximum
        float delay  = 1.0f;   // scales the delay wet maximum
        float riser  = 1.0f;   // scales the riser level maximum
    };

    /** Local 0..1 progress of m inside [lo, hi], clamped. */
    static float window (float m, float lo, float hi) noexcept
    {
        return juce::jlimit (0.0f, 1.0f, (m - lo) / (hi - lo));
    }

    /** Pure macro -> targets mapping with every effect at full depth. */
    static Targets mapMacro (float amount) noexcept { return mapMacro (amount, Depths {}); }

    /** Pure macro -> targets mapping. No state, so it is trivially unit-testable.

        A default argument can't be used here: Depths is a nested type and its
        default member initialisers aren't available in this member's default
        argument, so the no-depth overload above delegates from its body. */
    static Targets mapMacro (float amount, Depths depth) noexcept
    {
        const float m = juce::jlimit (0.0f, 1.0f, amount);
        Targets t;

        // 1. High-pass: always engaged, exponential cutoff with a slow start.
        //    Depth scales the top of the sweep: depth=1 -> 800 Hz (original),
        //    depth=0 -> stays at 20 Hz so the low-cut is effectively off.
        const float h     = std::pow (window (m, 0.0f, 1.0f), 1.5f);
        const float topHz = 20.0f + (800.0f - 20.0f) * juce::jlimit (0.0f, 1.0f, depth.hpf);
        t.hpfCutoffHz = 20.0f * std::pow (topHz / 20.0f, h);          // 20 -> (20..800) Hz

        // 2. Reverb: engages at 20%.
        const float r = window (m, 0.20f, 1.0f);
        t.reverbWet  = 0.60f * r * juce::jlimit (0.0f, 1.0f, depth.reverb);
        t.reverbSize = 0.30f + 0.65f * r;

        // 3. Delay: engages at 40%, feedback clamped for stability.
        const float d = window (m, 0.40f, 1.0f);
        t.delayWet      = 0.50f * d * juce::jlimit (0.0f, 1.0f, depth.delay);
        t.delayFeedback = std::min (0.85f * d, 0.95f);

        // 4. Riser: engages at 60% with a sharp late surge.
        const float s = window (m, 0.60f, 1.0f);
        const float sCurve = s * s;
        t.riserLevel    = 0.70f * sCurve * juce::jlimit (0.0f, 1.0f, depth.riser);
        t.riserCutoffHz = 200.0f * std::pow (8000.0f / 200.0f, s);     // 200 -> 8k Hz

        return t;
    }

    void prepare (double sampleRate, int numChannels, int maximumBlockSize)
    {
        sr    = sampleRate;
        numCh = juce::jmax (1, numChannels);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate       = sampleRate;
        spec.maximumBlockSize = (juce::uint32) juce::jmax (1, maximumBlockSize);
        spec.numChannels      = (juce::uint32) numCh;

        hpf.prepare (spec);
        hpf.setType (juce::dsp::StateVariableTPTFilterType::highpass);

        riserFilter.prepare (spec);
        riserFilter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        riserFilter.setResonance (1.5f);

        reverb.prepare (spec);

        delayLine.setMaximumDelayInSamples (maxDelaySamples);
        delayLine.prepare (spec);
        delayLine.setDelay ((float) (delayTimeSeconds * sampleRate));

        dryBuffer.setSize (numCh, juce::jmax (1, maximumBlockSize), false, false, true);

        const double ramp = 0.04; // 40 ms, fast enough to track but kills zipper noise
        hpfCutoffSm  .reset (sampleRate, ramp);
        reverbWetSm  .reset (sampleRate, ramp);
        reverbSizeSm .reset (sampleRate, ramp);
        delayWetSm   .reset (sampleRate, ramp);
        delayFbSm    .reset (sampleRate, ramp);
        riserLevelSm .reset (sampleRate, ramp);
        riserCutoffSm.reset (sampleRate, ramp);
        mixSm        .reset (sampleRate, ramp);
        trimSm       .reset (sampleRate, ramp);

        // Snap smoothers to the current targets so the first block does not sweep.
        const Targets t = mapMacro (amount, depths);
        hpfCutoffSm  .setCurrentAndTargetValue (t.hpfCutoffHz);
        reverbWetSm  .setCurrentAndTargetValue (t.reverbWet);
        reverbSizeSm .setCurrentAndTargetValue (t.reverbSize);
        delayWetSm   .setCurrentAndTargetValue (t.delayWet);
        delayFbSm    .setCurrentAndTargetValue (t.delayFeedback);
        riserLevelSm .setCurrentAndTargetValue (t.riserLevel);
        riserCutoffSm.setCurrentAndTargetValue (t.riserCutoffHz);
        mixSm        .setCurrentAndTargetValue (mix);
        trimSm       .setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputTrimDb));

        reset();
    }

    void reset()
    {
        hpf.reset();
        riserFilter.reset();
        reverb.reset();
        delayLine.reset();
    }

    void setAmount       (float newAmount) noexcept { amount       = newAmount; }
    void setMix          (float newMix)    noexcept { mix          = newMix; }
    void setOutputTrimDb (float db)        noexcept { outputTrimDb = db; }

    void setReverbDepth (float d) noexcept { depths.reverb = d; }
    void setDelayDepth  (float d) noexcept { depths.delay  = d; }
    void setRiserDepth  (float d) noexcept { depths.riser  = d; }
    void setHpfDepth    (float d) noexcept { depths.hpf    = d; }

    void process (juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        const int channels   = juce::jmin (numCh, buffer.getNumChannels());

        if (numSamples <= 0 || channels <= 0)
            return;

        // Keep a clean copy of the input for the final dry/wet mix.
        for (int ch = 0; ch < channels; ++ch)
            dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        const Targets t = mapMacro (amount, depths);
        hpfCutoffSm  .setTargetValue (t.hpfCutoffHz);
        reverbWetSm  .setTargetValue (t.reverbWet);
        reverbSizeSm .setTargetValue (t.reverbSize);
        delayWetSm   .setTargetValue (t.delayWet);
        delayFbSm    .setTargetValue (t.delayFeedback);
        riserLevelSm .setTargetValue (t.riserLevel);
        riserCutoffSm.setTargetValue (t.riserCutoffHz);
        mixSm        .setTargetValue (mix);
        trimSm       .setTargetValue (juce::Decibels::decibelsToGain (outputTrimDb));

        // --- 1. High-pass (per sample so the rising cutoff stays zipper-free) ----
        for (int n = 0; n < numSamples; ++n)
        {
            hpf.setCutoffFrequency (hpfCutoffSm.getNextValue());
            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample (ch, n, hpf.processSample (ch, buffer.getSample (ch, n)));
        }

        // --- 2. Reverb (block based; params are not zipper-prone) ---------------
        {
            const float wet  = reverbWetSm.skip (numSamples);
            const float size = reverbSizeSm.skip (numSamples);

            // juce::dsp::Reverb scales dry by 2x and wet by 3x internally, so a
            // dryLevel of 0.5 is unity. Crossfade dry from unity (wet=0) to 0.
            reverbParams.roomSize   = size;
            reverbParams.wetLevel   = wet;
            reverbParams.dryLevel   = 0.5f * (1.0f - wet);
            reverbParams.damping    = 0.4f;
            reverbParams.width      = 1.0f;
            reverbParams.freezeMode = 0.0f;
            reverb.setParameters (reverbParams);

            const auto reverbChannels = (size_t) juce::jmin (channels, 2);
            juce::dsp::AudioBlock<float> block (buffer);
            auto subBlock = block.getSubsetChannelBlock (0, reverbChannels);
            juce::dsp::ProcessContextReplacing<float> ctx (subBlock);
            reverb.process (ctx);
        }

        // --- 3. Delay / echo with feedback (per sample) -------------------------
        for (int n = 0; n < numSamples; ++n)
        {
            const float wet = delayWetSm.getNextValue();
            const float fb  = delayFbSm.getNextValue();

            for (int ch = 0; ch < channels; ++ch)
            {
                const float in      = buffer.getSample (ch, n);
                const float delayed = delayLine.popSample (ch);
                delayLine.pushSample (ch, in + delayed * fb);
                buffer.setSample (ch, n, in + delayed * wet);
            }
        }

        // --- 4. Riser / filtered-noise sweep (summed in parallel) ---------------
        for (int n = 0; n < numSamples; ++n)
        {
            const float level  = riserLevelSm.getNextValue();
            const float cutoff = riserCutoffSm.getNextValue();

            riserFilter.setCutoffFrequency (cutoff);
            const float noise    = random.nextFloat() * 2.0f - 1.0f;
            const float filtered = riserFilter.processSample (0, noise) * level;

            for (int ch = 0; ch < channels; ++ch)
                buffer.addSample (ch, n, filtered);
        }

        // --- 5. Global dry/wet mix + output trim (per sample) -------------------
        for (int n = 0; n < numSamples; ++n)
        {
            const float wetDry = mixSm.getNextValue();
            const float gain   = trimSm.getNextValue();

            for (int ch = 0; ch < channels; ++ch)
            {
                const float dry = dryBuffer.getSample (ch, n);
                const float wet = buffer.getSample (ch, n);
                buffer.setSample (ch, n, (dry * (1.0f - wetDry) + wet * wetDry) * gain);
            }
        }
    }

private:
    static constexpr int maxDelaySamples = 192000; // up to ~2 s even at 96 kHz

    double sr    = 44100.0;
    int    numCh = 2;

    float amount       = 0.0f;
    float mix          = 1.0f;
    float outputTrimDb = 0.0f;
    Depths depths;

    const double delayTimeSeconds = 0.375; // 1/8 note at ~80 BPM, fixed for v1

    juce::dsp::StateVariableTPTFilter<float> hpf;
    juce::dsp::StateVariableTPTFilter<float> riserFilter;
    juce::dsp::Reverb                        reverb;
    juce::dsp::Reverb::Parameters            reverbParams;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine { maxDelaySamples };
    juce::Random                             random;
    juce::AudioBuffer<float>                 dryBuffer;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> hpfCutoffSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> riserCutoffSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbWetSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbSizeSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> delayWetSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> delayFbSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> riserLevelSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSm;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> trimSm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropEngine)
};
