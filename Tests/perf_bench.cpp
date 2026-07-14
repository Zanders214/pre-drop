// nanobench micro-benchmark for PreDrop's audio callback.
//
// Measures the per-block cost of the full effect chain at 48 kHz / 512 and emits
// a github-action-benchmark "customSmallerIsBetter" JSON file so the perf workflow
// can chart the trend and alert on regressions. The headline metric is the
// "DSP load %" = ns_per_block / (blockSize / sampleRate); < 100% = real-time capable.

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include <cmath>
#include <cstdio>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const double sr = 48000.0;
    const int    bs = 512;

    PreDropAudioProcessor proc;
    proc.prepareToPlay (sr, bs);

    // Full build-up engaged so every effect stage contributes to the measurement.
    *proc.apvts.getRawParameterValue ("amount") = 1.0f;

    juce::AudioBuffer<float> buffer (2, bs);

    // A one-period sine table we copy in each block so input energy is constant.
    std::vector<float> sine ((size_t) bs);
    for (int n = 0; n < bs; ++n)
        sine[(size_t) n] = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                                     * 220.0 * n / sr);

    ankerl::nanobench::Bench bench;
    bench.title ("DSP @48k/512").unit ("block").warmup (20).minEpochIterations (200);
    bench.run ("processBlock", [&]
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom (ch, 0, sine.data(), bs);
        juce::MidiBuffer midi;
        proc.processBlock (buffer, midi);
        ankerl::nanobench::doNotOptimizeAway (buffer.getReadPointer (0)[0]);
    });

    // GOTCHA: the Measure enum is nested in Result, not the namespace.
    const double ns       = bench.results().back().median (ankerl::nanobench::Result::Measure::elapsed) * 1.0e9;
    const double budgetNs = (double) bs / sr * 1.0e9;          // 10.67 ms @48k/512
    const double loadPct  = ns / budgetNs * 100.0;

    // github-action-benchmark "customSmallerIsBetter" schema:
    juce::String json;
    json << "[\n"
         << "  { \"name\": \"processBlock\",      \"unit\": \"ns/block\", \"value\": " << juce::String (ns, 3)      << " },\n"
         << "  { \"name\": \"DSP load @48k/512\", \"unit\": \"%\",        \"value\": " << juce::String (loadPct, 3) << " }\n"
         << "]\n";
    const juce::String out = (argc > 1) ? juce::String (argv[1]) : juce::String ("bench_result.json");
    juce::File::getCurrentWorkingDirectory().getChildFile (out).replaceWithText (json);
    std::printf ("processBlock=%.0f ns (%.1f%% RT load)\n", ns, loadPct);
    return 0;
}
