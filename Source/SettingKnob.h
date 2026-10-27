#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <memory>

#include "EffectSettings.h"
#include "PreDropTheme.h"

/**
    SettingKnob: a small knob for one effect setting, with its name and value
    beneath it, tied to its parameter.
*/
class SettingKnob : public juce::Component
{
public:
    SettingKnob (juce::AudioProcessorValueTreeState& state, EffectSettings::Id settingId,
                 juce::Colour colour, juce::LookAndFeel& knobs, const juce::String& tooltip)
        : id (settingId), valueColour (colour)
    {
        knob.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        knob.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setColour (juce::Slider::rotarySliderFillColourId, colour);
        knob.setLookAndFeel (&knobs);
        knob.setPaintingIsUnclipped (true);   // its glow, which reaches past this component too
        knob.setTooltip (tooltip);
        const auto& spec = EffectSettings::specs[(size_t) id];
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, spec.id, knob);
        knob.setDoubleClickReturnValue (true, spec.defaultValue);
        knob.onValueChange = [this] { repaint(); };
        addAndMakeVisible (knob);
        setPaintingIsUnclipped (true);
    }

    ~SettingKnob() override { knob.setLookAndFeel (nullptr); }

    void setUiScale (float s) noexcept { uiScale = juce::jmax (0.1f, s); }

    juce::Slider& slider() noexcept { return knob; }
    EffectSettings::Id settingId() const noexcept { return id; }

    /** The value as shown under the knob. */
    juce::String valueText() const { return EffectSettings::format (id, (float) knob.getValue()); }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = uiScale;
        const auto text = getLocalBounds().toFloat().withTop ((float) knob.getBottom() + 2.0f * s);
        g.setFont (Fonts::sans (9.0f * s, true, 0.08f));
        g.setColour (Palette::textMid);
        g.drawText (EffectSettings::specs[(size_t) id].label, text.withHeight (11.0f * s), juce::Justification::centredTop, false);
        g.setFont (Fonts::mono (9.5f * s));
        g.setColour (valueColour);
        g.drawText (valueText(), text.withTrimmedTop (11.0f * s).withHeight (12.0f * s), juce::Justification::centredTop, false);
    }

    void resized() override
    {
        const auto size = juce::jmin (getWidth(), getHeight() - (int) std::round (25.0f * uiScale));
        knob.setBounds (getLocalBounds().removeFromTop (size).withSizeKeepingCentre (size, size));
    }

private:
    EffectSettings::Id id;
    juce::Slider knob;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    juce::Colour valueColour;
    float uiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingKnob)
};
