#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

#include "EffectSettings.h"
#include "RiserSample.h"
#include "RtHandoff.h"
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
    at the same calibrated level. Reverb and delay can likewise be swapped for
    external effects (hosted plugins) via setExternalEffects. The mapping
    from the macro to each effect's targets lives in mapMacro(), a pure function
    and therefore the part most worth unit-testing.
*/
class PreDropEngine
{
public:
    PreDropEngine() = default;

    /** A reverb or delay supplied from outside (a hosted plugin), used in place
        of the built-in one. It gets the high-passed signal and must return its
        100%-wet output in place; the engine sets its level exactly as it would
        the built-in effect's. Called on the audio thread. */
    struct ExternalEffect
    {
        virtual ~ExternalEffect() = default;
        virtual void process (float* const* wet, int numChannels, int numSamples) noexcept = 0;
    };

    /** Per-effect targets derived from the macro, already in engineering units. */
    struct Targets
    {
        float hpfCutoffHz   = 20.0f;   // high-pass cutoff
        float reverbWet     = 0.0f;    // juce::dsp::Reverb wetLevel (wet-only send)
        float reverbSize    = 0.3f;    // 0..1 room size
        float delayWet      = 0.0f;    // echo gain
        float delayFeedback = 0.0f;    // 0..FEEDBACK (at most 0.9), below 1 for stability
        float riserLevel    = 0.0f;    // riser RMS as a fraction of the signal's RMS
        float reverbLevel   = 0.0f;    // reverb / delay wet level as a fraction of the
        float delayLevel    = 0.0f;    // signal (what an external effect is scaled to)
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

    /** Where the host's transport is at the start of a block, for a riser
        sample that follows the project. Positions are in quarter notes. */
    struct Transport
    {
        bool   playing    = false;
        double bpm        = 0.0;    // 0: the host gives no tempo
        double ppq        = 0.0;    // position at the block's first sample
        double barStart   = 0.0;    // position of the bar that contains it
        double barLength  = 4.0;    // quarter notes per bar in the host's meter
    };

    /** Wet level of reverb, delay and riser relative to the signal, at full
        Amount and the default depth of 0.5 (-6 dB). */
    static constexpr float nominalWetRatio = 0.5f;

    /** Delay feedback at full Amount by default (FEEDBACK); see mapMacro for
        why it stops here. */
    static constexpr float maxDelayFeedback = 0.6f;

    /** Room size at the start of the reverb's sweep, as a share of SIZE (the
        default 95% grows from 0.30). */
    static constexpr float reverbStartShare = 0.30f / 0.95f;

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

    /** ... and with the effects' default settings. */
    static Targets mapMacro (float amount, Depths depth) noexcept { return mapMacro (amount, depth, EffectSettings {}); }

