#pragma once

#include <algorithm>
#include <cmath>

/**
    PreDropVisualModel

    Pure, framework-free numeric model behind the Pre-Drop editor's "Neon Clean"
    UI. It turns the Amount macro (0..1) and the per-effect depths into what the
    effect chips show, the displayed high-pass cutoff, and the decorative
    "build-up energy" curve used by the histogram + tension readout.

    It intentionally mirrors PreDropEngine::mapMacro so the on-screen indicators
    never drift from the DSP: every effect starts with the first percent of
    Amount, and the HPF sweep (20 Hz -> 800 Hz at the default depth of 0.5, up to
    3.2 kHz at depth 1) is the engine's. The energy curve (energyAt) is purely
    cosmetic: it only shapes the histogram and is not part of the audio path.

    Kept header-only and dependency-free (just <cmath>/<algorithm>) so it can be
    unit-tested on any platform without JUCE or a display.
*/
struct PreDropVisualModel
{
    static float clamp01 (float x) noexcept { return std::min (1.0f, std::max (0.0f, x)); }

    /** How far one effect has gone, 0..1: Amount times that effect's depth knob,
        as shown on its chip. All four effects share the same build-up. */
    static float intensity (float amount, float depth) noexcept { return clamp01 (amount) * clamp01 (depth); }

    /** An effect is engaged (chip lit) as soon as Amount and its depth are above 0. */
    static bool engaged (float amount, float depth) noexcept { return amount > 0.0f && depth > 0.0f; }

    /** Octaves above 20 Hz the HPF reaches at full Amount. Mirrors
        PreDropEngine::hpfTopOctaves: off at depth 0, 800 Hz at 0.5, 3.2 kHz at 1. */
    static float hpfTopOctaves (float hpfDepth) noexcept
    {
        const float d = clamp01 (hpfDepth);
        constexpr float nominal = 5.321928f;   // log2 (800 / 20)
        constexpr float maximum = 7.321928f;   // log2 (3200 / 20)
        return d <= 0.5f ? nominal * (d / 0.5f)
                         : nominal + (maximum - nominal) * ((d - 0.5f) / 0.5f);
    }

    /** Displayed high-pass cutoff in Hz; equals mapMacro's hpfCutoffHz. */
    static float cutoffHz (float a, float hpfDepth = 0.5f) noexcept
    {
        return 20.0f * std::exp2 (hpfTopOctaves (hpfDepth) * clamp01 (a));
    }

    /** Composite build-up energy at position x in 0..1 (decorative histogram
        shape). Every effect rises with Amount on the same curve, so it does too. */
    static float energyAt (float x) noexcept { return clamp01 (x); }
};
