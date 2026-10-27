#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

#include "EffectSettings.h"

/**
    MacroMapping: how PreDrop's one Amount knob and the per-effect depths turn
    into each effect's targets (mapMacro, a pure function and so the part most
    worth unit-testing), and the gains that keep every effect at its
    calibrated level whatever its settings. PreDropEngine runs on these; the
    editor's indicators (PreDropVisualModel) read the same numbers.
*/
struct MacroMapping
{
    static constexpr float minHpfCutoffHz = 20.0f;   // where the sweep parks: the HPF's "off"

    /** Per-effect targets derived from the macro, already in engineering units. */
    struct Targets
    {
        float hpfCutoffHz   = 20.0f;   // high-pass cutoff
        float reverbWet     = 0.0f;    // juce::dsp::Reverb wetLevel (wet-only send)
        float reverbSize    = 0.3f;    // 0..1 room size
        float delayWet      = 0.0f;    // echo gain
        float delayFeedback = 0.0f;    // 0..FEEDBACK (at most 0.9), below 1 for stability
        float riserLevel    = 0.0f;    // riser RMS as a fraction of the signal's RMS
        float reverbLevel   = 0.0f;    // reverb / delay wet level as a fraction of the
        float delayLevel    = 0.0f;    // signal (what an external effect is scaled to)
        float riserCutoffHz = 200.0f;  // band-pass centre of the riser
    };

    /** Per-effect depth (0..1): how strong each effect gets at full Amount.

        0.5 is the balanced default (every effect at the same level), 0 switches
        the effect off and 1 doubles it. Amount still owns the build-up itself;
        depth only scales how far each effect goes. */
    struct Depths
    {
        float hpf    = 0.5f;   // top of the HPF sweep: 20 Hz (off) .. 800 Hz .. 3.2 kHz
        float reverb = 0.5f;
        float delay  = 0.5f;
        float riser  = 0.5f;
    };

    /** Wet level of reverb, delay and riser relative to the signal, at full
        Amount and the default depth of 0.5 (-6 dB). */
    static constexpr float nominalWetRatio = 0.5f;

    /** Delay feedback at full Amount by default (FEEDBACK); see mapMacro for
        why it stops here. */
    static constexpr float maxDelayFeedback = 0.6f;

    /** Room size at the start of the reverb's sweep, as a share of SIZE (the
        default 95% grows from 0.30). */
    static constexpr float reverbStartShare = 0.30f / 0.95f;

    /** Gain an effect's depth applies to its nominal level: 0 -> off,
        0.5 -> 1x (balanced), 1 -> 2x. */
    static float depthGain (float depth) noexcept { return 2.0f * juce::jlimit (0.0f, 1.0f, depth); }

    /** Octaves above 20 Hz the HPF reaches at full Amount: 0 at depth 0 (off),
        log2(800/20) at the default 0.5, two octaves more (3.2 kHz) at depth 1.
        Mirrored by PreDropVisualModel::hpfTopOctaves for the cutoff readout. */
    static float hpfTopOctaves (float depth) noexcept
    {
        const float d = juce::jlimit (0.0f, 1.0f, depth);
        constexpr float nominal = 5.321928f;   // log2 (800 / 20)
        constexpr float maximum = 7.321928f;   // log2 (3200 / 20)
        return d <= 0.5f ? nominal * (d / 0.5f)
                         : nominal + (maximum - nominal) * ((d - 0.5f) / 0.5f);
    }

    /** Wet-only gain of juce::dsp::Reverb (wetLevel 1, damping 0.4, width 1) for a
        room size, measured on pink noise at 44.1 / 48 kHz. Its combs ring longer
        as the room grows, so the tail gets louder; dividing by this keeps the
        reverb at the level mapMacro asks for whatever the size. */
    static float reverbWetGain (float roomSize) noexcept
    {
        const float g = roomSize * 0.28f + 0.7f;   // juce::Reverb's comb feedback
        return (1.456f - 0.254f * roomSize) / std::sqrt (1.0f - g * g);
    }

