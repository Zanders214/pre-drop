#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

#include "DropRelease.h"
#include "EffectSettings.h"
#include "MacroMapping.h"
#include "RiserVoice.h"
#include "RtSafety.h"

namespace PreDropEngineDetail
{
// Whether an effect runs this block. One that has settled at "off" is
// skipped, and reset the first time, so it later re-engages from silence.
template <typename Reset>
inline bool runOrIdle (bool& isIdle, bool settledOff, Reset&& resetEffect)
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
} // namespace PreDropEngineDetail

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
    external effects (hosted plugins) via setExternalEffects. After the drop,
    DropRelease plays the downlifter: the riser falls and the reverb and delay
    ring out. The mapping from the macro to each effect's targets lives in
    MacroMapping, and the riser in RiserVoice.
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

    // The macro's mapping and calibration live in MacroMapping.h.
    using Targets   = MacroMapping::Targets;
    using Depths    = MacroMapping::Depths;
    using Transport = HostTransport;

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

        riser.prepare (spec);
        applied = {};
        applySettings();

        // Set the reverb's parameters before preparing it: prepare() snaps its
        // internal smoothers, so the wet-only send starts with no dry leaking in.
        const Targets t = MacroMapping::mapMacro (controls.amount, controls.depths, controls.settings);
        applyReverbParameters (t.reverbSize, t.reverbWet);
        reverb.prepare (spec);

        preDelay.line.setMaximumDelayInSamples ((int) std::ceil (maxPreDelaySeconds * sampleRate) + 2);
        preDelay.line.prepare (spec);

        delayLine.setMaximumDelayInSamples ((int) std::ceil (EffectSettings::maxDelaySeconds * sampleRate) + 2);
        delayLine.prepare (spec);

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
        sm.mix        .setCurrentAndTargetValue (controls.mix);
        sm.trim       .setCurrentAndTargetValue (juce::Decibels::decibelsToGain (controls.outputTrimDb));
        sm.hpfBlend   .setCurrentAndTargetValue (t.hpfCutoffHz > MacroMapping::minHpfCutoffHz ? 1.0f : 0.0f);
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
        riser.reset();
        reverb.reset();
        preDelay.line.reset();
        delayLine.reset();
        tone = {};

        // Re-prime the coefficient cache so the next block re-applies the cutoff.
        lastHpfCutoff = -1.0f;
        idle          = {};
        release.reset();
    }

    /** Hands the engine a riser sample built for its current sample rate, or
        RiserSample::none() to go back to the built-in noise. Call from the
        message thread: process() adopts it at the start of a block without
        allocating or locking, and returns the replaced one for
        collectRetiredRiserSamples() to free. */
    void setRiserSample (std::unique_ptr<RiserSample> sample) { riser.setSample (std::move (sample)); }

    /** Frees riser samples the audio thread has swapped out. Message thread. */
    void collectRetiredRiserSamples() { riser.collectRetired(); }

    /** The host's transport for this block. Audio thread, before process(). */
    void setTransport (const Transport& now) noexcept { transport = now; }

    /** Where a riser sample is in its loop (0..1), or -1 while it isn't
        playing (no sample, the riser is down, or it waits for a bar line).
        Any thread. */
    float getRiserPlayhead() const noexcept { return riser.getPlayhead(); }

    /** The external reverb and delay for this block (nullptr: the built-in
        one). Audio thread, before process(). Switching crossfades over 40 ms,
        except that a removed effect stops at once (it can't be called again). */
    void setExternalEffects (ExternalEffect* reverbEffect, ExternalEffect* delayEffect) noexcept
    {
        external.reverb = reverbEffect;
        external.delay  = delayEffect;
    }

    void setAmount       (float newAmount) noexcept { controls.amount       = newAmount; }
    void setMix          (float newMix)    noexcept { controls.mix          = newMix; }
    void setOutputTrimDb (float db)        noexcept { controls.outputTrimDb = db; }

    /** How the built-in effects sound (see EffectSettings). Audio thread,
        before process(); a plain copy. */
    void setEffectSettings (const EffectSettings& newSettings) noexcept { controls.settings = newSettings; }

    void setReverbDepth (float d) noexcept { controls.depths.reverb = d; }
    void setDelayDepth  (float d) noexcept { controls.depths.delay  = d; }
    void setRiserDepth  (float d) noexcept { controls.depths.riser  = d; }
    void setHpfDepth    (float d) noexcept { controls.depths.hpf    = d; }

    /** Wet only: output just the effects (echoes, reverb, riser), not the
        signal they ride on. For a return track, where the dry signal already
        reaches the mix through each track and must not be doubled. */
    void setWetOnly (bool shouldBeWetOnly) noexcept { controls.wetOnly = shouldBeWetOnly; }

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

        riser.adoptSample();

        // Keep a clean copy of the input for the final dry/wet mix.
        for (int ch = 0; ch < channels; ++ch)
            buffers.dry.copyFrom (ch, 0, buffer, ch, 0, numSamples);

        applySettings();
        release.update (controls.amount, controls.depths, controls.settings, transport, sr, numSamples);
        riser.setDownlifting (release.isActive());
        setTargets();
        const Engaged on = engageEffects();
        const Block block { buffer.getArrayOfWritePointers(), channels, numSamples };

        // --- 1. High-pass (per sample so the rising cutoff stays zipper-free) ----
        if (on.hpf)
            processHighPass (block);

        // The high-passed signal is what reverb, delay and riser all ride on.
        const float riserRmsStart = riser.followSignalLevel (block);
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
            riser.add (block, riserRmsStart, sm.riserLevel, sm.riserCutoff, transport);
        }
        else
        {
            sm.riserLevel.skip (numSamples);
            sm.riserCutoff.skip (numSamples);
            riser.stop();
        }

        // --- 5. Global dry/wet mix + output trim (per sample) -------------------
        mixAndTrim (block);
    }

