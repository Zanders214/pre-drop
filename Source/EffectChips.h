#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

#include "PreDropTheme.h"
#include "PreDropVisualModel.h"

/**
    The four effect status chips (HPF / REV / DLY / RIS).

    Each chip shows a coloured status dot whose glow tracks the effect's
    intensity, the effect abbreviation, and a live derived value (HPF cutoff;
    reverb / delay / riser progress as a percentage). A chip is fully opaque once
    its effect has engaged (Amount past 0 / 20 / 40 / 60 %) and dim before that.

    Read-only: it reflects the Amount macro via PreDropVisualModel and never
    writes a parameter, so the editor just feeds it the current Amount.
*/
class EffectChips : public juce::Component
{
public:
    EffectChips() { setInterceptsMouseClicks (false, false); }

    std::function<float()> getAmount;
    void setUiScale (float s) noexcept { uiScale = juce::jmax (0.1f, s); }

    void paint (juce::Graphics& g) override
    {
        const float a = getAmount ? getAmount() : 0.0f;
        const float s = uiScale;
        const auto  bounds = getLocalBounds().toFloat();

        using namespace PreDrop;

        struct Chip
        {
            juce::String  abbrev;
            juce::Colour  colour;
            float         glow;     // dot intensity 0..1
            bool          engaged;  // full vs dim
            juce::String  value;
        };

        const Chip chips[4] =
        {
            { "HPF", Palette::cyan,   juce::jmax (0.15f, PreDropVisualModel::hpfProgress (a)), true,
              Format::hertz (PreDropVisualModel::cutoffHz (a)) },
            { "REV", Palette::violet, PreDropVisualModel::reverbProgress (a), PreDropVisualModel::reverbEngaged (a),
              Format::percent (PreDropVisualModel::reverbProgress (a)) },
            { "DLY", Palette::pink,   PreDropVisualModel::delayProgress (a),  PreDropVisualModel::delayEngaged (a),
              Format::percent (PreDropVisualModel::delayProgress (a)) },
            { "RIS", Palette::amber,  PreDropVisualModel::riserProgress (a),  PreDropVisualModel::riserEngaged (a),
              Format::percent (PreDropVisualModel::riserProgress (a)) },
        };

        const int   n     = 4;
        const float gap   = 7.0f * s;
        const float chipW = (bounds.getWidth() - gap * (n - 1)) / (float) n;
        const float pad   = 9.0f * s;
        const float dotR  = 4.0f * s;

        const auto abbrevFont = Fonts::sans (10.0f * s, true);
        const auto valueFont  = Fonts::mono (8.5f * s);

        for (int i = 0; i < n; ++i)
        {
            const auto& chip = chips[i];
            const float op = chip.engaged ? 1.0f : 0.4f;

            const juce::Rectangle<float> rect (bounds.getX() + (float) i * (chipW + gap),
                                               bounds.getY(), chipW, bounds.getHeight());

            g.setColour (Palette::chipSurface.withMultipliedAlpha (op));
            g.fillRoundedRectangle (rect, 10.0f * s);

            const juce::Point<float> dot (rect.getX() + pad + dotR, rect.getCentreY());

            // The reference binds the dot's own opacity to the effect's progress
            // (on top of the chip's engaged/idle opacity), so the dot fill — not
            // just its glow — fades with intensity and vanishes when idle.
            const float dotAlpha = op * chip.glow;

            juce::Path dotPath;
            dotPath.addEllipse (juce::Rectangle<float> (dotR * 2.0f, dotR * 2.0f).withCentre (dot));
            juce::DropShadow (chip.colour.withMultipliedAlpha (dotAlpha), (int) (7.0f * s), {})
                .drawForPath (g, dotPath);

            g.setColour (chip.colour.withMultipliedAlpha (dotAlpha));
            g.fillEllipse (juce::Rectangle<float> (dotR * 2.0f, dotR * 2.0f).withCentre (dot));

            const float textX = dot.x + dotR + 7.0f * s;
            const juce::Rectangle<float> textArea (textX, rect.getY(),
                                                   rect.getRight() - textX - pad, rect.getHeight());
            const auto topHalf = textArea.withHeight (textArea.getHeight() * 0.5f);
            const auto botHalf = textArea.withTop (textArea.getCentreY());

            g.setFont (abbrevFont);
            g.setColour (Palette::textMid.withMultipliedAlpha (op));
            g.drawText (chip.abbrev, topHalf, juce::Justification::bottomLeft, false);

            g.setFont (valueFont);
            g.setColour (Palette::textDimmer.withMultipliedAlpha (op));
            g.drawText (chip.value, botHalf, juce::Justification::topLeft, false);
        }
    }

private:
    float uiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectChips)
};