    /** Pure macro -> targets mapping. No state, so it is trivially unit-testable.

        Default arguments can't be used here: Depths is a nested type and its
        default member initialisers aren't available in this member's default
        argument, so the shorter overloads above delegate from their bodies. */
    static Targets mapMacro (float amount, Depths depth, const EffectSettings& settings) noexcept
    {
        const float p = juce::jlimit (0.0f, 1.0f, amount);
        Targets t;

        // 1. High-pass: sweeps up evenly in pitch (octaves) as Amount rises.
        t.hpfCutoffHz = minHpfCutoffHz * std::exp2 (hpfTopOctaves (depth.hpf) * p);

        // 2. Reverb: the room grows with Amount up to SIZE; the wet send is
        //    compensated for the louder tail so its level rises in step with
        //    the others.
        const float size = settings[EffectSettings::reverbSize];
        t.reverbSize  = size * (reverbStartShare + (1.0f - reverbStartShare) * p);
        t.reverbLevel = nominalWetRatio * depthGain (depth.reverb) * p;
        t.reverbWet   = t.reverbLevel / reverbWetGain (t.reverbSize);

        // 3. Delay: feedback grows with Amount, and the echo gain is scaled by
        //    sqrt(1 - fb^2) because a feedback loop's steady-state energy is
        //    1 / (1 - fb^2) times one echo; this holds the delay level in step.
        //    That energy sum is exact for material whose echoes don't line up.
        //    Echoes that do (a tone or loop whose period divides the delay time)
        //    add in phase to 1 / (1 - fb), sqrt((1 + fb) / (1 - fb)) times more.
        //    The default FEEDBACK of 0.6 bounds that to 2x (+6 dB, ~5 s tail)
        //    rather than 3.5x (+11 dB) at 0.85.
        t.delayFeedback = settings[EffectSettings::delayFeedback] * p;
        t.delayLevel    = nominalWetRatio * depthGain (depth.delay) * p;
        t.delayWet      = t.delayLevel * std::sqrt (1.0f - t.delayFeedback * t.delayFeedback);

        // 4. Riser: level relative to the signal, sweeping evenly in pitch from
        //    START to END (200 Hz -> 8 kHz by default).
        const float start = settings[EffectSettings::riserStart];
        t.riserLevel    = nominalWetRatio * depthGain (depth.riser) * p;
        t.riserCutoffHz = start * std::pow (settings[EffectSettings::riserEnd] / start, p);

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

        for (auto* stage : { &hpf, &hpf2 })
        {
            stage->prepare (spec);
            stage->setType (juce::dsp::StateVariableTPTFilterType::highpass);
        }

        riser.filter.prepare (spec);
        riser.filter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        applied = {};
        applySettings();

        // Set the reverb's parameters before preparing it: prepare() snaps its
        // internal smoothers, so the wet-only send starts with no dry leaking in.
        const Targets t = mapMacro (amount, depths, settings);
        applyReverbParameters (t.reverbSize, t.reverbWet);
        reverb.prepare (spec);

        preDelay.line.setMaximumDelayInSamples ((int) std::ceil (maxPreDelaySeconds * sampleRate) + 2);
        preDelay.line.prepare (spec);

        delayLine.setMaximumDelayInSamples ((int) std::ceil (EffectSettings::maxDelaySeconds * sampleRate) + 2);
        delayLine.prepare (spec);

        // Not running concurrently with process(), so a riser sample handed over
        // since the last block (typically one rebuilt for this sample rate) can
        // be adopted, and the one it replaces freed, right here.
        riserSample.adoptNow();

        buffers.dry     .setSize (numCh, juce::jmax (1, maximumBlockSize), false, false, true);
        buffers.reverb  .setSize (numCh, juce::jmax (1, maximumBlockSize), false, false, true);
        buffers.external.setSize (2 * externalChannels, juce::jmax (1, maximumBlockSize), false, false, true);

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
        sm.hpfBlend   .reset (sampleRate, 0.03);   // in / out of the high-pass
        sm.extReverb  .reset (sampleRate, ramp);
        sm.extDelay   .reset (sampleRate, ramp);
        sm.delayTime  .reset (sampleRate, 0.1);    // a short glide, not a jump, to a new time
        sm.preDelay   .reset (sampleRate, 0.1);

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
        sm.hpfBlend   .setCurrentAndTargetValue (t.hpfCutoffHz > minHpfCutoffHz ? 1.0f : 0.0f);
        sm.delayTime  .setCurrentAndTargetValue (delayTimeSamples());
        sm.preDelay   .setCurrentAndTargetValue (preDelaySamples());
        delayLine.setDelay (sm.delayTime.getCurrentValue());
        preDelay.line.setDelay (sm.preDelay.getCurrentValue());

        reset();
    }

    void reset()
    {
        hpf.reset();
        hpf2.reset();
        riser.filter.reset();
        reverb.reset();
        preDelay.line.reset();
        delayLine.reset();
        tone = {};

        // Re-prime the coefficient caches so the next block re-applies the
        // cutoffs, and let the riser's level follower start from silence.
        lastHpfCutoff      = -1.0f;
        idle               = {};
        riser.lastCutoff   = -1.0f;
        riser.meanSquare   = 0.0f;
        riser.signalRms    = 0.0f;
        riser.samplePos    = 0.0;
        riser.armed        = true;
        riser.wait         = 0;
        riserPlayhead      = -1.0f;
    }

    /** Hands the engine a riser sample built for its current sample rate, or
        RiserSample::none() to go back to the built-in noise. Call from the
        message thread: process() adopts it at the start of a block without
        allocating or locking, and returns the replaced one for
        collectRetiredRiserSamples() to free. */
    void setRiserSample (std::unique_ptr<RiserSample> sample) { riserSample.send (std::move (sample)); }

    /** Frees riser samples the audio thread has swapped out. Message thread. */
    void collectRetiredRiserSamples() { riserSample.collect(); }

    /** The host's transport for this block. Audio thread, before process(). */
    void setTransport (const Transport& now) noexcept { transport = now; }

    /** Where a riser sample is in its loop (0..1), or -1 while it isn't
        playing (no sample, the riser is down, or it waits for a bar line).
        Any thread. */
    float getRiserPlayhead() const noexcept { return riserPlayhead.load(); }

    /** The external reverb and delay for this block (nullptr: the built-in
        one). Audio thread, before process(). Switching crossfades over 40 ms,
        except that a removed effect stops at once (it can't be called again). */
    void setExternalEffects (ExternalEffect* reverbEffect, ExternalEffect* delayEffect) noexcept
    {
        external.reverb = reverbEffect;
        external.delay  = delayEffect;
    }

