#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

#include "PluginProcessor.h"
#include "PreDropTheme.h"

/**
    TrackPanel: what a PreDrop set to TRACK shows in place of the curve.

    The link status (following the main, or why not), the track's name as it
    appears in the main's list (double-click to rename), and the HIGH-PASS /
    REVERB / DELAY switches for this track: the same switches the main's
    TRACKS panel edits.
*/
class TrackPanel : public juce::Component
{
public:
    TrackPanel (PreDropAudioProcessor& p, juce::LookAndFeel& pillLookAndFeel) : processor (p)
    {
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

        static constexpr std::array<const char*, 3> labels { "HIGH-PASS", "REVERB", "DELAY" };
        static constexpr std::array<const char*, 3> params { "trackHpf", "trackReverb", "trackDelay" };
        for (size_t i = 0; i < switches.size(); ++i)
        {
            switches[i].setButtonText (labels[i]);
            switches[i].setClickingTogglesState (true);
            switches[i].setLookAndFeel (&pillLookAndFeel);
            switches[i].setPaintingIsUnclipped (true);
            switches[i].onStateChange = [this] { repaint(); };
            addAndMakeVisible (switches[i]);
            attachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                p.getState(), params[i], switches[i]);
        }
        refresh();
    }

    ~TrackPanel() override
    {
        for (auto& s : switches)
            s.setLookAndFeel (nullptr);
    }

    void setUiScale (float s) noexcept { uiScale = juce::jmax (0.1f, s); }

    /** Updates the status and name. Called from the editor's timer. */
    void refresh()
    {
        if (! name.isBeingEdited() && name.getText() != processor.getTrackName())
            name.setText (processor.getTrackName(), juce::dontSendNotification);

        if (const auto status = statusLines().first; status != shownStatus)
        {
            shownStatus = status;
            repaint();
        }
    }

    juce::Label& nameLabel() noexcept { return name; }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = uiScale;
        g.setColour (Palette::chipSurface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f * s);

        const auto [title, detail] = statusLines();
        const bool linked = processor.getLinkHub().tracksActive();
        auto area = getLocalBounds().toFloat().reduced (12.0f * s, 10.0f * s);

        g.setFont (Fonts::sans (10.0f * s, true, 0.12f));
        g.setColour (linked ? Palette::cyan : Palette::amber);
        g.drawText (title, area.removeFromTop (13.0f * s), juce::Justification::centredLeft, false);

        g.setFont (Fonts::sans (9.5f * s, false, 0.02f));
        g.setColour (Palette::textDim);
        g.drawText (detail, area.removeFromTop (13.0f * s), juce::Justification::centredLeft, true);

        g.setFont (Fonts::sans (9.0f * s, true, 0.12f));
        g.setColour (Palette::textDimmer);
        g.drawText ("TRACK NAME", nameCaption.toFloat(), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const float s = uiScale;
        const auto px = [s] (float v) { return (int) std::round (v * s); };
        name.setFont (PreDrop::Fonts::sans (13.0f * s, true));

        auto area = getLocalBounds().reduced (px (12.0f), px (10.0f));
        area.removeFromTop (px (26.0f) + px (8.0f));          // status lines + gap
        auto nameRow = area.removeFromTop (px (20.0f));
        nameCaption = nameRow.removeFromLeft (px (80.0f));
        name.setBounds (nameRow);

        area.removeFromTop (px (10.0f));
        auto row = area.removeFromTop (px (22.0f));
        const int gap = px (8.0f);
        const int w = (row.getWidth() - 2 * gap) / 3;
        for (size_t i = 0; i < switches.size(); ++i)
        {
            switches[i].setBounds (row.removeFromLeft (w));
            row.removeFromLeft (gap);
        }
    }

private:
    std::pair<juce::String, juce::String> statusLines() const
    {
        const auto& hub = processor.getLinkHub();
        if (! hub.hasMain())
            return { "NOT LINKED", "Add PreDrop to your master as MAIN, set to SELECTED TRACKS." };
        if (! hub.tracksActive())
            return { "WAITING FOR THE MAIN", "The main applies to ALL TRACKS; set it to SELECTED TRACKS." };
        return { "LINKED TO THE MAIN", "Follows the main's Amount and effect depths." };
    }

    PreDropAudioProcessor& processor;
    juce::Label name;
    std::array<juce::TextButton, 3> switches;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>, 3> attachments;
    juce::Rectangle<int> nameCaption;
    juce::String shownStatus;
    float uiScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackPanel)
};
