#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/**
    PreDropLookAndFeel

    Native JUCE drawing for the "Neon Clean" Pre-Drop editor:

      - drawRotarySlider: the Amount macro knob — 270° track arc, a multicolour
        cyan->violet->pink->amber value arc with a warm glow, the inset knob body,
        a glowing pointer dot, and the centred "NN %  /  AMOUNT" readout.
      - drawLinearSlider: the Mix / Output sliders — a thin track with a
        cyan->violet fill and a round light thumb.

    All metrics are authored at the 360 px reference scale and multiplied by
    uiScale (set by the editor from getWidth() / 360) so the face scales cleanly.
*/
class PreDropLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PreDropLookAndFeel();

    void setUiScale (float newScale) noexcept { uiScale = juce::jmax (0.1f, newScale); }

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

private:
    void drawKnobReadout (juce::Graphics&, juce::Point<float> centre, float amount, float scale) const;

    float uiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropLookAndFeel)
};