    void setAmount       (float newAmount) noexcept { amount       = newAmount; }
    void setMix          (float newMix)    noexcept { mix          = newMix; }
    void setOutputTrimDb (float db)        noexcept { outputTrimDb = db; }

    /** How the built-in effects sound (see EffectSettings). Audio thread,
        before process(); a plain copy. */
    void setEffectSettings (const EffectSettings& newSettings) noexcept { settings = newSettings; }

    void setReverbDepth (float d) noexcept { depths.reverb = d; }
    void setDelayDepth  (float d) noexcept { depths.delay  = d; }
    void setRiserDepth  (float d) noexcept { depths.riser  = d; }
    void setHpfDepth    (float d) noexcept { depths.hpf    = d; }

    /** Wet only: output just the effects (echoes, reverb, riser), not the
        signal they ride on. For a return track, where the dry signal already
        reaches the mix through each track and must not be doubled. */
    void setWetOnly (bool shouldBeWetOnly) noexcept { wetOnly = shouldBeWetOnly; }

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

        adoptRiserSample();

        // Keep a clean copy of the input for the final dry/wet mix.
        for (int ch = 0; ch < channels; ++ch)
            buffers.dry.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        applySettings();
        setTargets();
        const Engaged on = engageEffects();
        const Block block { buffer.getArrayOfWritePointers(), channels, numSamples };

        // --- 1. High-pass (per sample so the rising cutoff stays zipper-free) ----
        if (on.hpf)
            processHighPass (block);

        // The high-passed signal is what reverb, delay and riser all ride on.
        const float riserRmsStart = followSignalLevel (block);
        keepExternalSends (buffer, channels, numSamples);

        // --- 2. Reverb, wet only, on a copy of the high-passed signal ------------
        const int reverbChannels = juce::jmin (channels, 2);
        runReverb (buffer, on.reverb, reverbChannels);

        // --- 3. Delay / echo with feedback (fed the high-passed signal) ---------
        runDelay (block, on.delay);

        // Sum the reverb in alongside the signal and echoes.
        if (on.reverb)
            for (int ch = 0; ch < reverbChannels; ++ch)
                juce::FloatVectorOperations::add (block.data[ch], buffers.reverb.getReadPointer (ch), numSamples);

        // --- 3b. External reverb / delay: each gets the high-passed signal (kept
        //     aside above) and its wet output is summed in at the calibrated level.
        runExternal (external.reverb, sm.extReverb, 0, block.data, channels, numSamples);
        runExternal (external.delay,  sm.extDelay,  1, block.data, channels, numSamples);

        // --- 4. Riser: user sample or filtered-noise sweep (summed in parallel) --
        if (on.riser)
        {
            addRiser (block, riserRmsStart);
        }
        else
        {
            sm.riserLevel.skip (numSamples);
            sm.riserCutoff.skip (numSamples);
            riserPlayhead = -1.0f;
        }

        // --- 5. Global dry/wet mix + output trim (per sample) -------------------
        mixAndTrim (block);
    }

private:
    // The block process() works on: the buffer's first `channels` channels.
    struct Block
    {
        float* const* data;
        int           channels;
        int           numSamples;
    };

    // Which effects run this block.
    struct Engaged
    {
        bool hpf, reverb, delay, riser;
    };

    // Points every smoother at what Amount and the depths ask for. An external
    // reverb / delay takes over from the built-in one: the built-in ramps out
    // while the external one ramps in at the same level.
    void setTargets() noexcept
    {
        const Targets t = mapMacro (amount, depths, settings);
        sm.hpfCutoff  .setTargetValue (t.hpfCutoffHz);
        sm.reverbWet  .setTargetValue (external.reverb != nullptr ? 0.0f : t.reverbWet);
        sm.reverbSize .setTargetValue (t.reverbSize);
        sm.delayWet   .setTargetValue (external.delay != nullptr ? 0.0f : t.delayWet);
        sm.extReverb  .setTargetValue (external.reverb != nullptr ? t.reverbLevel : 0.0f);
        sm.extDelay   .setTargetValue (external.delay  != nullptr ? t.delayLevel  : 0.0f);
        sm.delayFb    .setTargetValue (t.delayFeedback);
        sm.riserLevel .setTargetValue (t.riserLevel);
        sm.riserCutoff.setTargetValue (std::min (t.riserCutoffHz, 0.45f * (float) sr));   // clear of Nyquist
        sm.mix        .setTargetValue (mix);
        sm.trim       .setTargetValue (juce::Decibels::decibelsToGain (outputTrimDb));
        sm.delayTime  .setTargetValue (delayTimeSamples());
        sm.preDelay   .setTargetValue (preDelaySamples());
    }

