#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

/**
    RiserSample

    A user-supplied riser, prepared off the audio thread so the engine only ever
    reads it: resampled to the engine's sample rate, given a short crossfade at
    the loop point so it repeats without a click, and measured so that playing
    it at `unitGain` gives unit RMS. That last step lets a sample slot into the
    same level balance as the built-in noise riser (PreDropEngine scales both by
    the riser level and the signal's loudness).

    Immutable once created. An empty one (no samples) means "use the built-in
    noise riser"; PreDropEngine::setRiserSample takes either.
*/
struct RiserSample
{
    juce::AudioBuffer<float> audio;     // at the engine's sample rate, 1 or 2 channels
    float unitGain = 1.0f;              // 1 / RMS of `audio`
    juce::String name;

    int length() const noexcept { return audio.getNumSamples(); }

    /** The "use the built-in noise" marker. */
    static std::unique_ptr<RiserSample> none() { return std::make_unique<RiserSample>(); }

    /** Builds a riser from decoded audio. Returns nullptr when there is nothing
        to play (empty, or silent so it cannot be normalised). Allocates: call
        on the message thread or from prepareToPlay, never on the audio thread. */
    static std::unique_ptr<RiserSample> create (const juce::AudioBuffer<float>& source, double sourceRate,
                                                double targetRate, const juce::String& name)
    {
        const int channels = juce::jmin (2, source.getNumChannels());
        if (channels <= 0 || source.getNumSamples() <= 0 || sourceRate <= 0.0 || targetRate <= 0.0)
            return nullptr;

        auto result = std::make_unique<RiserSample>();
        result->name = name;

        const auto resampled = resample (source, channels, sourceRate / targetRate);
        result->audio = bakeLoopCrossfade (resampled, (int) (loopCrossfadeSeconds * targetRate));

        double sum = 0.0;
        for (int ch = 0; ch < channels; ++ch)
            for (int n = 0; n < result->length(); ++n)
                sum += (double) result->audio.getSample (ch, n) * result->audio.getSample (ch, n);

        const double rms = std::sqrt (sum / ((double) channels * (double) result->length()));
        if (rms < 1.0e-6)
            return nullptr;

        result->unitGain = (float) (1.0 / rms);
        return result;
    }

private:
    static constexpr double loopCrossfadeSeconds = 0.02;

    // Windowed-sinc resampling of the first `channels` channels. `ratio` is
    // source rate / target rate; 1 copies straight through.
    static juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& source, int channels, double ratio)
    {
        const int inLength = source.getNumSamples();
        if (std::abs (ratio - 1.0) < 1.0e-9)
        {
            juce::AudioBuffer<float> copy (channels, inLength);
            for (int ch = 0; ch < channels; ++ch)
                copy.copyFrom (ch, 0, source, ch, 0, inLength);
            return copy;
        }

        // The interpolator delays its output by its base latency (in input
        // samples), so run it over zero-padded input for that much longer and
        // drop the leading outputs.
        const auto latency  = (int) std::ceil (juce::WindowedSincInterpolator::getBaseLatency());
        const int  outLength = juce::jmax (1, (int) ((double) inLength / ratio));
        const int  skip      = (int) std::lround ((double) latency / ratio);

        juce::AudioBuffer<float> padded (1, inLength + 2 * latency + 8);
        juce::AudioBuffer<float> out (channels, outLength);
        std::vector<float> scratch ((size_t) (outLength + skip));

        for (int ch = 0; ch < channels; ++ch)
        {
            padded.clear();
            padded.copyFrom (0, 0, source, ch, 0, inLength);

            juce::WindowedSincInterpolator interpolator;
            interpolator.process (ratio, padded.getReadPointer (0), scratch.data(), outLength + skip);
            out.copyFrom (ch, 0, scratch.data() + skip, outLength);
        }
        return out;
    }

    // Folds the head of the sample into its tail with an equal-power crossfade
    // of `fade` samples, so playback can wrap from the end straight back to the
    // start with no click. The result is `fade` samples shorter and starts
    // `fade` samples in. Samples too short to spare it are returned as-is.
    static juce::AudioBuffer<float> bakeLoopCrossfade (const juce::AudioBuffer<float>& in, int fade)
    {
        const int length = in.getNumSamples();
        fade = juce::jmin (fade, length / 4);
        if (fade < 2)
            return in;

        const int loopLength = length - fade;
        juce::AudioBuffer<float> out (in.getNumChannels(), loopLength);

        for (int ch = 0; ch < in.getNumChannels(); ++ch)
        {
            const float* x = in.getReadPointer (ch);
            float* y = out.getWritePointer (ch);

            for (int i = 0; i < loopLength; ++i)
                y[i] = x[i + fade];

            for (int k = 0; k < fade; ++k)
            {
                const float w = (float) (k + 1) / (float) (fade + 1);
                const float fadeIn  = std::sin (w * juce::MathConstants<float>::halfPi);
                const float fadeOut = std::cos (w * juce::MathConstants<float>::halfPi);
                const int i = loopLength - fade + k;
                y[i] = x[i + fade] * fadeOut + x[k] * fadeIn;
            }
        }
        return out;
    }
};
