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

    // Drive the build-up macro to 100% so HPF + reverb + delay + riser all run.
    *proc.apvts.getRawParameterValue ("amount") = 1.0f;

    juce::AudioBuffer<float> buffer (2, bs);
    const int   blocks = (int) (sr / bs) * 2;          // ~2 s of callbacks
    const float freq   = 220.0f;
    double      phase  = 0.0;
    const double inc   = 2.0 * juce::MathConstants<double>::pi * freq / sr;

    for (int b = 0; b < blocks; ++b)
    {
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
        proc.processBlock (buffer, midi);               // exercises the annotated DSP
    }

    std::printf ("PASS: %d processBlock calls, no RT-safety violations\n", blocks);
    return 0;
}
