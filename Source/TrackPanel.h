#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

#include "EffectSlotButton.h"
#include "PluginProcessor.h"
#include "PreDropLookAndFeel.h"
#include "PreDropTheme.h"

/**
    TrackPanel: what a PreDrop set to TRACK or RETURN shows in place of the curve.

    The link status (following the main, or why not), the track's name as it
    appears in the main's list (double-click to rename), and this track's
    HIGH-PASS / REVERB / DELAY levels: its share of the main's depth for each
    effect, the same levels the main's TRACKS panel edits, with the reverb's and
    delay's sources (built-in or a plug-in) under theirs. A RETURN has no row
    in that list (each track's send knob is its amount), so in return mode the
    name row explains the sends instead.
*/
class TrackPanel : public juce::Component
{
public:
    explicit TrackPanel (PreDropAudioProcessor& p)
        : processor (p),
          reverbSlot (p, PreDropAudioProcessor::EffectSlot::reverb, pillLookAndFeel),
          delaySlot  (p, PreDropAudioProcessor::EffectSlot::delay,  pillLookAndFeel)
    {
        addAndMakeVisible (reverbSlot);
        addAndMakeVisible (delaySlot);

        name.setEditable (false, true, false);   // double-click to rename
        name.setJustificationType (juce::Justification::centredLeft);
        name.setColour (juce::Label::textColourId, PreDrop::Palette::textStrong);
        name.setColour (juce::Label::textWhenEditingColourId, PreDrop::Palette::textStrong);
        name.setColour (juce::Label::backgroundWhenEditingColourId, PreDrop::Palette::panelBottom);
        name.setColour (juce::Label::outlineWhenEditingColourId, PreDrop::Palette::badge);
        name.setTooltip ("This track's name in the main's TRACKS list. Double-click to rename; clear it to use the track's own name.");
        name.onTextChange = [this]
        {
            processor.setTrackName (name.getText());
            refresh();
        };
        addAndMakeVisible (name);

        for (size_t i = 0; i < levels.size(); ++i)
        {
            auto& knob = levels[i];
            knob.setSliderStyle (juce::Slider::RotaryVerticalDrag);
            knob.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
            knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            knob.setColour (juce::Slider::rotarySliderFillColourId, colours()[i]);
            knob.setDoubleClickReturnValue (true, 1.0);
            knob.setLookAndFeel (&knobLookAndFeel);
            knob.setPaintingIsUnclipped (true);       // its glow reaches past the knob;
            knob.onValueChange = [this] { repaint(); };   // the panel repaints around it
            addAndMakeVisible (knob);
            attachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                p.getState(), paramIds[i], knob);
        }
        refresh();
    }

    ~TrackPanel() override
    {
        for (auto& knob : levels)
            knob.setLookAndFeel (nullptr);
    }

    void setUiScale (float s) noexcept
    {
        uiScale = juce::jmax (0.1f, s);
        knobLookAndFeel.setUiScale (uiScale);
        pillLookAndFeel.setUiScale (uiScale);
    }

    /** Updates the status and name. Called from the editor's timer. */
    void refresh()
    {
        if (! name.isBeingEdited() && name.getText() != processor.getTrackName())
            name.setText (processor.getTrackName(), juce::dontSendNotification);
        reverbSlot.refresh();
        delaySlot.refresh();

        if (const auto status = statusLines().first; status != shownStatus)
        {
            shownStatus = status;
            repaint();
        }
    }

    void setReturnMode (bool isReturn)
    {
        if (returnMode == isReturn)
            return;
        returnMode = isReturn;
        name.setVisible (! isReturn);
        shownStatus = {};
        refresh();
        repaint();
    }

    juce::Label& nameLabel() noexcept { return name; }
    juce::Slider& levelKnob (LinkHub::Effect effect) noexcept { return levels[(size_t) effect]; }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = uiScale;
        g.setColour (Palette::chipSurface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f * s);

        const auto [title, detail] = statusLines();
        const bool linked = processor.getLinkHub().linkActive();
        auto area = getLocalBounds().toFloat().reduced (12.0f * s, 10.0f * s);

        g.setFont (Fonts::sans (10.0f * s, true, 0.12f));
        g.setColour (linked ? Palette::cyan : Palette::amber);
        g.drawText (title, area.removeFromTop (13.0f * s), juce::Justification::centredLeft, false);

        g.setFont (Fonts::sans (9.5f * s, false, 0.02f));
        g.setColour (Palette::textDim);
        g.drawText (detail, area.removeFromTop (13.0f * s), juce::Justification::centredLeft, true);

        g.setFont (Fonts::sans (9.0f * s, true, 0.12f));
        g.setColour (Palette::textDimmer);
        g.drawText (returnMode ? "SENDS" : "TRACK NAME", nameCaption.toFloat(), juce::Justification::centredLeft, false);
        if (returnMode)
        {
            g.setFont (Fonts::sans (10.0f * s, false, 0.02f));
            g.setColour (Palette::textMid);
            g.drawText ("Each track's send knob sets how much it gets.", name.getBounds().toFloat(),
                        juce::Justification::centredLeft, true);
        }

        // Each level knob: its effect name beside it, the percentage under that.
        for (size_t i = 0; i < levels.size(); ++i)
        {
            const auto text = levelText[i].toFloat();
            g.setFont (Fonts::sans (9.0f * s, true, 0.06f));
            g.setColour (Palette::textMid);
            g.drawText (labels[i], text.withHeight (text.getHeight() * 0.5f), juce::Justification::bottomLeft, false);
            g.setFont (Fonts::mono (9.5f * s));
            g.setColour (colours()[i]);
            g.drawText (Format::percent ((float) levels[i].getValue()),
                        text.withTrimmedTop (text.getHeight() * 0.5f), juce::Justification::topLeft, false);
        }
    }

    void resized() override
    {
        const float s = uiScale;
        const auto px = [s] (float v) { return (int) std::round (v * s); };
        name.setFont (PreDrop::Fonts::sans (13.0f * s, true));

        auto area = getLocalBounds().reduced (px (12.0f), px (10.0f));
        area.removeFromTop (px (26.0f) + px (6.0f));          // status lines + gap
        auto nameRow = area.removeFromTop (px (20.0f));
        nameCaption = nameRow.removeFromLeft (px (80.0f));
        name.setBounds (nameRow);

        // Three columns, each a small knob with its label and level beside it
        // and, for the reverb and delay, the effect's source underneath.
        area.removeFromTop (px (8.0f));
        auto row = area.removeFromTop (px (36.0f) + px (4.0f) + px (14.0f));
        const int colW = row.getWidth() / 3;
        const std::array<EffectSlotButton*, 3> sources { nullptr, &reverbSlot, &delaySlot };
        for (size_t i = 0; i < levels.size(); ++i)
        {
            auto col = row.removeFromLeft (colW);
            auto knobRow = col.removeFromTop (px (36.0f));
            levels[i].setBounds (knobRow.removeFromLeft (px (36.0f)).withSizeKeepingCentre (px (36.0f), px (36.0f)));
            knobRow.removeFromLeft (px (5.0f));
            levelText[i] = knobRow.withSizeKeepingCentre (knobRow.getWidth(), px (26.0f));
            col.removeFromTop (px (4.0f));
            if (sources[i] != nullptr)
                sources[i]->setBounds (col.removeFromTop (px (14.0f)).withTrimmedRight (px (8.0f)));
        }
    }