    // The settings that aren't swept by Amount: pushed to the filters only
    // when they change (each push recomputes coefficients).
    void applySettings() noexcept
    {
        const bool  steep = settings.hpfSteep();
        const float q     = settings.hpfQ();
        if (steep != applied.hpfSteep || ! juce::exactlyEqual (q, applied.hpfQ))
        {
            // 24 dB: two stages with a Butterworth pair of Qs, both scaled by
            // the resonance (12 dB: the one stage at its Q).
            const float scale = q * juce::MathConstants<float>::sqrt2;   // 1 with no resonance
            hpf.setResonance  (steep ? 0.5411961f * scale : q);
            hpf2.setResonance (steep ? 1.3065630f * scale : q);
            applied.hpfSteep = steep;
            applied.hpfQ     = q;
        }

        if (const float rq = settings.riserQ(); ! juce::exactlyEqual (rq, applied.riserQ))
        {
            riser.filter.setResonance (rq);
            riser.qGain      = riserQGain (rq);
            riser.lastCutoff = -1.0f;                   // recompute the unit gain
            applied.riserQ   = rq;
        }

        // A one-pole low-pass in the delay's repeats (none when TONE is open),
        // and the gain that keeps the darker repeats at the delay's level.
        const float toneHz = settings[EffectSettings::delayTone];
        const bool  open   = toneHz >= EffectSettings::toneOpenHz - 1.0f;
        tone.coeff = open ? 1.0f : 1.0f - std::exp (-juce::MathConstants<float>::twoPi * toneHz / (float) sr);
        tone.gain  = open ? 1.0f : toneGain (toneHz, sm.delayFb.getTargetValue());
    }

    /** Gain that keeps the delay at its level through TONE's low-pass, which
        takes about half a dB per octave below 20 kHz out of pink noise, a
        little more as the repeats pile up (measured at 48 kHz, to within
        0.4 dB from 500 Hz to 16 kHz). */
    static float toneGain (float toneHz, float feedback) noexcept
    {
        const float octaves = std::log2 (EffectSettings::toneOpenHz / juce::jmax (1.0f, toneHz));
        return juce::Decibels::decibelsToGain ((0.1f + 0.5f * juce::jmax (0.0f, octaves)) * (1.0f + 0.25f * feedback));
    }

    /** Gain that keeps the noise riser at its level at band-pass Q `q`: its
        unit gain is measured at Q 1.5, and band-passed noise grows roughly as
        sqrt (Q), a little less as the band narrows (fitted on the sweep's top,
        to within 0.1 dB from Q 0.5 to 8). */
    static float riserQGain (float q) noexcept
    {
        const float x = std::log2 (q / 1.5f);
        return std::sqrt (1.5f / q) * juce::Decibels::decibelsToGain (-(0.72f * x - 0.15f * x * x));
    }

    float delayTimeSamples() const noexcept
    {
        return (float) juce::jlimit (1.0, EffectSettings::maxDelaySeconds * sr, settings.delaySeconds (transport.bpm) * sr);
    }

    float preDelaySamples() const noexcept
    {
        return (float) juce::jlimit (0.0, maxPreDelaySeconds * sr, settings[EffectSettings::reverbPreDelay] / 1000.0 * sr);
    }

    // An effect whose control has settled at "off" is skipped outright (and
    // reset once on the way, so it re-engages from silence). At Amount 0, or
    // on a linked track with nothing switched on, the audio passes through
    // bit for bit and costs next to nothing.
    Engaged engageEffects() noexcept
    {
        const auto settledAtOff = [] (const auto& smoother, float off)
        {
            return ! smoother.isSmoothing() && smoother.getCurrentValue() <= off;
        };
        // The high-pass is the one effect in line with the signal, and even
        // parked at 20 Hz it shifts the low end's phase, so it can't simply be
        // switched out: it blends to the unfiltered signal over a short ramp on
        // the way out (and back in), and is only skipped once fully dry. The
        // other effects are added on top, so their own level ramps do that.
        const bool hpfWanted = ! settledAtOff (sm.hpfCutoff, minHpfCutoffHz);
        sm.hpfBlend.setTargetValue (hpfWanted ? 1.0f : 0.0f);
        return {
            runOrIdle (idle.hpf,    ! hpfWanted && settledAtOff (sm.hpfBlend, 0.0f), [this] { hpf.reset(); hpf2.reset(); }),
            runOrIdle (idle.reverb, settledAtOff (sm.reverbWet, 0.0f),           [this] { reverb.reset(); preDelay.line.reset(); }),
            runOrIdle (idle.delay,  settledAtOff (sm.delayWet, 0.0f),            [this] { delayLine.reset(); tone.state = {}; }),
            runOrIdle (idle.riser,  settledAtOff (sm.riserLevel, 0.0f),          [this] { riser.filter.reset(); riser.samplePos = 0.0; riser.armed = true; })
        };
    }

