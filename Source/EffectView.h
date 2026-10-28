#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include "EffectSettings.h"
#include "EffectSlotButton.h"
#include "PluginProcessor.h"
#include "PreDropLookAndFeel.h"
#include "PreDropTheme.h"
#include "SettingKnob.h"

/**
    EffectView: the REVERB, DELAY or HIGH-PASS view, opened from the pill under
    that effect's knob in EFFECTS, or the DOWNLIFTER view, opened from its
    button beside EFFECTS. It fills the slot below the chips, button row
    included, like the RISER view.

    Along the top: BACK (to EFFECTS), the effect's source (BUILT-IN or a VST3,
    for the reverb and delay) and its name. Below, the built-in effect's
    settings (EffectSettings): knobs, and pills for the switches. While a
    plug-in stands in for the built-in reverb or delay its settings don't
    apply, so they're dimmed (its own window has its settings).
*/
class EffectView : public juce::Component
{
public:
    enum class Effect { reverb, delay, hpf, drop };

    EffectView (PreDropAudioProcessor& p, Effect which) : processor (p), effect (which)
    {
        using Id = EffectSettings::Id;
        auto& state = p.getState();

        for (auto* b : { &backButton, &syncPill, &pingPongPill, &slopePill, &dropPill })
        {
            b->setLookAndFeel (&pills);
            PreDropLookAndFeel::setCompactPill (*b);
            b->setPaintingIsUnclipped (true);
        }
        backButton.setButtonText ("BACK");
        backButton.setTooltip ("Back to the effect depths.");
        backButton.onClick = [this] { if (onBack != nullptr) onBack(); };
        addAndMakeVisible (backButton);

        const auto colour = accent();
        const auto addKnob = [&] (Id id, const juce::String& tip)
        {
            knobs.push_back (std::make_unique<SettingKnob> (state, id, colour, knobLookAndFeel, tip));
            addAndMakeVisible (*knobs.back());
        };
        const auto addToggle = [&] (juce::TextButton& pill, Id id, const juce::String& text, const juce::String& tip)
        {
            pill.setButtonText (text);
            pill.setClickingTogglesState (true);
            pill.setTooltip (tip);
            toggles.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                state, EffectSettings::specs[(size_t) id].id, pill));
            addAndMakeVisible (pill);
        };

        switch (effect)
        {
            case Effect::reverb:
                source = std::make_unique<EffectSlotButton> (p, PreDropAudioProcessor::EffectSlot::reverb, pills);
                addKnob (Id::reverbSize,     "How big the room gets at full Amount (it grows towards it).");
                addKnob (Id::reverbDamping,  "How quickly the high end dies away in the tail.");
                addKnob (Id::reverbWidth,    "Stereo width of the tail: 0% is mono.");
                addKnob (Id::reverbPreDelay, "A gap before the tail starts, so the sound stays clear in front of it.");
                break;

            case Effect::delay:
                source = std::make_unique<EffectSlotButton> (p, PreDropAudioProcessor::EffectSlot::delay, pills);
                addKnob (Id::delayNote,     "The time between echoes, as a note at the project's tempo.");
                addKnob (Id::delayTime,     "The time between echoes, in milliseconds.");
                addKnob (Id::delayFeedback, "How much of each echo comes back at full Amount (it grows towards it): more means a longer tail.");
                addKnob (Id::delayTone,     "Darkens the echoes (a low-pass on the repeats); OPEN leaves them as they are.");
                addToggle (syncPill, Id::delaySync, "SYNC", "On: the time is a note at the project's tempo. Off: milliseconds.");
                addToggle (pingPongPill, Id::delayPingPong, "PING-PONG", "The echoes bounce between left and right.");
                break;

            case Effect::hpf:
                addKnob (Id::hpfResonance, "A peak at the cutoff that sharpens the sweep as it rises.");
                addToggle (slopePill, Id::hpfSlope, "24 dB SLOPE", "Off: 12 dB per octave, gentle. On: 24 dB, a steeper cut.");
                break;

            case Effect::drop:
                addKnob (Id::dropLength, "How long the downlifter lasts after the drop, in bars.");
                addKnob (Id::dropLevel,  "How loud its noise is: 50% is the riser's level, 0% leaves just the tails.");
                addKnob (Id::dropEnd,    "Where its noise falls to, from where the riser got to.");
                addKnob (Id::dropReverb, "How much of the build-up's reverb rings on after the drop: 100% all of it, 0% none (cut at the drop).");
                addKnob (Id::dropDelay,  "How much of the build-up's echoes ring on after the drop: 100% all of them, 0% none (cut at the drop).");
                addToggle (dropPill, Id::dropOn, "AFTER THE DROP",
                           "On: when Amount drops (from 50% or more to 10% or less within a beat), the riser falls "
                           "and the reverb and delay ring out. Off: the drop cuts them off.");
                break;
        }
        if (source != nullptr)
            addAndMakeVisible (*source);

        refresh();
    }

    ~EffectView() override
    {
        for (auto* b : { &backButton, &syncPill, &pingPongPill, &slopePill, &dropPill })
            b->setLookAndFeel (nullptr);
    }

    /** Called by the BACK button. */
    void setOnBack (std::function<void()> callback) { onBack = std::move (callback); }

    void setUiScale (float s) noexcept
    {
        uiScale = juce::jmax (0.1f, s);
        pills.setUiScale (uiScale);
        knobLookAndFeel.setUiScale (uiScale);
        for (const auto& k : knobs)
            k->setUiScale (uiScale);
    }

    /** Follows the source and the switches. From the editor's timer. */
    void refresh()
    {
        if (source != nullptr)
            source->refresh();

        // SYNC picks which TIME knob shows: the note or the milliseconds.
        if (effect == Effect::delay)
        {
            const bool synced = syncPill.getToggleState();
            knob (EffectSettings::Id::delayNote)->setVisible (synced);
            knob (EffectSettings::Id::delayTime)->setVisible (! synced);
        }

        // A plug-in's own settings replace the built-in one's; a downlifter
        // switched off has none to set.
        const bool builtIn = usesBuiltIn();
        const bool live    = builtIn && (effect != Effect::drop || dropPill.getToggleState());
        for (const auto& k : knobs)
        {
            k->setEnabled (live);
            k->setAlpha (live ? 1.0f : 0.35f);
        }
        for (auto* b : { &syncPill, &pingPongPill })
        {
            b->setEnabled (builtIn);
            b->setAlpha (builtIn ? 1.0f : 0.35f);
        }

        if (const auto note = noteText(); note != shownNote)
        {
            shownNote = note;
            repaint();
        }
    }

    /** True while the built-in effect plays (no plug-in stands in for it). */
    bool usesBuiltIn() const
    {
        if (effect == Effect::hpf || effect == Effect::drop)
            return true;
        const auto slot = effect == Effect::reverb ? PreDropAudioProcessor::EffectSlot::reverb
                                                   : PreDropAudioProcessor::EffectSlot::delay;
        return processor.getEffectPlugins().getName (slot).isEmpty();
    }

    // For tests.
    SettingKnob*      knob (EffectSettings::Id id) noexcept
    {
        for (const auto& k : knobs)
            if (k->settingId() == id)
                return k.get();
        return nullptr;
    }
    juce::TextButton& back() noexcept        { return backButton; }
    juce::TextButton& sync() noexcept        { return syncPill; }
    juce::TextButton& pingPong() noexcept    { return pingPongPill; }
    juce::TextButton& slope() noexcept       { return slopePill; }
    juce::TextButton& dropSwitch() noexcept  { return dropPill; }
    EffectSlotButton* sourcePill() noexcept  { return source.get(); }
    juce::String      getNote() const        { return noteText(); }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = uiScale;
        g.setColour (Palette::chipSurface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f * s);

        g.setFont (Fonts::sans (10.0f * s, true, 0.12f));
        g.setColour (accent());
        g.drawText (title(), titleArea.toFloat(), juce::Justification::centredRight, false);

        g.setFont (Fonts::sans (9.0f * s, false, 0.02f));
        g.setColour (usesBuiltIn() ? Palette::textDim : Palette::amber);
        g.drawText (shownNote, noteArea.toFloat(), juce::Justification::centred, true);
    }

    void resized() override
    {
        const float s = uiScale;
        const auto px = [s] (float v) { return (int) std::round (v * s); };
        auto area = getLocalBounds().reduced (px (10.0f), px (8.0f));
        const int gap = px (6.0f);

        auto header = area.removeFromTop (px (14.0f));
        backButton.setBounds (header.removeFromLeft (px (46.0f)));
        header.removeFromLeft (gap);
        titleArea = header.removeFromRight (px (80.0f));
        header.removeFromRight (gap);
        if (source != nullptr)
            source->setBounds (header.withWidth (juce::jmin (header.getWidth(), px (120.0f))));

        area.removeFromTop (px (4.0f));
        noteArea = area.removeFromTop (px (12.0f));

        // Switches along the bottom, the knobs centred above them.
        std::vector<juce::TextButton*> switches;
        for (auto* b : { &syncPill, &pingPongPill, &slopePill, &dropPill })
            if (b->isVisible())
                switches.push_back (b);
        if (! switches.empty())
        {
            auto row = area.removeFromBottom (px (14.0f));
            area.removeFromBottom (px (6.0f));
            const int w = juce::jmin (px (110.0f), (row.getWidth() - gap * ((int) switches.size() - 1)) / (int) switches.size());
            const int total = w * (int) switches.size() + gap * ((int) switches.size() - 1);
            row = row.withSizeKeepingCentre (total, row.getHeight());
            for (auto* b : switches)
            {
                b->setBounds (row.removeFromLeft (w));
                row.removeFromLeft (gap);
            }
        }

        // The two TIME knobs share a place (SYNC shows one of them).
        std::vector<SettingKnob*> placed;
        for (const auto& k : knobs)
            if (k->settingId() != EffectSettings::Id::delayTime)
                placed.push_back (k.get());
        const int columns = juce::jmax (3, (int) placed.size());
        const int colW    = area.getWidth() / columns;
        const int height  = juce::jmin (area.getHeight(), px (72.0f));
        auto row = area.withSizeKeepingCentre (colW * (int) placed.size(), height);
        for (auto* k : placed)
        {
            const auto cell = row.removeFromLeft (colW).reduced (px (4.0f), 0);
            k->setBounds (cell);
            if (k->settingId() == EffectSettings::Id::delayNote)
                knob (EffectSettings::Id::delayTime)->setBounds (cell);
        }
    }