private:
    using Block = SampleBlock;

    // Which effects run this block.
    struct Engaged
    {
        bool hpf;
        bool reverb;
        bool delay;
        bool riser;
    };

    // Points every smoother at what Amount and the depths ask for. An external
    // reverb / delay takes over from the built-in one: the built-in ramps out
    // while the external one ramps in at the same level.
    void setTargets() noexcept
    {
        Targets t = MacroMapping::mapMacro (controls.amount, controls.depths, controls.settings);
        release.shape (t);
        sm.hpfCutoff  .setTargetValue (t.hpfCutoffHz);
        sm.reverbWet  .setTargetValue (external.reverb != nullptr ? 0.0f : t.reverbWet);
        sm.reverbSize .setTargetValue (t.reverbSize);
        sm.delayWet   .setTargetValue (external.delay != nullptr ? 0.0f : t.delayWet);
        sm.extReverb  .setTargetValue (external.reverb != nullptr ? t.reverbLevel : 0.0f);
        sm.extDelay   .setTargetValue (external.delay  != nullptr ? t.delayLevel  : 0.0f);
        sm.delayFb    .setTargetValue (t.delayFeedback);
        sm.riserLevel .setTargetValue (t.riserLevel);
        sm.riserCutoff.setTargetValue (std::min (t.riserCutoffHz, 0.45f * (float) sr));   // clear of Nyquist
        sm.mix        .setTargetValue (controls.mix);
        sm.trim       .setTargetValue (juce::Decibels::decibelsToGain (controls.outputTrimDb));
        sm.delayTime  .setTargetValue (delayTimeSamples());
        sm.preDelay   .setTargetValue (preDelaySamples());
    }

    // The settings that aren't swept by Amount: pushed to the filters only
    // when they change (each push recomputes coefficients).
    void applySettings() noexcept
    {
        const auto& settings = controls.settings;
        const bool  steep    = settings.hpfSteep();
        if (const float q = settings.hpfQ(); steep != applied.hpfSteep || ! juce::exactlyEqual (q, applied.hpfQ))
        {
            // 24 dB: two stages with a Butterworth pair of Qs. The resonance
            // lifts only the second (from 1.31 to the 12 dB slope's top Q), so
            // the peak is no taller than at 12 dB; lifting both would stack
            // their peaks (+19 dB on noise at full resonance).
            const float top = EffectSettings::specs[(size_t) EffectSettings::Id::hpfResonance].max;
            const float res = settings[EffectSettings::Id::hpfResonance] / top;
            constexpr float steepQ = 1.3065630f;
            const float maxQ = 0.70710678f * 12.0f;   // EffectSettings::hpfQ() at full resonance
            hpf.setResonance  (steep ? 0.5411961f : q);
            hpf2.setResonance (steep ? steepQ * std::pow (maxQ / steepQ, res) : q);
            applied.hpfSteep = steep;
            applied.hpfQ     = q;
        }

        riser.setResonance (settings.riserQ());

        // A one-pole low-pass in the delay's repeats (none when TONE is open),
        // and the gain that keeps the darker repeats at the delay's level.
        const float toneHz = settings[EffectSettings::Id::delayTone];
        const bool  open   = toneHz >= EffectSettings::toneOpenHz - 1.0f;
        tone.coeff = open ? 1.0f : 1.0f - std::exp (-juce::MathConstants<float>::twoPi * toneHz / (float) sr);
        tone.gain  = open ? 1.0f : MacroMapping::toneGain (toneHz, sm.delayFb.getTargetValue());
    }

    float delayTimeSamples() const noexcept
    {
        return (float) juce::jlimit (1.0, EffectSettings::maxDelaySeconds * sr, controls.settings.delaySeconds (transport.bpm) * sr);
    }

    float preDelaySamples() const noexcept
    {
        return (float) juce::jlimit (0.0, maxPreDelaySeconds * sr, controls.settings[EffectSettings::Id::reverbPreDelay] / 1000.0 * sr);
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
        const bool hpfWanted = ! settledAtOff (sm.hpfCutoff, MacroMapping::minHpfCutoffHz);
        sm.hpfBlend.setTargetValue (hpfWanted ? 1.0f : 0.0f);
        return {
            PreDropEngineDetail::runOrIdle (idle.hpf,    ! hpfWanted && settledAtOff (sm.hpfBlend, 0.0f), [this] { hpf.reset(); hpf2.reset(); }),
            PreDropEngineDetail::runOrIdle (idle.reverb, settledAtOff (sm.reverbWet, 0.0f),           [this] { reverb.reset(); preDelay.line.reset(); }),
            PreDropEngineDetail::runOrIdle (idle.delay,  settledAtOff (sm.delayWet, 0.0f),            [this] { delayLine.reset(); tone.state = {}; }),
            PreDropEngineDetail::runOrIdle (idle.riser,  settledAtOff (sm.riserLevel, 0.0f),          [this] { riser.park(); })
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

    // Keeps the high-passed signal aside for any external effects (stage 3b):
    // the delay stage writes over it.
    void keepExternalSends (const juce::AudioBuffer<float>& buffer, int channels, int numSamples) noexcept
    {
        for (int slot = 0; slot < 2; ++slot)
        {
            if ((slot == 0 ? external.reverb : external.delay) == nullptr)
                continue;
            for (int ch = 0; ch < juce::jmin (channels, externalChannels); ++ch)
            {
                if (release.isActive())   // the downlifter: only the tail rings on
                    buffers.external.clear (slot * externalChannels + ch, 0, numSamples);
                else
                    buffers.external.copyFrom (slot * externalChannels + ch, 0, buffer, ch, 0, numSamples);
            }
        }
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
        {
            if (release.isActive())   // the downlifter: only the tail rings on
                buffers.reverb.clear (ch, 0, numSamples);
            else
                buffers.reverb.copyFrom (ch, 0, buffer, ch, 0, numSamples);
        }

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
            if (controls.wetOnly)
                for (int ch = 0; ch < b.channels; ++ch)
                    juce::FloatVectorOperations::clear (b.data[ch], b.numSamples);
            return;
        }

        // PING-PONG (stereo only): both sides go in on the left and the
        // repeats cross over. Each side then gets every other echo, half the
        // power, so they're raised 3 dB to sit at the same level.
        const bool pingPong = controls.settings.isOn (EffectSettings::Id::delayPingPong) && b.channels >= 2;
        const float sideGain = pingPong ? juce::MathConstants<float>::sqrt2 : 1.0f;
        const float send = release.isActive() ? 0.0f : 1.0f;   // the downlifter: only the repeats ring on

        for (int n = 0; n < b.numSamples; ++n)
        {
            const float wet = sm.delayWet.getNextValue() * sideGain * tone.gain;
            const float fb  = sm.delayFb.getNextValue();
            if (sm.delayTime.isSmoothing())
                delayLine.setDelay (sm.delayTime.getNextValue());

            echo (b, n, { wet, fb, send }, pingPong);
        }
    }

    // The delay's gains for a sample: the echoes' level, the feedback, and
    // how much of the signal goes in.
    struct EchoGains
    {
        float wet;
        float fb;
        float send;
    };

    // One sample of the delay: the repeats, through TONE's low-pass, fed back
    // (crossed over in ping-pong) and summed in at `wet`.
    void echo (const Block& b, int n, EchoGains g, bool pingPong) noexcept
    {
        const float wet = g.wet;
        const float fb  = g.fb;
        const float signal = controls.wetOnly ? 0.0f : 1.0f;
        std::array<float, 2> delayed {};
        for (int ch = 0; ch < b.channels; ++ch)
        {
            float& state = tone.state[(size_t) juce::jmin (ch, 1)];
            state += tone.coeff * (delayLine.popSample (ch) - state);
            if (ch < 2)
            {
                delayed[(size_t) ch] = state;
            }
            else   // beyond stereo: as usual
            {
                delayLine.pushSample (ch, g.send * b.data[ch][n] + state * fb);
                b.data[ch][n] = signal * b.data[ch][n] + state * wet;
            }
        }

        if (pingPong)
        {
            delayLine.pushSample (0, 0.5f * g.send * (b.data[0][n] + b.data[1][n]) + delayed[1] * fb);
            delayLine.pushSample (1, delayed[0] * fb);
        }
        else
        {
            for (int ch = 0; ch < juce::jmin (b.channels, 2); ++ch)
                delayLine.pushSample (ch, g.send * b.data[ch][n] + delayed[(size_t) ch] * fb);
        }

        for (int ch = 0; ch < juce::jmin (b.channels, 2); ++ch)
            b.data[ch][n] = signal * b.data[ch][n] + delayed[(size_t) ch] * wet;
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
        params.damping    = controls.settings[EffectSettings::Id::reverbDamping];
        params.width      = controls.settings[EffectSettings::Id::reverbWidth];

        // Narrower, the reverb's two sides (which don't line up) are mixed
        // into each other and lose up to 3 dB: put it back.
        const float w = params.width;
        params.wetLevel *= std::sqrt (2.0f / (1.0f + w * w));
        params.freezeMode = 0.0f;
        reverb.setParameters (params);
    }

    static constexpr double maxPreDelaySeconds = 0.25;

    double sr    = 44100.0;
    int    numCh = 2;

    // What the setters set.
    struct Controls
    {
        float          amount       = 0.0f;
        float          mix          = 1.0f;
        float          outputTrimDb = 0.0f;
        Depths         depths;
        bool           wetOnly      = false;
        EffectSettings settings;
    };
    Controls controls;

    juce::dsp::StateVariableTPTFilter<float> hpf;
    juce::dsp::StateVariableTPTFilter<float> hpf2;   // the second stage of the 24 dB slope
    juce::dsp::Reverb                        reverb;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine;

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

    RiserVoice    riser;
    HostTransport transport;
    DropRelease   release;

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