    void processHighPass (const Block& b) noexcept
    {
        for (int n = 0; n < b.numSamples; ++n)
        {
            // The cutoff only moves while the smoother ramps; recomputing the TPT
            // coefficient (an std::tan) every sample at a steady setting is pure
            // waste, so only push it to the filter when it actually changes.
            if (const float fc = sm.hpfCutoff.getNextValue(); ! juce::exactlyEqual (fc, lastHpfCutoff))
            {
                hpf.setCutoffFrequency (fc);
                hpf2.setCutoffFrequency (fc);
                lastHpfCutoff = fc;
            }

            const float blend = sm.hpfBlend.getNextValue();
            for (int ch = 0; ch < b.channels; ++ch)
            {
                const float dry = b.data[ch][n];
                float filtered  = hpf.processSample (ch, dry);
                if (applied.hpfSteep)
                    filtered = hpf2.processSample (ch, filtered);
                b.data[ch][n] = dry + blend * (filtered - dry);
            }
        }
    }

    // The riser follows the high-passed signal's level (smoothed over ~0.4 s)
    // so it stays in step. Returns the level at the start of the block, for
    // the riser to ramp from.
    float followSignalLevel (const Block& b) noexcept
    {
        const float startRms = riser.signalRms;

        double sum = 0.0;
        for (int ch = 0; ch < b.channels; ++ch)
            for (int n = 0; n < b.numSamples; ++n)
                sum += (double) b.data[ch][n] * b.data[ch][n];

        const auto blockMeanSquare = (float) (sum / (double) (b.numSamples * b.channels));
        const auto coeff = (float) (1.0 - std::exp (-(double) b.numSamples / (riserFollowSeconds * sr)));
        riser.meanSquare += coeff * (blockMeanSquare - riser.meanSquare);
        riser.signalRms   = std::sqrt (riser.meanSquare);
        return startRms;
    }

    // Keeps the high-passed signal aside for any external effects (stage 3b):
    // the delay stage writes over it.
    void keepExternalSends (const juce::AudioBuffer<float>& buffer, int channels, int numSamples) noexcept
    {
        for (int slot = 0; slot < 2; ++slot)
            if ((slot == 0 ? external.reverb : external.delay) != nullptr)
                for (int ch = 0; ch < juce::jmin (channels, externalChannels); ++ch)
                    buffers.external.copyFrom (slot * externalChannels + ch, 0, buffer, ch, 0, numSamples);
    }

    // The built-in reverb, wet only, into buffers.reverb (summed in later).
    void runReverb (const juce::AudioBuffer<float>& buffer, bool on, int reverbChannels)
    {
        const int numSamples   = buffer.getNumSamples();
        const float reverbSize  = sm.reverbSize.skip (numSamples);
        const float reverbLevel = sm.reverbWet.skip (numSamples);
        if (! on)
            return;

        applyReverbParameters (reverbSize, reverbLevel);

        for (int ch = 0; ch < reverbChannels; ++ch)
            buffers.reverb.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        // PRE-DELAY: the send reaches the room that much later.
        for (int n = 0; n < numSamples; ++n)
        {
            if (sm.preDelay.isSmoothing())
                preDelay.line.setDelay (sm.preDelay.getNextValue());
            for (int ch = 0; ch < reverbChannels; ++ch)
            {
                auto* send = buffers.reverb.getWritePointer (ch);
                preDelay.line.pushSample (ch, send[n]);
                send[n] = preDelay.line.popSample (ch);
            }
        }

        juce::dsp::AudioBlock<float> block (buffers.reverb);
        auto subBlock = block.getSubsetChannelBlock (0, (size_t) reverbChannels)
                             .getSubBlock (0, (size_t) numSamples);
        juce::dsp::ProcessContextReplacing ctx (subBlock);
        reverb.process (ctx);
    }