private:
    static constexpr std::array<const char*, 3> labels   { "HIGH-PASS", "REVERB", "DELAY" };
    static constexpr std::array<const char*, 3> paramIds { "trackHpf", "trackReverb", "trackDelay" };

    static std::array<juce::Colour, 3> colours()
    {
        using namespace PreDrop::Palette;
        return { cyan, violet, pink };
    }

    std::pair<juce::String, juce::String> statusLines() const
    {
        const auto& hub = processor.getLinkHub();
        if (! hub.linkActive())
            return { "NOT LINKED", "Add PreDrop to your master (it becomes the MAIN) to link up." };
        if (returnMode)
            return { "LINKED AS A RETURN", "Adds reverb and delay, wet only, to what tracks send here." };
        return { "LINKED TO THE MAIN", "Levels below are this track's share of the main's depths." };
    }

    PreDropAudioProcessor& processor;
    DepthKnobLookAndFeel   knobLookAndFeel;
    PreDropLookAndFeel     pillLookAndFeel;
    EffectSlotButton       reverbSlot;
    EffectSlotButton       delaySlot;
    juce::Label name;
    std::array<juce::Slider, 3> levels;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 3> attachments;
    std::array<juce::Rectangle<int>, 3> levelText;
    juce::Rectangle<int> nameCaption;
    juce::String shownStatus;
    float uiScale    = 1.0f;
    bool  returnMode = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackPanel)
};
