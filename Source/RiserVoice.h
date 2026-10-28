#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <cmath>
#include <memory>

#include "MacroMapping.h"
#include "RiserSample.h"
#include "RtHandoff.h"

/** Where the host's transport is at the start of a block, for a riser
    sample that follows the project. Positions are in quarter notes. */
struct HostTransport
{
    bool   playing    = false;
    double bpm        = 0.0;    // 0: the host gives no tempo
    double ppq        = 0.0;    // position at the block's first sample
    double barStart   = 0.0;    // position of the bar that contains it
    double barLength  = 4.0;    // quarter notes per bar in the host's meter
};

/** The audio PreDropEngine works on: the buffer's first `channels` channels. */
struct SampleBlock
{
    float* const* data;
    int           channels;
    int           numSamples;
};

/**
    RiserVoice: PreDrop's riser, the part of PreDropEngine that sums a rising
    sound in on top of the signal. That is band-passed noise swept upward, or
    a looping user sample (handed over from the message thread without locks),
    at the riser level times the followed level of the signal it rides on.

    A sample that follows the project plays at the project's tempo and starts
    on the host's next bar line. Everything here but setSample and
    collectRetired runs on the audio thread.
*/
class RiserVoice
{
public:
    using LevelSmoother  = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;
    using CutoffSmoother = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        sr = spec.sampleRate;
        filter.prepare (spec);
        filter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
        appliedQ = -1.0f;

        // Not running concurrently with the audio thread, so a sample handed
        // over since the last block (typically one rebuilt for this sample
        // rate) can be adopted, and the one it replaces freed, right here.
        sample.adoptNow();
    }

    /** Back to silence: the filter, the level follower and the sample's place. */
    void reset() noexcept
    {
        park();
        lastCutoff = -1.0f;
        meanSquare = 0.0f;
        signalRms  = 0.0f;
        wait       = 0;
        playhead   = -1.0f;
    }

    /** Down: clears the filter, and the next build-up starts a sample from the top. */
    void park() noexcept
    {
        filter.reset();
        samplePos = 0.0;
        armed     = true;
    }

    /** The noise band-pass's Q (EffectSettings::riserQ); cheap when unchanged. */
    void setResonance (float q) noexcept
    {
        if (juce::exactlyEqual (q, appliedQ))
            return;
        filter.setResonance (q);
        qGain      = MacroMapping::riserQGain (q);
        lastCutoff = -1.0f;   // recompute the unit gain
        appliedQ   = q;
    }

    /** See PreDropEngine::setRiserSample. Message thread. */
    void setSample (std::unique_ptr<RiserSample> newSample) { sample.send (std::move (newSample)); }

    /** Frees samples the audio thread has swapped out. Message thread. */
    void collectRetired() { sample.collect(); }

    /** Where a sample is in its loop (0..1), or -1 while it isn't playing. Any thread. */
    float getPlayhead() const noexcept { return playhead.load(); }

    /** While the downlifter plays (see DropRelease), the voice is the noise,
        even with a sample loaded, and holds the build-up's level rather than
        following the signal (the drop's, by then). A sample goes back to its
        top, for the next build-up. */
    void setDownlifting (bool shouldDownlift) noexcept
    {
        if (shouldDownlift && ! downlifting)
        {
            samplePos = 0.0;
            armed     = true;
            wait      = 0;
        }
        downlifting = shouldDownlift;
    }

    /** Silent this block (the riser is down): no playhead to show. */
    void stop() noexcept { playhead = -1.0f; }

    // Adopts a newly handed-over riser sample (the replaced one goes back to
    // the message thread to free). The same sample re-rendered (for a new
    // tempo, say) carries on from the same point in its loop; a different one
    // starts from the top.
    void adoptSample() noexcept
    {
        const RiserSample* before = sample.current();
        const int    beforeId     = before != nullptr ? before->sourceId : -1;
        const double beforeLength = before != nullptr ? (double) before->length() : 0.0;

        bool adopted = false;
        const RiserSample* now = sample.acquire (&adopted);
        if (! adopted || now == nullptr)
            return;

        if (now->sourceId == beforeId && beforeLength > 0.0 && now->length() > 0)
            samplePos = std::fmod (samplePos / beforeLength * now->length(), (double) now->length());
        else
            samplePos = 0.0;
    }

    // The riser follows the high-passed signal's level (smoothed over ~0.4 s)
    // so it stays in step. Returns the level at the start of the block, for
    // the riser to ramp from.
    float followSignalLevel (const SampleBlock& b) noexcept
    {
        const float startRms = signalRms;
        if (downlifting)
            return startRms;

        double sum = 0.0;
        for (int ch = 0; ch < b.channels; ++ch)
            for (int n = 0; n < b.numSamples; ++n)
                sum += (double) b.data[ch][n] * b.data[ch][n];

        const auto blockMeanSquare = (float) (sum / (double) (b.numSamples * b.channels));
        const auto coeff = (float) (1.0 - std::exp (-(double) b.numSamples / (followSeconds * sr)));
        meanSquare += coeff * (blockMeanSquare - meanSquare);
        signalRms   = std::sqrt (meanSquare);
        return startRms;
    }

    // Sums the riser in: the user's sample if one is loaded, else the noise
    // sweep, at the riser level times the followed signal level (ramped across
    // the block from where it started).
    void add (const SampleBlock& b, float startRms, LevelSmoother& level, CutoffSmoother& cutoff,
              const HostTransport& transport) noexcept
    {
        const RiserSample* s = sample.current();
        const bool useSample = ! downlifting && s != nullptr && s->length() > 0;

        // A sample that follows the project plays faster or slower by the
        // project's tempo over its own.
        const double rate = useSample && s->tempo > 0.0 && transport.bpm > 0.0 ? transport.bpm / s->tempo : 1.0;

        const float rmsStep = (signalRms - startRms) / (float) b.numSamples;
        float rms = startRms;
        for (int n = 0; n < b.numSamples; ++n)
        {
            const float gain = level.getNextValue();
            const float fc   = cutoff.getNextValue();   // kept in step for a switch back to noise
            rms += rmsStep;

            if (useSample)
                addSample (b, n, { *s, rate, transport }, gain, rms);
            else
                addNoise (b, n, fc, gain * rms);
        }

        const bool playing = useSample && ! armed && wait == 0;
        playhead = playing ? (float) (samplePos / s->length()) : -1.0f;
    }