    // The built-in delay, in place: the signal (none when wet only) plus echoes.
    void runDelay (const Block& b, bool on) noexcept
    {
        if (! on)
        {
            sm.delayWet.skip (b.numSamples);
            sm.delayFb.skip (b.numSamples);
            delayLine.setDelay (sm.delayTime.skip (b.numSamples));
            if (wetOnly)
                for (int ch = 0; ch < b.channels; ++ch)
                    juce::FloatVectorOperations::clear (b.data[ch], b.numSamples);
            return;
        }

        // PING-PONG (stereo only): both sides go in on the left and the
        // repeats cross over. Each side then gets every other echo, half the
        // power, so they're raised 3 dB to sit at the same level.
        const bool pingPong = settings.isOn (EffectSettings::delayPingPong) && b.channels >= 2;
        const float sideGain = pingPong ? juce::MathConstants<float>::sqrt2 : 1.0f;

        for (int n = 0; n < b.numSamples; ++n)
        {
            const float wet = sm.delayWet.getNextValue() * sideGain * tone.gain;
            const float fb  = sm.delayFb.getNextValue();
            if (sm.delayTime.isSmoothing())
                delayLine.setDelay (sm.delayTime.getNextValue());

            // The repeats, through TONE's low-pass.
            std::array<float, 2> delayed {};
            for (int ch = 0; ch < b.channels; ++ch)
            {
                float& state = tone.state[(size_t) juce::jmin (ch, 1)];
                state += tone.coeff * (delayLine.popSample (ch) - state);
                if (ch < 2)
                    delayed[(size_t) ch] = state;
                else   // beyond stereo: as usual
                {
                    delayLine.pushSample (ch, b.data[ch][n] + state * fb);
                    b.data[ch][n] = (wetOnly ? 0.0f : b.data[ch][n]) + state * wet;
                }
            }

            if (pingPong)
            {
                delayLine.pushSample (0, 0.5f * (b.data[0][n] + b.data[1][n]) + delayed[1] * fb);
                delayLine.pushSample (1, delayed[0] * fb);
            }
            else
            {
                for (int ch = 0; ch < juce::jmin (b.channels, 2); ++ch)
                    delayLine.pushSample (ch, b.data[ch][n] + delayed[(size_t) ch] * fb);
            }

            for (int ch = 0; ch < juce::jmin (b.channels, 2); ++ch)
                b.data[ch][n] = (wetOnly ? 0.0f : b.data[ch][n]) + delayed[(size_t) ch] * wet;
        }
    }

    // Sums the riser in: the user's sample if one is loaded, else the noise
    // sweep, at the riser level times the followed signal level (ramped across
    // the block from where it started).
    void addRiser (const Block& b, float startRms) noexcept
    {
        const RiserSample* sample = riserSample.current();
        const bool useSample = sample != nullptr && sample->length() > 0;

        // A sample that follows the project plays faster or slower by the
        // project's tempo over its own.
        const double rate = useSample && sample->tempo > 0.0 && transport.bpm > 0.0 ? transport.bpm / sample->tempo : 1.0;

        const float rmsStep = (riser.signalRms - startRms) / (float) b.numSamples;
        float signalRms = startRms;
        for (int n = 0; n < b.numSamples; ++n)
        {
            const float level  = sm.riserLevel.getNextValue();
            const float cutoff = sm.riserCutoff.getNextValue();   // kept in step for a switch back to noise
            signalRms += rmsStep;

            if (useSample)
                addSampleRiser (b, n, *sample, rate, level, signalRms);
            else
                addNoiseRiser (b, n, cutoff, level, signalRms);
        }

        const bool playing = useSample && ! riser.armed && riser.wait == 0;
        riserPlayhead = playing ? (float) (riser.samplePos / sample->length()) : -1.0f;
    }

    void addSampleRiser (const Block& b, int n, const RiserSample& sample, double rate, float level, float signalRms) noexcept
    {
        if (level <= 0.0f)
        {
            // Fully down (Amount or riser depth at 0): hold at the top, so the
            // next build-up starts on the sample's first sample.
            riser.samplePos = 0.0;
            riser.armed     = true;
            return;
        }

        if (riser.armed)
        {
            // The build-up starts: a sample that follows the project starts on
            // the host's next bar line; one at its own speed starts now.
            const auto start = sample.tempo > 0.0 ? nextBarStart (n) : BarStart {};
            riser.armed     = false;
            riser.wait      = start.wait;
            riser.samplePos = std::fmod (start.pastSeconds * sr * rate, (double) sample.length());
        }
        if (riser.wait > 0)
        {
            --riser.wait;
            return;
        }

        // Loop the sample at unit RMS, scaled exactly like the noise.
        const float gain = sample.unitGain * level * signalRms;
        const int last = sample.audio.getNumChannels() - 1;
        for (int ch = 0; ch < b.channels; ++ch)
            b.data[ch][n] += readLooped (sample.audio.getReadPointer (juce::jmin (ch, last)), sample.length(), riser.samplePos) * gain;

        riser.samplePos += rate;
        if (riser.samplePos >= (double) sample.length())
            riser.samplePos -= (double) sample.length();
    }