    /** Pure macro -> targets mapping with every effect at its default depth. */
    static Targets mapMacro (float amount) noexcept { return mapMacro (amount, Depths {}); }

    /** ... and with the effects' default settings. */
    static Targets mapMacro (float amount, Depths depth) noexcept { return mapMacro (amount, depth, EffectSettings {}); }

    /** Pure macro -> targets mapping. No state, so it is trivially unit-testable.

        Default arguments can't be used here: Depths is a nested type and its
        default member initialisers aren't available in this member's default
        argument, so the shorter overloads above delegate from their bodies. */
    static Targets mapMacro (float amount, Depths depth, const EffectSettings& settings) noexcept
    {
        const float p = juce::jlimit (0.0f, 1.0f, amount);
        Targets t;

        // 1. High-pass: sweeps up evenly in pitch (octaves) as Amount rises.
        t.hpfCutoffHz = minHpfCutoffHz * std::exp2 (hpfTopOctaves (depth.hpf) * p);

        // 2. Reverb: the room grows with Amount up to SIZE; the wet send is
        //    compensated for the louder tail so its level rises in step with
        //    the others.
        const float size = settings[EffectSettings::Id::reverbSize];
        t.reverbSize  = size * (reverbStartShare + (1.0f - reverbStartShare) * p);
        t.reverbLevel = nominalWetRatio * depthGain (depth.reverb) * p;
        t.reverbWet   = t.reverbLevel / reverbWetGain (t.reverbSize);

        // 3. Delay: feedback grows with Amount, and the echo gain is scaled by
        //    sqrt(1 - fb^2) because a feedback loop's steady-state energy is
        //    1 / (1 - fb^2) times one echo; this holds the delay level in step.
        //    That energy sum is exact for material whose echoes don't line up.
        //    Echoes that do (a tone or loop whose period divides the delay time)
        //    add in phase to 1 / (1 - fb), sqrt((1 + fb) / (1 - fb)) times more.
        //    The default FEEDBACK of 0.6 bounds that to 2x (+6 dB, ~5 s tail)
        //    rather than 3.5x (+11 dB) at 0.85.
        t.delayFeedback = settings[EffectSettings::Id::delayFeedback] * p;
        t.delayLevel    = nominalWetRatio * depthGain (depth.delay) * p;
        t.delayWet      = t.delayLevel * std::sqrt (1.0f - t.delayFeedback * t.delayFeedback);

        // 4. Riser: level relative to the signal, sweeping evenly in pitch from
        //    START to END (200 Hz -> 8 kHz by default).
        const float start = settings[EffectSettings::Id::riserStart];
        t.riserLevel    = nominalWetRatio * depthGain (depth.riser) * p;
        t.riserCutoffHz = start * std::pow (settings[EffectSettings::Id::riserEnd] / start, p);

        return t;
    }

    /** Gain that keeps the delay at its level through TONE's low-pass, which
        takes about half a dB per octave below 20 kHz out of pink noise, a
        little more as the repeats pile up (measured at 48 kHz, to within
        0.4 dB from 500 Hz to 16 kHz). */
    static float toneGain (float toneHz, float feedback) noexcept
    {
        const float octaves = std::log2 (EffectSettings::toneOpenHz / juce::jmax (1.0f, toneHz));
        return juce::Decibels::decibelsToGain ((0.1f + 0.5f * juce::jmax (0.0f, octaves)) * (1.0f + 0.25f * feedback));
    }

    /** Gain that keeps the noise riser at its level at band-pass Q `q`: its
        unit gain is measured at Q 1.5, and band-passed noise grows roughly as
        sqrt (Q), a little less as the band narrows (fitted on the sweep's top,
        to within 0.1 dB from Q 0.5 to 8). */
    static float riserQGain (float q) noexcept
    {
        const float x = std::log2 (q / 1.5f);
        return std::sqrt (1.5f / q) * juce::Decibels::decibelsToGain (-(0.72f * x - 0.15f * x * x));
    }
};
