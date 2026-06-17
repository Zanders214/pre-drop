/*
    PreDropVisualModel unit tests.

    Framework-free (no JUCE, no display): the editor's numeric model is pure, so
    we can pin its math directly against the design reference values from
    design_handoff_predrop_ui (renderVals / energyAt). The engine-coupling check
    — that the displayed cutoff and thresholds match PreDropEngine::mapMacro — is
    intentionally JUCE-side and lives in PreDropEngineTests.cpp.
*/

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

bool approx (float a, float b, float tol) { return std::fabs (a - b) <= tol; }

// The engine's HPF sweep, replicated so we can assert the model reproduces it
// without dragging in JUCE here: 20 * (800/20)^(amount^1.5).
float engineCutoff (float a)
{
    const float h = std::pow (std::min (1.0f, std::max (0.0f, a)), 1.5f);
    return 20.0f * std::pow (800.0f / 20.0f, h);
}

void testProgress()
{
    std::printf ("Per-effect progress + thresholds:\n");
    using M = PreDropVisualModel;

    check (approx (M::hpfProgress (0.0f), 0.0f, 1e-6f), "HPF progress is 0 at amount=0");
    check (approx (M::hpfProgress (1.0f), 1.0f, 1e-6f), "HPF progress is 1 at amount=1");

    check (approx (M::reverbProgress (0.20f), 0.0f, 1e-6f), "reverb progress is 0 exactly at the 20% threshold");
    check (M::reverbProgress (0.21f) > 0.0f,                "reverb progress lifts just past 20%");
    check (approx (M::delayProgress (0.40f), 0.0f, 1e-6f),  "delay progress is 0 exactly at the 40% threshold");
    check (M::delayProgress (0.41f) > 0.0f,                 "delay progress lifts just past 40%");
    check (approx (M::riserProgress (0.60f), 0.0f, 1e-6f),  "riser progress is 0 exactly at the 60% threshold");
    check (M::riserProgress (0.61f) > 0.0f,                 "riser progress lifts just past 60%");

    check (approx (M::reverbProgress (0.5f), 0.375f,   1e-4f), "reverb progress at 0.5 matches (0.3/0.8)");
    check (approx (M::delayProgress  (0.5f), 0.16667f, 1e-4f), "delay progress at 0.5 matches (0.1/0.6)");

    check (! M::reverbEngaged (0.19f) &&  M::reverbEngaged (0.20f), "reverb engages at 20%");
    check (! M::delayEngaged  (0.39f) &&  M::delayEngaged  (0.40f), "delay engages at 40%");
    check (! M::riserEngaged  (0.59f) &&  M::riserEngaged  (0.60f), "riser engages at 60%");
}

void testCutoff()
{
    std::printf ("HPF cutoff sweep:\n");
    using M = PreDropVisualModel;

    check (approx (M::cutoffHz (0.0f),  20.0f, 0.01f), "cutoff is 20 Hz at amount=0");
    check (approx (M::cutoffHz (1.0f), 800.0f, 0.5f),  "cutoff reaches 800 Hz at amount=1");

    bool matchesEngine = true;
    for (int i = 0; i <= 100; ++i)
    {
        const float a = (float) i / 100.0f;
        matchesEngine = matchesEngine && approx (M::cutoffHz (a), engineCutoff (a), 0.5f);
    }
    check (matchesEngine, "model cutoff tracks the engine's 20->800 Hz log sweep at every amount");
}

void testEnergyCurve()
{
    std::printf ("Build-up energy curve:\n");
    using M = PreDropVisualModel;

    check (approx (M::energyAt (0.0f), 0.0f, 1e-6f), "energy is 0 at x=0");
    check (approx (M::energyAt (1.0f), 1.0f, 1e-6f), "energy saturates to 1 at x=1");
    check (approx (M::energyAt (0.5f), 0.19321f, 1e-4f), "energy at x=0.5 matches the reference (~0.193)");

    bool monotonic = true;
    float prev = M::energyAt (0.0f);
    for (int i = 1; i <= 200; ++i)
    {
        const float cur = M::energyAt ((float) i / 200.0f);
        monotonic = monotonic && (cur >= prev - 1e-6f);
        prev = cur;
    }
    check (monotonic, "energy rises monotonically across the curve");
}
} // namespace

int main()
{
    std::printf ("PreDropVisualModel tests\n========================\n");

    testProgress();
    testCutoff();
    testEnergyCurve();

    std::printf ("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