    // Where the riser starts, from sample `n` of this block: on the host's next
    // bar line, as `wait` samples from now. Just past a bar line (within a
    // sixteenth) it starts now, `pastSeconds` into the sample, so it is still in
    // step. Without a playing transport it starts now, from the top.
    struct BarStart
    {
        int    wait        = 0;
        double pastSeconds = 0.0;
    };
    BarStart nextBarStart (int n) const noexcept
    {
        const auto& t = transport;
        if (! t.playing || t.bpm <= 0.0 || t.barLength <= 0.0)
            return {};

        const double quarterNotesPerSample = t.bpm / (60.0 * sr);
        double intoBar = std::fmod (t.ppq + n * quarterNotesPerSample - t.barStart, t.barLength);
        if (intoBar < 0.0)
            intoBar += t.barLength;

        constexpr double lateStart = 0.25;   // a sixteenth note, in quarter notes
        if (intoBar <= lateStart)
            return { 0, intoBar * 60.0 / t.bpm };
        return { (int) std::ceil ((t.barLength - intoBar) / quarterNotesPerSample), 0.0 };
    }

    // The sample at a fractional position in a loop of `length` (4-point
    // Catmull-Rom, wrapping around the loop). Exact at whole positions.
    static float readLooped (const float* x, int length, double pos) noexcept
    {
        const int   i = (int) pos;
        const auto  f = (float) (pos - i);
        const auto at = [x, length] (int k) { return x[(k % length + length) % length]; };
        const float y0 = x[i];
        if (f <= 0.0f)
            return y0;

        const float ym1 = at (i - 1);
        const float y1  = at (i + 1);
        const float y2  = at (i + 2);
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }

    // Adopts a newly handed-over riser sample (the replaced one goes back to
    // the message thread to free). The same sample re-rendered (for a new
    // tempo, say) carries on from the same point in its loop; a different one
    // starts from the top.
    void adoptRiserSample() noexcept
    {
        const RiserSample* before = riserSample.current();
        const int    beforeId     = before != nullptr ? before->sourceId : -1;
        const double beforeLength = before != nullptr ? (double) before->length() : 0.0;

        bool adopted = false;
        const RiserSample* now = riserSample.acquire (&adopted);
        if (! adopted || now == nullptr)
            return;

        if (now->sourceId == beforeId && beforeLength > 0.0 && now->length() > 0)
            riser.samplePos = std::fmod (riser.samplePos / beforeLength * now->length(), (double) now->length());
        else
            riser.samplePos = 0.0;
    }

    void addNoiseRiser (const Block& b, int n, float cutoff, float level, float signalRms) noexcept
    {
        // Same coefficient caching as the HPF: skip the per-sample std::tan
        // when the band-pass centre is parked. The gain that brings the
        // band-passed noise to unit RMS is recomputed alongside it.
        if (! juce::exactlyEqual (cutoff, riser.lastCutoff))
        {
            riser.filter.setCutoffFrequency (cutoff);
            riser.unitGain   = unitRmsGain (cutoff) * riser.qGain;
            riser.lastCutoff = cutoff;
        }

        const float noise    = random.nextFloat() * 2.0f - 1.0f;
        const float filtered = riser.filter.processSample (0, noise) * riser.unitGain * level * signalRms;

        for (int ch = 0; ch < b.channels; ++ch)
            b.data[ch][n] += filtered;
    }

    // Global dry/wet mix and output trim, per sample.
    void mixAndTrim (const Block& b) noexcept
    {
        auto* const* dry = buffers.dry.getArrayOfReadPointers();
        for (int n = 0; n < b.numSamples; ++n)
        {
            const float wetDry = sm.mix.getNextValue();
            const float gain   = sm.trim.getNextValue();

            for (int ch = 0; ch < b.channels; ++ch)
                b.data[ch][n] = (dry[ch][n] * (1.0f - wetDry) + b.data[ch][n] * wetDry) * gain;
        }
    }
    static constexpr float minHpfCutoffHz = 20.0f;   // where the sweep parks: the HPF's "off"

    // Whether an effect runs this block. One that has settled at "off" is
    // skipped, and reset the first time, so it later re-engages from silence.
    template <typename Reset>
    static bool runOrIdle (bool& isIdle, bool settledOff, Reset&& resetEffect)
    {
        if (! settledOff)
        {
            isIdle = false;
            return true;
        }
        if (! isIdle)
        {
            resetEffect();
            isIdle = true;
        }
        return false;
    }

