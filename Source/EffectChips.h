#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>

#include "PreDropTheme.h"
#include "PreDropVisualModel.h"

/**
    The four effect status chips (HPF / REV / DLY / RIS).

    Each chip shows a coloured status dot whose glow tracks the effect's
    intensity, the effect abbreviation, and a live derived value (HPF cutoff;
    reverb / delay / riser intensity, i.e. Amount x that effect's depth, as a
    percentage). A chip is fully opaque once its effect is engaged (Amount and
    its depth above 0) and dim before that.

    Read-only: it reflects Amount and the depths via PreDropVisualModel and never
    writes a parameter, so the editor just feeds it the current values.
*/
class EffectChips : public juce::Component
{
public:
    EffectChips() { setInterceptsMouseClicks (false, false); }

    enum class Effect { hpf, reverb, delay, riser };

    void setAmountSource (std::function<float()> source) { getAmount = std::move (source); }

    /** Optional per effect; an effect without a source shows the default depth. */
    void setDepthSource (Effect effect, std::function<float()> source)
    {
        getDepth[(size_t) effect] = std::move (source);
    }
    void setUiScale (float s) noexcept { uiScale = juce::jmax (0.1f, s); }

    void paint (juce::Graphics& g) override
    {
        const float a = getAmount ? getAmount() : 0.0f;
        const auto depth = [this] (Effect e)
        {
            const auto& source = getDepth[(size_t) e];
            return source ? source() : defaultDepth;
        };
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

        using M = PreDropVisualModel;
        const auto chipFor = [&] (const char* abbrev, juce::Colour colour, Effect e)
        {
            const float d = depth (e);
            const auto value = e == Effect::hpf ? Format::hertz (M::cutoffHz (a, d))
                                                : Format::percent (M::intensity (a, d));
            return Chip { abbrev, colour, M::intensity (a, d), M::engaged (a, d), value };
        };

        const std::array<Chip, 4> chips
        {{
            chipFor ("HPF", Palette::cyan,   Effect::hpf),
            chipFor ("REV", Palette::violet, Effect::reverb),
            chipFor ("DLY", Palette::pink,   Effect::delay),
            chipFor ("RIS", Palette::amber,  Effect::riser),
        }};

        const auto  n     = (int) chips.size();
        const float gap   = 7.0f * s;
        const float chipW = (bounds.getWidth() - gap * (n - 1)) / (float) n;
        const float pad   = 9.0f * s;
        const float dotR  = 4.0f * s;

        const auto abbrevFont = Fonts::sans (10.0f * s, true);
        const auto valueFont  = Fonts::mono (8.5f * s);

        for (int i = 0; i < n; ++i)
        {
            const auto& chip = chips[(size_t) i];
            const float op = chip.engaged ? 1.0f : 0.4f;

            const juce::Rectangle rect (bounds.getX() + (float) i * (chipW + gap),
                                        bounds.getY(), chipW, bounds.getHeight());

            g.setColour (Palette::chipSurface.withMultipliedAlpha (op));
            g.fillRoundedRectangle (rect, 10.0f * s);

            const juce::Point dot (rect.getX() + pad + dotR, rect.getCentreY());

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
            const juce::Rectangle textArea (textX, rect.getY(),
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
    static constexpr float defaultDepth = 0.5f;   // matches the depth parameters' default

    std::function<float()> getAmount;
    std::array<std::function<float()>, 4> getDepth;
    float uiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectChips)
};
