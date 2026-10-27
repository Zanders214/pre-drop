/*
    PreDropVisualModel unit tests.

    Framework-free (no JUCE, no display): the editor's numeric model is pure, so
    we can pin its math directly. The engine-coupling check (that the displayed
    cutoff and engagement match MacroMapping::mapMacro) is intentionally
    JUCE-side and lives in PreDropEngineTests.cpp.
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
// without dragging in JUCE here: 20 Hz * 2^(octaves(depth) * amount), with the
// top at 800 Hz for the default depth 0.5 and 3.2 kHz at depth 1.
float engineCutoff (float a, float depth)
{
    const float d = std::min (1.0f, std::max (0.0f, depth));
    const float top = d <= 0.5f ? std::log2 (40.0f) * (d / 0.5f)
                                : std::log2 (40.0f) + 2.0f * ((d - 0.5f) / 0.5f);
    return 20.0f * std::exp2 (top * std::min (1.0f, std::max (0.0f, a)));
}

void testIntensity()
{
    std::printf ("Per-effect intensity + engagement:\n");
    using M = PreDropVisualModel;

    check (approx (M::intensity (0.0f, 0.5f), 0.0f, 1e-6f),  "intensity is 0 at amount=0");
    check (approx (M::intensity (1.0f, 0.5f), 0.5f, 1e-6f),  "full Amount at the default depth reads 50%");
    check (approx (M::intensity (1.0f, 1.0f), 1.0f, 1e-6f),  "full Amount at depth 100% reads 100%");
    check (approx (M::intensity (0.3f, 0.5f), 0.15f, 1e-6f), "intensity is Amount x depth");
    check (approx (M::intensity (2.0f, 2.0f), 1.0f, 1e-6f),  "intensity is clamped to 0..1");

    check (! M::engaged (0.0f, 0.5f),  "nothing is engaged at amount=0");
    check (M::engaged (0.01f, 0.5f),   "every effect engages from the first percent of Amount");
    check (! M::engaged (1.0f, 0.0f),  "a depth of 0 keeps that effect idle");
}

void testCutoff()
{
    std::printf ("HPF cutoff sweep:\n");
    using M = PreDropVisualModel;

    check (approx (M::cutoffHz (0.0f),  20.0f, 0.01f), "cutoff is 20 Hz at amount=0");
    check (approx (M::cutoffHz (1.0f), 800.0f, 0.5f),  "cutoff reaches 800 Hz at amount=1 (default depth)");
    check (approx (M::cutoffHz (0.5f), std::sqrt (20.0f * 800.0f), 0.5f),
           "half Amount is halfway in pitch (geometric mean of 20 Hz and 800 Hz)");

    check (approx (M::cutoffHz (1.0f, 0.0f),   20.0f, 0.01f), "depth 0 keeps the cutoff at 20 Hz");
    check (approx (M::cutoffHz (1.0f, 1.0f), 3200.0f, 2.0f),  "depth 100% tops out at 3.2 kHz");
    check (M::cutoffHz (1.0f, 0.25f) < M::cutoffHz (1.0f, 0.5f), "lowering HPF depth lowers the displayed cutoff");

    bool matchesEngine = true;
    for (const float depth : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        for (int i = 0; i <= 100; ++i)
        {
            const float a = (float) i / 100.0f;
            matchesEngine = matchesEngine && approx (M::cutoffHz (a, depth), engineCutoff (a, depth), 0.05f);
        }
    check (matchesEngine, "model cutoff tracks the engine's sweep at every amount and depth");
}

void testEnergyCurve()
{
    std::printf ("Build-up energy curve:\n");
    using M = PreDropVisualModel;

    check (approx (M::energyAt (0.0f), 0.0f, 1e-6f), "energy is 0 at x=0");
    check (approx (M::energyAt (1.0f), 1.0f, 1e-6f), "energy saturates to 1 at x=1");
    check (approx (M::energyAt (0.5f), 0.5f, 1e-6f), "energy rises in step with Amount, like every effect");

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

    testIntensity();
    testCutoff();
    testEnergyCurve();

    std::printf ("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