    // Runs one external effect on its kept send (slot 0 reverb, 1 delay) and
    // sums its wet output in at the smoothed level. A removed effect can't be
    // called again, so its level drops straight to zero.
    template <typename Smoother>
    void runExternal (ExternalEffect* effect, Smoother& level, int slot,
                      float* const* data, int channels, int numSamples) noexcept
    {
        if (effect == nullptr)
        {
            level.setCurrentAndTargetValue (0.0f);
            return;
        }

        const int fxChannels = juce::jmin (channels, externalChannels);
        std::array<float*, externalChannels> send {};
        for (int ch = 0; ch < fxChannels; ++ch)
            send[(size_t) ch] = buffers.external.getWritePointer (slot * externalChannels + ch);

        effect->process (send.data(), fxChannels, numSamples);

        for (int n = 0; n < numSamples; ++n)
        {
            const float gain = level.getNextValue();
            for (int ch = 0; ch < channels; ++ch)
                data[ch][n] += gain * send[(size_t) juce::jmin (ch, fxChannels - 1)][n];
        }
    }

    void applyReverbParameters (float roomSize, float wetLevel)
    {
        juce::dsp::Reverb::Parameters params;
        params.roomSize   = roomSize;
        params.wetLevel   = wetLevel;
        params.dryLevel   = 0.0f;   // wet only: the signal is summed back in process()
        params.damping    = settings[EffectSettings::reverbDamping];
        params.width      = settings[EffectSettings::reverbWidth];

        // Narrower, the reverb's two sides (which don't line up) are mixed
        // into each other and lose up to 3 dB: put it back.
        const float w = params.width;
        params.wetLevel *= std::sqrt (2.0f / (1.0f + w * w));
        params.freezeMode = 0.0f;
        reverb.setParameters (params);
    }

    static constexpr double maxPreDelaySeconds = 0.25;
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
    bool  wetOnly = false;

    EffectSettings settings;

    juce::dsp::StateVariableTPTFilter<float> hpf;
    juce::dsp::StateVariableTPTFilter<float> hpf2;   // the second stage of the 24 dB slope
    juce::dsp::Reverb                        reverb;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine;
    juce::Random                             random;

    // The reverb's pre-delay line.
    struct PreDelay
    {
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> line;
    };
    PreDelay preDelay;

    // The delay's TONE low-pass (one pole per side, applied to the repeats).
    struct Tone
    {
        float coeff = 1.0f;
        float gain  = 1.0f;
        std::array<float, 2> state {};
    };
    Tone tone;

    // The settings last pushed to the filters (pushing recomputes coefficients).
    struct Applied
    {
        bool  hpfSteep = false;
        float hpfQ     = -1.0f;
        float riserQ   = -1.0f;
    };
    Applied applied;

    // The input kept for the dry/wet mix, the built-in reverb's send, and the
    // external effects' sends (two channels each: reverb, then delay).
    static constexpr int externalChannels = 2;
    struct Buffers
    {
        juce::AudioBuffer<float> dry;
        juce::AudioBuffer<float> reverb;
        juce::AudioBuffer<float> external;
    };
    Buffers buffers;

    struct ExternalEffects
    {
        ExternalEffect* reverb = nullptr;
        ExternalEffect* delay  = nullptr;
    };
    ExternalEffects external;

    // Last cutoff pushed to the HPF, so process() can skip recomputing an
    // identical coefficient (an std::tan) every sample. -1 forces the first apply.
    float lastHpfCutoff = -1.0f;

    // Effects currently skipped because they have settled at "off".
    struct Idle
    {
        bool hpf    = false;
        bool reverb = false;
        bool delay  = false;
        bool riser  = false;
    };
    Idle idle;

    // The riser: the noise band-pass, its coefficient cache and unit-RMS gain
    // for the current centre, the level follower that ties it to the signal,
    // and the playback position in a user sample.
    struct Riser
    {
        juce::dsp::StateVariableTPTFilter<float> filter;
        float lastCutoff = -1.0f;
        float unitGain   = 1.0f;
        float qGain      = 1.0f;   // the unit gain's correction for the band-pass's Q
        float meanSquare = 0.0f;
        float signalRms  = 0.0f;
        double samplePos = 0.0;    // in the sample's loop, fractional when playing at another tempo
        bool   armed     = true;   // down: the next build-up waits for a bar line
        int    wait      = 0;      // samples still to wait for it
    };
    Riser riser;
    Transport transport;
    std::atomic<float> riserPlayhead { -1.0f };

    // User riser sample, handed from the message thread to the audio thread.
    RtHandoff<RiserSample> riserSample;

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
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> hpfBlend;   // dry 0 .. filtered 1
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> extReverb;  // external effects' levels
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> extDelay;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> delayTime;  // in samples
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> preDelay;   // in samples
    };
    Smoothers sm;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropEngine)
};
