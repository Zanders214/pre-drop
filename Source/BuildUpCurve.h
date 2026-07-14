#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "PreDropTheme.h"
#include "PreDropVisualModel.h"

/**
    The build-up energy histogram.

    Paints a 44-bar curve of the composite build-up energy, lighting the bars up
    to the current Amount with the cyan->violet->pink->amber spectrum and leaving
    the rest idle, plus a glowing play-head cursor, the "tension NN%" readout, and
    the 0/25/50/75/100 axis. Horizontal drag/click is a second handle on the same
    Amount parameter, so the curve both displays and edits the macro.
*/
class BuildUpCurve : public juce::Component
{
public:
    BuildUpCurve (juce::AudioProcessorValueTreeState& state, juce::String amountParamId)
        : apvts (state), paramId (std::move (amountParamId))
    {
        amountValue = apvts.getRawParameterValue (paramId);
        amountParam = apvts.getParameter (paramId);
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    }

    void setUiScale (float s) noexcept { uiScale = juce::jmax (0.1f, s); }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;

        const float a = amountValue != nullptr ? amountValue->load() : 0.0f;
        const float s = uiScale;
        auto b = getLocalBounds().toFloat();

        // --- label row ------------------------------------------------------
        auto labelRow = b.removeFromTop (14.0f * s);
        g.setFont (Fonts::sans (10.0f * s, false, 0.1f));
        g.setColour (Palette::textDimmer);
        g.drawText ("BUILD-UP CURVE", labelRow, juce::Justification::centredLeft, false);

        g.setFont (Fonts::mono (10.0f * s));
        g.setColour (Palette::amber);
        g.drawText ("tension " + Format::percent (PreDropVisualModel::energyAt (a)),
                    labelRow, juce::Justification::centredRight, false);

        b.removeFromTop (9.0f * s);
        const auto barsRect = b.removeFromTop (84.0f * s);
        b.removeFromTop (7.0f * s);
        const auto axisRow = b.removeFromTop (12.0f * s);

        // --- bars -----------------------------------------------------------
        const int   N    = 44;
        const float gap  = 2.0f * s;
        const float barW = (barsRect.getWidth() - gap * (float) (N - 1)) / (float) N;

        for (int i = 0; i < N; ++i)
        {
            const float x      = (float) i / (float) (N - 1);
            const float energy = PreDropVisualModel::energyAt (x);
            const float hFrac  = (6.0f + energy * 94.0f) / 100.0f;
            const float barH   = barsRect.getHeight() * hFrac;
            const float bx     = barsRect.getX() + (float) i * (barW + gap);

            const bool lit = x <= a + 1.0e-6f;
            g.setColour (lit ? ColourMath::spectrum (x) : Palette::idleBar);

            // Round only the top corners (flat base sitting on the axis), matching
            // the reference's `border-radius: 2px 2px 0 0`.
            juce::Path bar;
            bar.addRoundedRectangle (bx, barsRect.getBottom() - barH, barW, barH,
                                     2.0f * s, 2.0f * s, true, true, false, false);
            g.fillPath (bar);
        }

        // --- play-head cursor ----------------------------------------------
        const float cx   = barsRect.getX() + a * barsRect.getWidth();
        const float cTop = barsRect.getY() - 4.0f * s;
        const float cBot = barsRect.getBottom();

        // 2px white play-head with a soft 8px glow (spec: 0 0 8px rgba(255,255,255,0.5)),
        // drawn with the same DropShadow technique as the knob/chip dots.
        juce::Path cursor;
        cursor.addRectangle (cx - 1.0f * s, cTop, 2.0f * s, cBot - cTop);
        juce::DropShadow (juce::Colours::white.withAlpha (0.5f), (int) juce::jmax (1.0f, 8.0f * s), {})
            .drawForPath (g, cursor);
        g.setColour (juce::Colours::white);
        g.fillPath (cursor);

        // --- axis labels (space-between) -----------------------------------
        g.setFont (Fonts::mono (9.0f * s));
        g.setColour (Palette::textFaint);
        const char* labels[5] = { "0", "25", "50", "75", "100" };
        const float labelW = 30.0f * s;
        for (int k = 0; k < 5; ++k)
        {
            const float anchor = axisRow.getX() + (float) k / 4.0f * axisRow.getWidth();
            auto just = juce::Justification::centred;
            float lx = anchor - labelW * 0.5f;
            if (k == 0) { just = juce::Justification::centredLeft;  lx = axisRow.getX(); }
            if (k == 4) { just = juce::Justification::centredRight; lx = axisRow.getRight() - labelW; }
            g.drawText (labels[k], juce::Rectangle<float> (lx, axisRow.getY(), labelW, axisRow.getHeight()),
                        just, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (amountParam != nullptr) amountParam->beginChangeGesture();
        setAmountFromMouse (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override { setAmountFromMouse (e); }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (amountParam != nullptr) amountParam->endChangeGesture();
    }

private:
    juce::Rectangle<float> barsRect() const
    {
        auto b = getLocalBounds().toFloat();
        b.removeFromTop (14.0f * uiScale);
        b.removeFromTop (9.0f * uiScale);
        return b.removeFromTop (84.0f * uiScale);
    }

    void setAmountFromMouse (const juce::MouseEvent& e)
    {
        if (amountParam == nullptr) return;
        const auto rect = barsRect();
        if (rect.getWidth() <= 0.0f) return;
        const float norm = juce::jlimit (0.0f, 1.0f, ((float) e.position.x - rect.getX()) / rect.getWidth());
        amountParam->setValueNotifyingHost (norm);
        repaint();
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::String                        paramId;
    std::atomic<float>*                 amountValue = nullptr;
    juce::RangedAudioParameter*         amountParam = nullptr;
    float                               uiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BuildUpCurve)
};