private:
    juce::Colour accent() const
    {
        using namespace PreDrop::Palette;
        switch (effect)
        {
            case Effect::reverb: return violet;
            case Effect::delay:  return pink;
            case Effect::drop:   return amber;
            case Effect::hpf:    break;
        }
        return cyan;
    }

    juce::String title() const
    {
        switch (effect)
        {
            case Effect::reverb: return "REVERB";
            case Effect::delay:  return "DELAY";
            case Effect::drop:   return "DOWNLIFTER";
            case Effect::hpf:    break;
        }
        return "HIGH-PASS";
    }

    // A line under the header: what the settings do, or why they don't apply.
    juce::String noteText() const
    {
        if (! usesBuiltIn())
            return "A plug-in plays this " + juce::String (effect == Effect::reverb ? "reverb" : "delay")
                 + ": its own window has its settings.";
        switch (effect)
        {
            case Effect::reverb: return "The room grows to SIZE as Amount rises.";
            case Effect::delay:
            {
                const auto settings = effectSettingsOf (processor.getState());
                const double tempo  = processor.getRiser().getHostTempo();
                const auto ms = juce::roundToInt (settings.delaySeconds (tempo) * 1000.0);
                if (! settings.isOn (EffectSettings::Id::delaySync))
                    return "The echoes grow to FEEDBACK as Amount rises.";
                return "Echoes every " + juce::String (ms) + " ms"
                     + (tempo > 0.0 ? juce::String() : juce::String (" (at 120 BPM until the project plays)")) + ".";
            }
            case Effect::drop:
                if (! dropPill.getToggleState())
                    return "Off: the drop cuts the build-up off.";
                return "After the drop: the riser falls and the reverb and delay ring out.";
            case Effect::hpf:    break;
        }
        return "Sweeps up from 20 Hz as Amount rises (to the HIGH-PASS depth's top).";
    }

    PreDropAudioProcessor& processor;
    Effect                 effect;
    PreDropLookAndFeel     pills;
    DepthKnobLookAndFeel   knobLookAndFeel;

    juce::TextButton backButton;
    juce::TextButton syncPill;
    juce::TextButton pingPongPill;
    juce::TextButton slopePill;
    juce::TextButton dropPill;
    std::unique_ptr<EffectSlotButton> source;
    std::vector<std::unique_ptr<SettingKnob>> knobs;
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> toggles;

    juce::Rectangle<int> titleArea;
    juce::Rectangle<int> noteArea;
    juce::String shownNote;
    float uiScale = 1.0f;
    std::function<void()> onBack;   // the BACK button

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectView)
};
