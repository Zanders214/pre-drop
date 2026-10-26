#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>

#include "RiserSample.h"
#include "RtSafety.h"

/**
    PreDropEngine

    The DSP heart of the "pre-drop" build-up effect, deliberately kept free of any
    host / plugin plumbing so it can be unit-tested or reused on its own.

    A single macro parameter ("amount", 0..1) drives four effects together, so a
    slow turn of one knob takes a steady loop and builds it into the tension
    before a drop:

        amount  0% ............................................. 100%
        HPF     |===================================================|  cut low end
        Reverb  |===================================================|  swell space
        Delay   |===================================================|  washed tail
        Riser   |===================================================|  noise sweep

    Every effect starts with the first percent of Amount and reaches its full
    level at 100%, all on the same curve, so they rise in step. A per-effect
    depth (0..1) then sets how strong each one gets: 0.5 is the balanced
    default, 0 switches the effect off and 1 doubles it.

    Reverb, delay and riser run in parallel off the high-passed signal and are
    each calibrated to the same wet level *relative to that signal*: -6 dB at
    full Amount and depth 0.5, 0 dB at depth 1. The riser follows the signal's
    level, so the balance holds on quiet and loud material alike. The riser is
    band-passed noise by default; setRiserSample swaps in a looping user sample
    at the same calibrated level. The mapping
    from the macro to each effect's targets lives in mapMacro(), a pure function
    and therefore the part most worth unit-testing.
*/
class PreDropEngine
{
public:
    PreDropEngine() = default;

    ~PreDropEngine()
    {
        // Nothing else is running by now, so the hand-over slots can be freed here.
        std::unique_ptr<RiserSample> (incomingRiser.exchange (nullptr));
        collectRetiredRiserSamples();
    }

    /** Per-effect targets derived from the macro, already in engineering units. */
    struct Targets
    {
        float hpfCutoffHz   = 20.0f;   // high-pass cutoff
        float reverbWet     = 0.0f;    // juce::dsp::Reverb wetLevel (wet-only send)
        float reverbSize    = 0.3f;    // 0..1 room size
        float delayWet      = 0.0f;    // echo gain
        float delayFeedback = 0.0f;    // 0..maxDelayFeedback, well below 1 for stability
        float riserLevel    = 0.0f;    // riser RMS as a fraction of the signal's RMS
        float riserCutoffHz = 200.0f;  // band-pass centre of the riser
    };

    /** Per-effect depth (0..1): how strong each effect gets at full Amount.

        0.5 is the balanced default (every effect at the same level), 0 switches
        the effect off and 1 doubles it. Amount still owns the build-up itself;
        depth only scales how far each effect goes. */
    struct Depths
    {
        float hpf    = 0.5f;   // top of the HPF sweep: 20 Hz (off) .. 800 Hz .. 3.2 kHz
        float reverb = 0.5f;
        float delay  = 0.5f;
        float riser  = 0.5f;
    };

    /** Wet level of reverb, delay and riser relative to the signal, at full
        Amount and the default depth of 0.5 (-6 dB). */
    static constexpr float nominalWetRatio = 0.5f;

    /** Delay feedback at full Amount; see mapMacro for why it stops here. */
    static constexpr float maxDelayFeedback = 0.6f;

    /** Gain an effect's depth applies to its nominal level: 0 -> off,
        0.5 -> 1x (balanced), 1 -> 2x. */
    static float depthGain (float depth) noexcept { return 2.0f * juce::jlimit (0.0f, 1.0f, depth); }

    /** Octaves above 20 Hz the HPF reaches at full Amount: 0 at depth 0 (off),
        log2(800/20) at the default 0.5, two octaves more (3.2 kHz) at depth 1.
        Mirrored by PreDropVisualModel::hpfTopOctaves for the cutoff readout. */
    static float hpfTopOctaves (float depth) noexcept
    {
        const float d = juce::jlimit (0.0f, 1.0f, depth);
        constexpr float nominal = 5.321928f;   // log2 (800 / 20)
        constexpr float maximum = 7.321928f;   // log2 (3200 / 20)
        return d <= 0.5f ? nominal * (d / 0.5f)
                         : nominal + (maximum - nominal) * ((d - 0.5f) / 0.5f);
    }

