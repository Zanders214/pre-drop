/*
    PreDropEngine unit tests.

    Headless and framework-free: plain check() asserts plus an int main() that
    returns non-zero on failure, so it runs on any platform without a display and
    plugs straight into CTest. The macro mapping is the riskiest logic, so it gets
    the most coverage, backed by a couple of whole-engine smoke tests.
*/

#include "PreDropEngine.h"
#include "PreDropVisualModel.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
int failures = 0;

void check (bool condition, const std::string& message)
{
    std::printf ("  [%s] %s\n", condition ? "PASS" : "FAIL", message.c_str());
    if (! condition)
        ++failures;
}

bool approx (float a, float b, float tolerance) { return std::abs (a - b) <= tolerance; }

juce::AudioBuffer<float> makeSine (double sampleRate, double freq, int numSamples, int numChannels = 2)
{
    juce::AudioBuffer<float> buffer (numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sampleRate));
    return buffer;
}

float rms (const juce::AudioBuffer<float>& buffer, int channel, int start, int end)
{
    double sum = 0.0;
    for (int i = start; i < end; ++i)
        sum += (double) buffer.getSample (channel, i) * buffer.getSample (channel, i);
    return (float) std::sqrt (sum / juce::jmax (1, end - start));
}

// ---- Test 1: the macro mapping engages effects in the expected order. ---------
void testMappingBoundaries()
{
    std::printf ("Mapping boundaries:\n");

    const float eps = 1.0e-9f;
    const auto at0 = PreDropEngine::mapMacro (0.0f);
    check (approx (at0.hpfCutoffHz, 20.0f, 0.5f), "at amount=0 HPF cutoff sits at ~20 Hz");
    check (at0.reverbWet  < eps, "at amount=0 reverb is silent");
    check (at0.delayWet   < eps, "at amount=0 delay is silent");
    check (at0.riserLevel < eps, "at amount=0 riser is silent");

    const auto at05 = PreDropEngine::mapMacro (0.5f);
    check (at05.hpfCutoffHz > 20.0f, "at amount=0.5 the HPF has opened up");
    check (at05.reverbWet  > 0.0f,   "at amount=0.5 reverb is engaging (window opens at 20%)");
    check (at05.delayWet   > 0.0f,   "at amount=0.5 delay is engaging (window opens at 40%)");
    check (at05.riserLevel < eps,    "at amount=0.5 riser is still silent (window opens at 60%)");

    const auto at1 = PreDropEngine::mapMacro (1.0f);
    check (approx (at1.hpfCutoffHz, 800.0f, 1.0f),          "at amount=1 HPF cutoff reaches ~800 Hz");
    check (at1.reverbWet > 0.0f && at1.riserLevel > 0.0f,   "at amount=1 every effect is fully engaged");
    check (at1.delayFeedback <= 0.95f,                      "delay feedback stays clamped <= 0.95 for stability");

    // Monotonic build-up: more knob never means less of any effect.
    bool monotonic = true;
    PreDropEngine::Targets prev = PreDropEngine::mapMacro (0.0f);
    for (int i = 1; i <= 100; ++i)
    {
        const auto cur = PreDropEngine::mapMacro ((float) i / 100.0f);
        monotonic = monotonic
                 && cur.hpfCutoffHz   >= prev.hpfCutoffHz   - 1.0e-3f
                 && cur.reverbWet     >= prev.reverbWet     - 1.0e-6f
                 && cur.delayWet      >= prev.delayWet      - 1.0e-6f
                 && cur.riserLevel    >= prev.riserLevel    - 1.0e-6f;
        prev = cur;
    }
    check (monotonic, "every effect target rises monotonically with the macro");
}

// ---- Test 1b: per-effect depth scales each effect's maximum independently. ----
void testDepthScaling()
{
    std::printf ("Per-effect depth scaling:\n");

    const float eps = 1.0e-6f;

    // Default depths reproduce the original fixed mapping exactly.
    const auto full = PreDropEngine::mapMacro (1.0f);
    const auto fullExplicit = PreDropEngine::mapMacro (1.0f, PreDropEngine::Depths {});
    check (approx (full.reverbWet, fullExplicit.reverbWet, eps)
        && approx (full.hpfCutoffHz, fullExplicit.hpfCutoffHz, eps),
        "default Depths{} matches the original mapMacro(amount)");

    // Depth 0 silences just that effect; the others are untouched.
    PreDropEngine::Depths noReverb; noReverb.reverb = 0.0f;
    const auto rv0 = PreDropEngine::mapMacro (1.0f, noReverb);
    check (rv0.reverbWet < eps,                        "reverb depth 0 silences reverb at full amount");
    check (approx (rv0.delayWet, full.delayWet, eps),  "reverb depth 0 leaves delay untouched");
    check (approx (rv0.riserLevel, full.riserLevel, eps), "reverb depth 0 leaves riser untouched");

    PreDropEngine::Depths noDelay; noDelay.delay = 0.0f;
    check (PreDropEngine::mapMacro (1.0f, noDelay).delayWet < eps, "delay depth 0 silences delay");

    PreDropEngine::Depths noRiser; noRiser.riser = 0.0f;
    check (PreDropEngine::mapMacro (1.0f, noRiser).riserLevel < eps, "riser depth 0 silences riser");

    // Depth 0.5 halves the effect's maximum.
    PreDropEngine::Depths halfReverb; halfReverb.reverb = 0.5f;
    check (approx (PreDropEngine::mapMacro (1.0f, halfReverb).reverbWet, full.reverbWet * 0.5f, eps),
           "reverb depth 0.5 halves the reverb wet maximum");

    // HPF depth scales the top of the sweep: 0 -> stays at 20 Hz, 1 -> ~800 Hz.
    PreDropEngine::Depths noHpf; noHpf.hpf = 0.0f;
    check (approx (PreDropEngine::mapMacro (1.0f, noHpf).hpfCutoffHz, 20.0f, 0.01f),
           "HPF depth 0 keeps the cutoff at 20 Hz (low-cut effectively off)");
    check (approx (full.hpfCutoffHz, 800.0f, 1.0f),
           "HPF depth 1 (default) still reaches ~800 Hz");
}

