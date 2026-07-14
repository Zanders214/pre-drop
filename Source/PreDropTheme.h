#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>

/**
    Shared visual language for the "Direction D — Neon Clean" Pre-Drop editor:
    the colour palette, font helpers, small colour-mixing utilities, and the
    numeric value formatters. Kept in one place so the LookAndFeel, the chips,
    and the curve all speak the same tokens (see design_handoff_predrop_ui).
*/
namespace PreDrop
{
    namespace Palette
    {
        // Backgrounds / surfaces
        const juce::Colour panelTop    { 0xff1a2030 };
        const juce::Colour panelBottom { 0xff0a0b12 };
        const juce::Colour knobBodyHi  { 0xff222731 };
        const juce::Colour knobBodyLo  { 0xff0b0d12 };
        const juce::Colour chipSurface = juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.05f);
        const juce::Colour sliderTrack = juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.10f);
        const juce::Colour ringTrack   = juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.08f);
        const juce::Colour hairline    = juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.06f);
        const juce::Colour knobBorder  = juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.08f);
        const juce::Colour idleBar     = juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.08f);

        // Text
        const juce::Colour textStrong  { 0xffe8ecf3 };
        const juce::Colour textMid     { 0xffcfd4dc };
        const juce::Colour textDim     { 0xff8a93a3 };
        const juce::Colour textDimmer  { 0xff7e8794 };
        const juce::Colour textFaint   { 0xff5d6473 };
        const juce::Colour percentSign { 0xff9aa3b3 };

        // Effect / accent colours
        const juce::Colour cyan   { 0xff34d8ff };  // HPF
        const juce::Colour violet { 0xff8b7bff };  // Reverb
        const juce::Colour pink   { 0xffff5fa8 };  // Delay
        const juce::Colour amber  { 0xffffc24b };  // Riser
        const juce::Colour badge  { 0xff5e93ff };  // BUILD-UP pill
        const juce::Colour arcGlow = juce::Colour::fromFloatRGBA (1.0f, 0.51f, 0.353f, 0.35f); // rgba(255,130,90,0.35)
    }

    /**
        Font helpers. The handoff calls for Space Grotesk (UI) + JetBrains Mono
        (numbers). We use them when the host machine has them installed and fall
        back to the platform's default sans / monospaced face otherwise — the
        spec explicitly permits "the closest equivalents already available".
        Bundling the TTFs as BinaryData later is a localized swap inside these
        two functions.
    */
    namespace Fonts
    {
        inline juce::Font sans (float height, bool bold = false, float letterSpacingEm = 0.0f)
        {
            static const bool hasSpaceGrotesk = juce::Font::findAllTypefaceNames().contains ("Space Grotesk");

            auto options = juce::FontOptions().withHeight (height);
            if (hasSpaceGrotesk)
                options = options.withName ("Space Grotesk");
            if (bold)
                options = options.withStyle ("Bold");

            juce::Font f (options);
            if (std::abs (letterSpacingEm) > 1.0e-6f)
                f.setExtraKerningFactor (letterSpacingEm);
            return f;
        }

        inline juce::Font mono (float height, float letterSpacingEm = 0.0f)
        {
            static const juce::String name = juce::Font::findAllTypefaceNames().contains ("JetBrains Mono")
                                               ? juce::String ("JetBrains Mono")
                                               : juce::Font::getDefaultMonospacedFontName();

            juce::Font f (juce::FontOptions().withName (name).withHeight (height));
            if (std::abs (letterSpacingEm) > 1.0e-6f)
                f.setExtraKerningFactor (letterSpacingEm);
            return f;
        }
    }

    namespace ColourMath
    {
        inline juce::Colour lerp (juce::Colour a, juce::Colour b, float t) noexcept
        {
            t = juce::jlimit (0.0f, 1.0f, t);
            return juce::Colour::fromFloatRGBA (a.getFloatRed()   + (b.getFloatRed()   - a.getFloatRed())   * t,
                                                a.getFloatGreen() + (b.getFloatGreen() - a.getFloatGreen()) * t,
                                                a.getFloatBlue()  + (b.getFloatBlue()  - a.getFloatBlue())  * t,
                                                a.getFloatAlpha() + (b.getFloatAlpha() - a.getFloatAlpha()) * t);
        }

        /** The four-stop ring/value-arc gradient sampled at f in 0..1
            (cyan -> violet -> pink -> amber, evenly spaced). */
        inline juce::Colour arc (float f) noexcept
        {
            using namespace Palette;
            if (f < 1.0f / 3.0f) return lerp (cyan,   violet, f * 3.0f);
            if (f < 2.0f / 3.0f) return lerp (violet, pink,  (f - 1.0f / 3.0f) * 3.0f);
            return                       lerp (pink,   amber, (f - 2.0f / 3.0f) * 3.0f);
        }

        /** The build-up histogram spectrum, keyed to bar position x in 0..1
            (cyan->violet to 0.4, violet->pink to 0.7, pink->amber to 1.0). */
        inline juce::Colour spectrum (float x) noexcept
        {
            using namespace Palette;
            if (x < 0.4f) return lerp (cyan,   violet, x / 0.4f);
            if (x < 0.7f) return lerp (violet, pink,  (x - 0.4f) / 0.3f);
            return               lerp (pink,   amber, (x - 0.7f) / 0.3f);
        }
    }

    namespace Format
    {
        /** Cutoff: ">= 1 kHz" as "X.XX kHz", otherwise "NNN Hz". */
        inline juce::String hertz (float hz)
        {
            if (hz >= 1000.0f)
                return juce::String (hz / 1000.0f, 2) + " kHz";
            return juce::String (juce::roundToInt (hz)) + " Hz";
        }

        /** Progress 0..1 as an integer percentage, e.g. "73%". */
        inline juce::String percent (float x)
        {
            return juce::String (juce::roundToInt (x * 100.0f)) + "%";
        }
    }
}