    /** Wet-only gain of juce::dsp::Reverb (wetLevel 1, damping 0.4, width 1) for a
        room size, measured on pink noise at 44.1 / 48 kHz. Its combs ring longer
        as the room grows, so the tail gets louder; dividing by this keeps the
        reverb at the level mapMacro asks for whatever the size. */
    static float reverbWetGain (float roomSize) noexcept
    {
        const float g = roomSize * 0.28f + 0.7f;   // juce::Reverb's comb feedback
        return (1.456f - 0.254f * roomSize) / std::sqrt (1.0f - g * g);
    }

    /** Pure macro -> targets mapping with every effect at its default depth. */
    static Targets mapMacro (float amount) noexcept { return mapMacro (amount, Depths {}); }

    /** Pure macro -> targets mapping. No state, so it is trivially unit-testable.

        A default argument can't be used here: Depths is a nested type and its
        default member initialisers aren't available in this member's default
        argument, so the no-depth overload above delegates from its body. */
    static Targets mapMacro (float amount, Depths depth) noexcept
    {
        const float p = juce::jlimit (0.0f, 1.0f, amount);
        Targets t;

        // 1. High-pass: sweeps up evenly in pitch (octaves) as Amount rises.
        t.hpfCutoffHz = 20.0f * std::exp2 (hpfTopOctaves (depth.hpf) * p);

        // 2. Reverb: the room grows with Amount; the wet send is compensated for
        //    the louder tail so its level rises in step with the others.
        t.reverbSize = 0.30f + 0.65f * p;
        t.reverbWet  = nominalWetRatio * depthGain (depth.reverb) * p / reverbWetGain (t.reverbSize);

        // 3. Delay: feedback grows with Amount, and the echo gain is scaled by
        //    sqrt(1 - fb^2) because a feedback loop's steady-state energy is
        //    1 / (1 - fb^2) times one echo; this holds the delay level in step.
        //    That energy sum is exact for material whose echoes don't line up.
        //    Echoes that do (a tone or loop whose period divides the delay time)
        //    add in phase to 1 / (1 - fb), sqrt((1 + fb) / (1 - fb)) times more.
        //    Capping feedback at 0.6 bounds that to 2x (+6 dB, ~5 s tail) rather
        //    than 3.5x (+11 dB) at 0.85.
        t.delayFeedback = maxDelayFeedback * p;
        t.delayWet      = nominalWetRatio * depthGain (depth.delay) * p
                        * std::sqrt (1.0f - t.delayFeedback * t.delayFeedback);

        // 4. Riser: level relative to the signal, sweeping 200 Hz -> 8 kHz.
        t.riserLevel    = nominalWetRatio * depthGain (depth.riser) * p;
        t.riserCutoffHz = 200.0f * std::pow (8000.0f / 200.0f, p);

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

        riser.filter.prepare (spec);
        riser.filter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        riser.filter.setResonance (1.5f);

        // Set the reverb's parameters before preparing it: prepare() snaps its
        // internal smoothers, so the wet-only send starts with no dry leaking in.
        const Targets t = mapMacro (amount, depths);
        applyReverbParameters (t.reverbSize, t.reverbWet);
        reverb.prepare (spec);

        delayLine.setMaximumDelayInSamples (maxDelaySamples);
        delayLine.prepare (spec);
        delayLine.setDelay ((float) (delayTimeSeconds * sampleRate));

        // Not running concurrently with process(), so a riser sample handed over
        // since the last block (typically one rebuilt for this sample rate) can
        // be adopted, and the one it replaces freed, right here.
        if (auto* next = incomingRiser.exchange (nullptr))
            activeRiser.reset (next);

        dryBuffer   .setSize (numCh, juce::jmax (1, maximumBlockSize), false, false, true);
        reverbBuffer.setSize (numCh, juce::jmax (1, maximumBlockSize), false, false, true);

        const double ramp = 0.04; // 40 ms, fast enough to track but kills zipper noise
        sm.hpfCutoff  .reset (sampleRate, ramp);
        sm.reverbWet  .reset (sampleRate, ramp);
        sm.reverbSize .reset (sampleRate, ramp);
        sm.delayWet   .reset (sampleRate, ramp);
        sm.delayFb    .reset (sampleRate, ramp);
        sm.riserLevel .reset (sampleRate, ramp);
        sm.riserCutoff.reset (sampleRate, ramp);
        sm.mix        .reset (sampleRate, ramp);
        sm.trim       .reset (sampleRate, ramp);

        // Snap smoothers to the current targets so the first block does not sweep.
        sm.hpfCutoff  .setCurrentAndTargetValue (t.hpfCutoffHz);
        sm.reverbWet  .setCurrentAndTargetValue (t.reverbWet);
        sm.reverbSize .setCurrentAndTargetValue (t.reverbSize);
        sm.delayWet   .setCurrentAndTargetValue (t.delayWet);
        sm.delayFb    .setCurrentAndTargetValue (t.delayFeedback);
        sm.riserLevel .setCurrentAndTargetValue (t.riserLevel);
        sm.riserCutoff.setCurrentAndTargetValue (std::min (t.riserCutoffHz, 0.45f * (float) sampleRate));
        sm.mix        .setCurrentAndTargetValue (mix);
        sm.trim       .setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputTrimDb));

        reset();
    }

    void reset()
    {
        hpf.reset();
        riser.filter.reset();
        reverb.reset();
        delayLine.reset();

        // Re-prime the coefficient caches so the next block re-applies the
        // cutoffs, and let the riser's level follower start from silence.
        lastHpfCutoff      = -1.0f;
        riser.lastCutoff   = -1.0f;
        riser.meanSquare   = 0.0f;
        riser.signalRms    = 0.0f;
        riser.samplePos    = 0;
    }

    /** Hands the engine a riser sample built for its current sample rate, or
        RiserSample::none() to go back to the built-in noise. Call from the
        message thread: process() adopts it at the start of a block without
        allocating or locking, and returns the replaced one for
        collectRetiredRiserSamples() to free. */
    void setRiserSample (std::unique_ptr<RiserSample> sample)
    {
        collectRetiredRiserSamples();
        // A sample handed over earlier but not adopted yet is simply replaced.
        std::unique_ptr<RiserSample> (incomingRiser.exchange (sample.release()));
    }

    /** Frees riser samples the audio thread has swapped out. Message thread. */
    void collectRetiredRiserSamples()
    {
        std::unique_ptr<RiserSample> (retiredRiser.exchange (nullptr));
    }

    void setAmount       (float newAmount) noexcept { amount       = newAmount; }
    void setMix          (float newMix)    noexcept { mix          = newMix; }
    void setOutputTrimDb (float db)        noexcept { outputTrimDb = db; }

    void setReverbDepth (float d) noexcept { depths.reverb = d; }
    void setDelayDepth  (float d) noexcept { depths.delay  = d; }
    void setRiserDepth  (float d) noexcept { depths.riser  = d; }
    void setHpfDepth    (float d) noexcept { depths.hpf    = d; }

    // The audio-thread hot path. Annotated nonblocking so RealtimeSanitizer
    // verifies it never allocates, locks, or makes a syscall: every primitive it
    // touches (TPT filters, juce::dsp::Reverb, DelayLine, juce::Random) is
    // alloc/lock-free once prepare() has run.
    void process (juce::AudioBuffer<float>& buffer) PREDROP_RT_NONBLOCKING
    {
        const int numSamples = buffer.getNumSamples();
        const int channels   = juce::jmin (numCh, buffer.getNumChannels());

        if (numSamples <= 0 || channels <= 0)
            return;

        // Adopt a newly handed-over riser sample. The replaced one goes to the
        // retired slot for the message thread to free; if that slot is still
        // occupied, wait a block rather than lose it.
        if (retiredRiser.load() == nullptr)
            if (auto* next = incomingRiser.exchange (nullptr))
            {
                retiredRiser.store (activeRiser.release());
                activeRiser.reset (next);
                riser.samplePos = 0;
            }

        // Keep a clean copy of the input for the final dry/wet mix.
        for (int ch = 0; ch < channels; ++ch)
            dryBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        const Targets t = mapMacro (amount, depths);
        sm.hpfCutoff  .setTargetValue (t.hpfCutoffHz);
        sm.reverbWet  .setTargetValue (t.reverbWet);
        sm.reverbSize .setTargetValue (t.reverbSize);
        sm.delayWet   .setTargetValue (t.delayWet);
        sm.delayFb    .setTargetValue (t.delayFeedback);
        sm.riserLevel .setTargetValue (t.riserLevel);
        sm.riserCutoff.setTargetValue (std::min (t.riserCutoffHz, 0.45f * (float) sr));   // clear of Nyquist
        sm.mix        .setTargetValue (mix);
        sm.trim       .setTargetValue (juce::Decibels::decibelsToGain (outputTrimDb));

        auto* const* data = buffer.getArrayOfWritePointers();

        // --- 1. High-pass (per sample so the rising cutoff stays zipper-free) ----
        for (int n = 0; n < numSamples; ++n)
        {
            // The cutoff only moves while the smoother ramps; recomputing the TPT
            // coefficient (an std::tan) every sample at a steady setting is pure
            // waste, so only push it to the filter when it actually changes.
            if (const float fc = sm.hpfCutoff.getNextValue(); ! juce::exactlyEqual (fc, lastHpfCutoff))
            {
                hpf.setCutoffFrequency (fc);
                lastHpfCutoff = fc;
            }

            for (int ch = 0; ch < channels; ++ch)
                data[ch][n] = hpf.processSample (ch, data[ch][n]);
        }

        // The high-passed signal is what reverb, delay and riser all ride on; the
        // riser follows its level (smoothed over ~0.4 s) so it stays in step.
        const float riserRmsStart = riser.signalRms;
        {
            double sum = 0.0;
            for (int ch = 0; ch < channels; ++ch)
                for (int n = 0; n < numSamples; ++n)
                    sum += (double) data[ch][n] * data[ch][n];

            const auto blockMeanSquare = (float) (sum / (double) (numSamples * channels));
            const auto coeff = (float) (1.0 - std::exp (-(double) numSamples / (riserFollowSeconds * sr)));
            riser.meanSquare += coeff * (blockMeanSquare - riser.meanSquare);
            riser.signalRms   = std::sqrt (riser.meanSquare);
        }

        // --- 2. Reverb, wet only, on a copy of the high-passed signal ------------
        const int reverbChannels = juce::jmin (channels, 2);
        {
            const float size = sm.reverbSize.skip (numSamples);
            applyReverbParameters (size, sm.reverbWet.skip (numSamples));

            for (int ch = 0; ch < reverbChannels; ++ch)
                reverbBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

            juce::dsp::AudioBlock<float> block (reverbBuffer);
            auto subBlock = block.getSubsetChannelBlock (0, (size_t) reverbChannels)
                                 .getSubBlock (0, (size_t) numSamples);
            juce::dsp::ProcessContextReplacing ctx (subBlock);
            reverb.process (ctx);
        }

        // --- 3. Delay / echo with feedback, then sum signal + echoes + reverb -----
        auto* const* reverbWet = reverbBuffer.getArrayOfReadPointers();
        for (int n = 0; n < numSamples; ++n)
        {
            const float wet = sm.delayWet.getNextValue();
            const float fb  = sm.delayFb.getNextValue();

            for (int ch = 0; ch < channels; ++ch)
            {
                const float in      = data[ch][n];
                const float delayed = delayLine.popSample (ch);
                delayLine.pushSample (ch, in + delayed * fb);
                data[ch][n] = in + delayed * wet + (ch < reverbChannels ? reverbWet[ch][n] : 0.0f);
            }
        }

        // --- 4. Riser: user sample or filtered-noise sweep (summed in parallel) --
        const RiserSample* sample = activeRiser.get();
        const bool useSample = sample != nullptr && sample->length() > 0;

        const float rmsStep = (riser.signalRms - riserRmsStart) / (float) numSamples;
        float signalRms = riserRmsStart;
        for (int n = 0; n < numSamples; ++n)
        {
            const float level = sm.riserLevel.getNextValue();
            signalRms += rmsStep;

            if (useSample)
            {
                if (level <= 0.0f)
                {
                    // Fully down (Amount or riser depth at 0): hold at the top,
                    // so the next build-up starts on the sample's first sample.
                    riser.samplePos = 0;
                }
                else
                {
                    // Loop the sample at unit RMS, scaled exactly like the noise.
                    const float gain = sample->unitGain * level * signalRms;
                    const int last = sample->audio.getNumChannels() - 1;
                    for (int ch = 0; ch < channels; ++ch)
                        data[ch][n] += sample->audio.getSample (juce::jmin (ch, last), riser.samplePos) * gain;

                    if (++riser.samplePos >= sample->length())
                        riser.samplePos = 0;
                }

                sm.riserCutoff.getNextValue();   // keep the sweep in step for a switch back to noise
                continue;
            }

            // Same coefficient caching as the HPF: skip the per-sample std::tan
            // when the band-pass centre is parked. The gain that brings the
            // band-passed noise to unit RMS is recomputed alongside it.
            if (const float cutoff = sm.riserCutoff.getNextValue(); ! juce::exactlyEqual (cutoff, riser.lastCutoff))
            {
                riser.filter.setCutoffFrequency (cutoff);
                riser.unitGain   = unitRmsGain (cutoff);
                riser.lastCutoff = cutoff;
            }

            const float noise    = random.nextFloat() * 2.0f - 1.0f;
            const float filtered = riser.filter.processSample (0, noise) * riser.unitGain * level * signalRms;

            for (int ch = 0; ch < channels; ++ch)
                data[ch][n] += filtered;
        }

        // --- 5. Global dry/wet mix + output trim (per sample) -------------------
        auto* const* dry = dryBuffer.getArrayOfReadPointers();
        for (int n = 0; n < numSamples; ++n)
        {
            const float wetDry = sm.mix.getNextValue();
            const float gain   = sm.trim.getNextValue();

            for (int ch = 0; ch < channels; ++ch)
                data[ch][n] = (dry[ch][n] * (1.0f - wetDry) + data[ch][n] * wetDry) * gain;
        }
    }