private:
    // What a sample plays from: the sample, its speed and the host's transport.
    struct Playback
    {
        const RiserSample&   sample;
        double               rate;
        const HostTransport& transport;
    };

    void addSample (const SampleBlock& b, int n, const Playback& p, float level, float rms) noexcept
    {
        if (level <= 0.0f)
        {
            // Fully down (Amount or riser depth at 0): hold at the top, so the
            // next build-up starts on the sample's first sample.
            samplePos = 0.0;
            armed     = true;
            return;
        }

        const int length = p.sample.length();
        if (armed)
        {
            // The build-up starts: a sample that follows the project starts on
            // the host's next bar line; one at its own speed starts now.
            const auto start = p.sample.tempo > 0.0 ? nextBarStart (p.transport, n) : BarStart {};
            armed     = false;
            wait      = start.wait;
            samplePos = std::fmod (start.pastSeconds * sr * p.rate, (double) length);
        }
        if (wait > 0)
        {
            --wait;
            return;
        }

        // Loop the sample at unit RMS, scaled exactly like the noise.
        const float g = p.sample.unitGain * level * rms;
        const int last = p.sample.audio.getNumChannels() - 1;
        for (int ch = 0; ch < b.channels; ++ch)
            b.data[ch][n] += readLooped (p.sample.audio.getReadPointer (juce::jmin (ch, last)), length, samplePos) * g;

        samplePos += p.rate;
        if (samplePos >= (double) length)
            samplePos -= (double) length;
    }

    void addNoise (const SampleBlock& b, int n, float cutoffHz, float gain) noexcept
    {
        // Same coefficient caching as the HPF: skip the per-sample std::tan
        // when the band-pass centre is parked. The gain that brings the
        // band-passed noise to unit RMS is recomputed alongside it.
        if (! juce::exactlyEqual (cutoffHz, lastCutoff))
        {
            filter.setCutoffFrequency (cutoffHz);
            unitGain   = unitRmsGain (cutoffHz) * qGain;
            lastCutoff = cutoffHz;
        }

        const float noise    = random.nextFloat() * 2.0f - 1.0f;
        const float filtered = filter.processSample (0, noise) * unitGain * gain;

        for (int ch = 0; ch < b.channels; ++ch)
            b.data[ch][n] += filtered;
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
    BarStart nextBarStart (const HostTransport& t, int n) const noexcept
    {
        if (! t.playing || t.bpm <= 0.0 || t.barLength <= 0.0)
            return {};

        const double quarterNotesPerSample = t.bpm / (60.0 * sr);
        double intoBar = std::fmod (t.ppq + n * quarterNotesPerSample - t.barStart, t.barLength);
        if (intoBar < 0.0)
            intoBar += t.barLength;

        if (constexpr double lateStart = 0.25; intoBar <= lateStart)   // a sixteenth note, in quarter notes
            return { 0, intoBar * 60.0 / t.bpm };
        return { (int) std::ceil ((t.barLength - intoBar) / quarterNotesPerSample), 0.0 };
    }

    // The sample at a fractional position in a loop of `length` (4-point
    // Catmull-Rom, wrapping around the loop). Exact at whole positions.
    static float readLooped (const float* x, int length, double pos) noexcept
    {
        const auto  i = (int) pos;
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

    static constexpr double followSeconds = 0.4;   // the level follower's time constant

    double sr = 44100.0;

    // The noise band-pass, its coefficient cache and unit-RMS gain for the
    // current centre.
    juce::dsp::StateVariableTPTFilter<float> filter;
    juce::Random random;
    float lastCutoff = -1.0f;
    float unitGain   = 1.0f;
    float qGain      = 1.0f;    // the unit gain's correction for the band-pass's Q
    float appliedQ   = -1.0f;   // the Q last pushed to the filter

    // The level follower that ties it to the signal.
    float meanSquare = 0.0f;
    float signalRms  = 0.0f;

    // The playback position in a user sample.
    double samplePos = 0.0;    // in the sample's loop, fractional when playing at another tempo
    bool   armed     = true;   // down: the next build-up waits for a bar line
    int    wait      = 0;      // samples still to wait for it
    std::atomic<float> playhead { -1.0f };
    bool   downlifting = false;

    // The user's sample, handed from the message thread to the audio thread.
    RtHandoff<RiserSample> sample;
};
