// RealtimeSanitizer driver for PreDrop.
//
// Exercises the real audio callback under clang's -fsanitize=realtime so any
// allocation / lock / syscall on the audio thread becomes a hard failure. PreDrop
// is an audio effect (no MIDI, no juce::Synthesiser), so we drive it with a signal
// and an empty MidiBuffer, holding the "amount" macro at full so every effect stage
// in the chain is engaged (worst case).

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include <cmath>
#include <cstdio>

int main()
{
    juce::ScopedJuceInitialiser_GUI init;

    PreDropAudioProcessor proc;
    const double sr = 48000.0;
    const int    bs = 512;
    proc.prepareToPlay (sr, bs);

    // A second instance linked as a TRACK, so the linked path (reading the
    // main's published controls, applying the track's effects) runs too.
    PreDropAudioProcessor track;
    track.setRole (PreDropAudioProcessor::Role::track);
    track.prepareToPlay (sr, bs);
    juce::AudioBuffer<float> trackBuffer (2, bs);

    // And one as a RETURN: the wet-only path a send / return track takes.
    PreDropAudioProcessor fxReturn;
    fxReturn.setRole (PreDropAudioProcessor::Role::returnTrack);
    fxReturn.prepareToPlay (sr, bs);
    juce::AudioBuffer<float> returnBuffer (2, bs);

    // Drive the build-up macro to 100% so HPF + reverb + delay + riser all run.
    *proc.getState().getRawParameterValue ("amount") = 1.0f;

    juce::AudioBuffer<float> buffer (2, bs);
    const int   blocks = (int) (sr / bs) * 2;          // ~2 s of callbacks
    const float freq   = 220.0f;
    double      phase  = 0.0;
    const double inc   = 2.0 * juce::MathConstants<double>::pi * freq / sr;

    // Halfway through, swap the noise riser for a user sample, so the audio
    // thread's lock-free hand-over and the sample playback run under RTSan too.
    // Loading happens here, between callbacks, exactly as the editor does it.
    const auto riserFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rt_check_riser.wav");
    {
        juce::AudioBuffer<float> riser (2, (int) sr);
        juce::Random random { 1 };
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < riser.getNumSamples(); ++n)
                riser.setSample (ch, n, 0.2f * (random.nextFloat() * 2.0f - 1.0f));

        riserFile.deleteFile();
        juce::WavAudioFormat wav;
        if (auto out = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (
                new juce::FileOutputStream (riserFile), sr, 2, 16, {}, 0)))
            out->writeFromAudioSampleBuffer (riser, 0, riser.getNumSamples());
    }

    for (int b = 0; b < blocks; ++b)
    {
        if (b == blocks / 2)
        {
            proc.setAppliesToSelectedTracks (true);   // hand HPF / reverb / delay to the track
            const auto error = proc.loadRiserSample (riserFile);
            if (error.isNotEmpty())
            {
                std::printf ("FAIL: could not load the riser sample: %s\n", error.toRawUTF8());
                return 1;
            }
        }

        // Fill with a sine so the DSP actually has signal to chew on.
        for (int n = 0; n < bs; ++n)
        {
            const float s = (float) std::sin (phase);
            phase += inc;
            if (phase > 2.0 * juce::MathConstants<double>::pi)
                phase -= 2.0 * juce::MathConstants<double>::pi;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, n, s * 0.25f);
        }

        juce::MidiBuffer midi;                          // effect: stays empty
        trackBuffer.makeCopyOf (buffer, true);
        returnBuffer.makeCopyOf (buffer, true);
        proc.processBlock (buffer, midi);               // exercises the annotated DSP
        track.processBlock (trackBuffer, midi);         // the linked-track path
        fxReturn.processBlock (returnBuffer, midi);     // the wet-only return path
    }

    riserFile.deleteFile();
    std::printf ("PASS: %d blocks on a main, a linked track and a return (noise riser, then a sample riser\n"
                 "      with the track and return applying the effects), no RT-safety violations\n", blocks);
    return 0;
}
