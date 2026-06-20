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

    // The EFFECTS reveal toggle, drawn as a pill that matches the BUILD-UP badge.
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

private:
    void drawKnobReadout (juce::Graphics&, juce::Point<float> centre, float amount, float scale) const;

    float uiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropLookAndFeel)
};

/**
    DepthKnobLookAndFeel — a compact rotary for the per-effect depth knobs.

    The main PreDropLookAndFeel knob bakes in the big centred "AMOUNT" readout, so
    these small secondary knobs get their own stripped-down face: a 270° track, a
    value arc tinted with the effect's accent colour (read from the slider's
    rotarySliderFillColourId), an inset body, and a pointer dot — no centred text
    (the editor paints each knob's name and percent below it).
*/
class DepthKnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DepthKnobLookAndFeel() = default;

    void setUiScale (float newScale) noexcept { uiScale = juce::jmax (0.1f, newScale); }

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

private:
    float uiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DepthKnobLookAndFeel)
};