private:
    void applyReverbParameters (float roomSize, float wetLevel)
    {
        juce::dsp::Reverb::Parameters params;
        params.roomSize   = roomSize;
        params.wetLevel   = wetLevel;
        params.dryLevel   = 0.0f;   // wet only: the signal is summed back in process()
        params.damping    = 0.4f;
        params.width      = 1.0f;
        params.freezeMode = 0.0f;
        reverb.setParameters (params);
    }

    static constexpr int maxDelaySamples = 192000; // up to ~2 s even at 96 kHz
    static constexpr double delayTimeSeconds = 0.375; // 1/8 note at ~80 BPM, fixed for v1
    static constexpr double riserFollowSeconds = 0.4; // riser level-follower time constant

    // Gain that brings uniform [-1, 1] noise through the riser's Q = 1.5
    // band-pass to unit RMS. A constant-Q band-pass passes noise power in
    // proportion to fc / fs, narrowing near Nyquist; measured at 44.1 / 48 /
    // 96 kHz from 200 Hz to 8 kHz, RMS = 1.215 sqrt(x) sin(2 pi x) / (2 pi x)
    // with x = fc / fs, to within 0.25 dB.
    float unitRmsGain (float cutoffHz) const noexcept
    {
        const double x     = (double) cutoffHz / sr;
        const double twoPx = juce::MathConstants<double>::twoPi * x;
        return (float) (1.0 / (1.215 * std::sqrt (x) * std::sin (twoPx) / twoPx));
    }

    double sr    = 44100.0;
    int    numCh = 2;

    float amount       = 0.0f;
    float mix          = 1.0f;
    float outputTrimDb = 0.0f;
    Depths depths;

    juce::dsp::StateVariableTPTFilter<float> hpf;
    juce::dsp::Reverb                        reverb;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine { maxDelaySamples };
    juce::Random                             random;
    juce::AudioBuffer<float>                 dryBuffer;
    juce::AudioBuffer<float>                 reverbBuffer;

    // Last cutoff pushed to the HPF, so process() can skip recomputing an
    // identical coefficient (an std::tan) every sample. -1 forces the first apply.
    float lastHpfCutoff = -1.0f;

    // The riser: the noise band-pass, its coefficient cache and unit-RMS gain
    // for the current centre, the level follower that ties it to the signal,
    // and the playback position in a user sample.
    struct Riser
    {
        juce::dsp::StateVariableTPTFilter<float> filter;
        float lastCutoff = -1.0f;
        float unitGain   = 1.0f;
        float meanSquare = 0.0f;
        float signalRms  = 0.0f;
        int   samplePos  = 0;
    };
    Riser riser;

    // User riser sample, owned by the audio thread once adopted. New ones arrive
    // through `incomingRiser`; replaced ones leave through `retiredRiser`.
    std::unique_ptr<RiserSample> activeRiser;
    std::atomic<RiserSample*>    incomingRiser { nullptr };
    std::atomic<RiserSample*>    retiredRiser  { nullptr };

    // One ramp per mapped target, so every parameter change glides instead of
    // stepping. Cutoffs ramp multiplicatively (even in pitch), the rest linearly.
    struct Smoothers
    {
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> hpfCutoff;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> riserCutoff;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbWet;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> reverbSize;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> delayWet;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> delayFb;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> riserLevel;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mix;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> trim;
    };
    Smoothers sm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropEngine)
};
