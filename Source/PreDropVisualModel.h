#pragma once

#include <algorithm>
#include <cmath>

/**
    PreDropVisualModel

    Pure, framework-free numeric model behind the Pre-Drop editor's "Neon Clean"
    UI. It turns the single Amount macro (0..1) into the per-effect engagement
    progress, the displayed high-pass cutoff, and the decorative "build-up
    energy" curve used by the histogram + tension readout.

    The per-effect activation windows and the HPF cutoff sweep here intentionally
    mirror PreDropEngine::mapMacro so the on-screen indicators never drift from
    the DSP: the thresholds (0 / 20 / 40 / 60 %) and the 20 -> 800 Hz cutoff are
    the authoritative ones from the engine. The energy curve (energyAt) is purely
    cosmetic — it only shapes the histogram and is not part of the audio path.

    Kept header-only and dependency-free (just <cmath>/<algorithm>) so it can be
    unit-tested on any platform without JUCE or a display.
*/
struct PreDropVisualModel
{
    static float clamp01 (float x) noexcept { return std::min (1.0f, std::max (0.0f, x)); }

    /** Local 0..1 progress of a inside [lo, hi], clamped. Mirrors PreDropEngine::window. */
    static float window (float a, float lo, float hi) noexcept
    {
        return clamp01 ((a - lo) / (hi - lo));
    }

    // --- Per-effect engagement progress (0..1). Thresholds match the engine. ----
    static float hpfProgress    (float a) noexcept { return std::pow (clamp01 (a), 1.5f); }
    static float reverbProgress (float a) noexcept { return window (a, 0.20f, 1.0f); }
    static float delayProgress  (float a) noexcept { return window (a, 0.40f, 1.0f); }
    static float riserProgress  (float a) noexcept { const float s = window (a, 0.60f, 1.0f); return s * s; }

    /** Displayed high-pass cutoff in Hz: 20 -> (20..800), log-swept. Equals
        mapMacro's hpfCutoffHz. hpfDepth scales the top of the sweep (default 1
        -> 800 Hz) so the chip readout tracks the HPF depth control. */
    static float cutoffHz (float a, float hpfDepth = 1.0f) noexcept
    {
        const float topHz = 20.0f + (800.0f - 20.0f) * clamp01 (hpfDepth);
        return 20.0f * std::pow (topHz / 20.0f, hpfProgress (a));
    }

    // --- Engagement thresholds, for chip opacity (engaged vs idle). -------------
    static bool reverbEngaged (float a) noexcept { return a >= 0.20f; }
    static bool delayEngaged  (float a) noexcept { return a >= 0.40f; }
    static bool riserEngaged  (float a) noexcept { return a >= 0.60f; }

    /** Composite build-up energy at position x in 0..1 (decorative histogram shape). */
    static float energyAt (float x) noexcept
    {
        const float hp  = std::pow (clamp01 (x), 1.5f);
        const float rv  = clamp01 ((x - 0.20f) / 0.80f);
        const float dl  = clamp01 ((x - 0.40f) / 0.60f);
        const float rsW = clamp01 ((x - 0.60f) / 0.40f);
        const float rs  = rsW * rsW;
        return std::min (1.0f, hp * 0.20f + rv * 0.22f + dl * 0.24f + rs * 0.46f);
    }
};