// ---- Test 2: with the macro at zero, the effect is near-transparent. ----------
void testNearPassthroughAtZero()
{
    std::printf ("Near-passthrough at amount=0:\n");

    constexpr double sr = 48000.0;
    const int n = 8192;

    PreDropEngine engine;
    engine.prepare (sr, 2, n);
    engine.setAmount (0.0f);
    engine.setMix (1.0f);
    engine.setOutputTrimDb (0.0f);

    const auto in = makeSine (sr, 440.0, n);
    auto buffer = in;
    engine.process (buffer);

    // Compare the settled tail (skip filter warm-up) by RMS.
    const int start = n / 2;
    const float inRms  = rms (in,     0, start, n);
    const float outRms = rms (buffer, 0, start, n);

    check (approx (outRms, inRms, 0.03f), "at amount=0 output level matches input (transparent)");
}

// ---- Test 3: at full tilt the output stays finite and bounded. ----------------
void testStabilityAtFull()
{
    std::printf ("Stability at amount=1:\n");

    constexpr double sr = 48000.0;
    const int block = 512;
    const int blocks = 200; // ~2.1 s of sustained input

    PreDropEngine engine;
    engine.prepare (sr, 2, block);
    engine.setAmount (1.0f);
    engine.setMix (1.0f);
    engine.setOutputTrimDb (0.0f);

    bool finite = true;
    float peak = 0.0f;

    for (int b = 0; b < blocks; ++b)
    {
        auto buffer = makeSine (sr, 110.0, block);
        engine.process (buffer);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < block; ++i)
            {
                const float s = buffer.getSample (ch, i);
                if (! std::isfinite (s))
                    finite = false;
                peak = juce::jmax (peak, std::abs (s));
            }
    }

    check (finite,      "output stays finite (no NaN/Inf) under sustained input at full");
    check (peak < 8.0f, "output stays bounded (feedback does not run away)");
}

// ---- Test 4: the editor's visual model stays in lock-step with the DSP. --------
void testVisualModelMatchesEngine()
{
    std::printf ("Visual model <-> engine coupling:\n");

    bool cutoffMatches = true;
    for (int i = 0; i <= 100; ++i)
    {
        const float a = (float) i / 100.0f;
        cutoffMatches = cutoffMatches
                     && approx (PreDropVisualModel::cutoffHz (a), PreDropEngine::mapMacro (a).hpfCutoffHz, 0.5f);
    }
    check (cutoffMatches, "editor cutoff readout matches the engine HPF cutoff across the macro");

    // The chips' engaged flags must line up with the engine's effects switching on.
    check (PreDropEngine::mapMacro (0.19f).reverbWet  <= 0.0f && ! PreDropVisualModel::reverbEngaged (0.19f), "reverb idle below 20% in both UI and DSP");
    check (PreDropEngine::mapMacro (0.21f).reverbWet  >  0.0f &&   PreDropVisualModel::reverbEngaged (0.21f), "reverb engaged above 20% in both UI and DSP");
    check (PreDropEngine::mapMacro (0.39f).delayWet   <= 0.0f && ! PreDropVisualModel::delayEngaged  (0.39f), "delay idle below 40% in both UI and DSP");
    check (PreDropEngine::mapMacro (0.41f).delayWet   >  0.0f &&   PreDropVisualModel::delayEngaged  (0.41f), "delay engaged above 40% in both UI and DSP");
    check (PreDropEngine::mapMacro (0.59f).riserLevel <= 0.0f && ! PreDropVisualModel::riserEngaged  (0.59f), "riser idle below 60% in both UI and DSP");
    check (PreDropEngine::mapMacro (0.61f).riserLevel >  0.0f &&   PreDropVisualModel::riserEngaged  (0.61f), "riser engaged above 60% in both UI and DSP");
}
} // namespace

int main()
{
    std::printf ("PreDropEngine tests\n===================\n");

    testMappingBoundaries();
    testDepthScaling();
    testNearPassthroughAtZero();
    testStabilityAtFull();
    testVisualModelMatchesEngine();

    std::printf ("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
