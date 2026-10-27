#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

#include "PluginProcessor.h"
#include "PreDropLookAndFeel.h"
#include "PreDropTheme.h"

/**
    EffectSlotButton: picks what PreDrop uses as its reverb or its delay.

    A compact pill under its effect's knob: BUILT-IN, or the loaded plug-in's
    name (lit), shortened to fit. Clicking opens a menu:
    the built-in effect, the VST3 plug-ins installed in the standard folders,
    a file browser for any other one, and the loaded plug-in's own window. It
    warns (amber, with a tooltip) when a saved plug-in isn't installed here, or
    when the loaded one seems to pass its dry signal through.
*/
class EffectSlotButton : public juce::TextButton
{
public:
    using Slot = PreDropAudioProcessor::EffectSlot;

    EffectSlotButton (PreDropAudioProcessor& p, Slot s, juce::LookAndFeel& pills) : processor (p), slot (s)
    {
        setLookAndFeel (&pills);
        PreDropLookAndFeel::setCompactPill (*this);
        setPaintingIsUnclipped (true);   // the lit pill's glow reaches past it
        onClick = [this] { showMenu(); };
        refresh();
    }

    ~EffectSlotButton() override { setLookAndFeel (nullptr); }

    /** Updates the label and warnings. Called from the editor's timer. */
    void refresh()
    {
        const auto name    = processor.getEffectPluginName (slot);
        const auto missing = processor.getMissingEffectPluginName (slot);
        const bool leaks   = processor.effectPluginLeaksDry (slot);

        // Warnings are short to fit the pill; the tooltip says the rest.
        juce::String label = name.isNotEmpty() ? name.toUpperCase() : juce::String ("BUILT-IN");
        if (missing.isNotEmpty())
            label = "MISSING";
        else if (leaks)
            label = "100% WET?";

        if (label == shownLabel)
            return;

        shownLabel = label;
        fitLabel();
        setToggleState (name.isNotEmpty(), juce::dontSendNotification);
        setColour (juce::TextButton::textColourOffId, missing.isNotEmpty() || leaks ? PreDrop::Palette::amber : PreDrop::Palette::textStrong);

        const juce::String kind = slot == Slot::reverb ? "reverb" : "delay";
        if (missing.isNotEmpty())
            setTooltip (missing + " isn't installed on this computer, so the built-in " + kind
                        + " plays instead. Its settings are kept for when it is.");
        else if (leaks)
            setTooltip (name + " is passing its dry signal through. Set its mix to 100% wet: PreDrop sets the "
                        + kind + "'s level itself.");
        else if (name.isNotEmpty())
            setTooltip (name + " is PreDrop's " + kind + ". Click to open its window or choose another.");
        else
            setTooltip ("The built-in " + kind + ". Click to use one of your VST3 plug-ins instead.");

        if (auto* parent = getParentComponent())
            parent->repaint();   // the lit pill's glow
    }

    void resized() override
    {
        juce::TextButton::resized();
        fitLabel();
    }

    // The menu's item ids; choose() acts on one as if it had been picked.
    static constexpr int builtInId     = 1;
    static constexpr int browseId      = 2;
    static constexpr int windowId      = 3;
    static constexpr int firstPluginId = 100;   // + index into the installed list

    void choose (int itemId)
    {
        if (itemId == builtInId)
            processor.useBuiltInEffect (slot);
        else if (itemId == windowId)
            processor.showEffectPluginWindow (slot);
        else if (itemId == browseId)
            browse();
        else if (itemId >= firstPluginId && itemId - firstPluginId < installed.size())
            loadPlugin (installed[itemId - firstPluginId]);
        refresh();
    }

    /** Loads a plug-in into this slot, reporting a failure, and (by default)
        opens its window straight away so its sound can be shaped. */
    void loadPlugin (const juce::File& plugin, bool openWindow = true)
    {
        if (const auto error = processor.loadEffectPlugin (slot, plugin); error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't use that plug-in", error);
        else if (openWindow)
            processor.showEffectPluginWindow (slot);
        refresh();
    }

    /** The menu the pill opens (built separately from showing it, so it can be
        checked without a screen). Refreshes the list of installed plug-ins. */
    juce::PopupMenu buildMenu()
    {
        const juce::String kind = slot == Slot::reverb ? "reverb" : "delay";
        const auto loaded = processor.getEffectPluginName (slot);
        installed = PreDropAudioProcessor::findInstalledVst3Plugins();

        juce::PopupMenu plugins;
        for (int i = 0; i < installed.size(); ++i)
            plugins.addItem (firstPluginId + i, installed[i].getFileNameWithoutExtension());

        juce::PopupMenu menu;
        menu.addItem (builtInId, "Built-in " + kind, true, loaded.isEmpty());
        menu.addSubMenu ("Your VST3 plug-ins", plugins, ! installed.isEmpty());
        menu.addItem (browseId, "Browse for a VST3...");
        if (loaded.isNotEmpty())
        {
            menu.addSeparator();
            menu.addItem (windowId, "Open " + loaded + " window");
        }
        menu.addSeparator();
        menu.addItem (-1, "Set your plug-in to 100% wet: PreDrop sets its level.", false);
        return menu;
    }

private:
    // Shortens the label (with an ellipsis) until it fits the pill.
    void fitLabel()
    {
        const float scale = PreDropLookAndFeel::pillScale (true, (float) getHeight());
        setButtonText (PreDropLookAndFeel::fitPillLabel (shownLabel, true, (float) getWidth(), scale));
    }

    void showMenu()
    {
        buildMenu().showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [safe = juce::Component::SafePointer (this)] (int choice)
        {
            if (safe != nullptr && choice > 0)
                safe->choose (choice);
        });
    }

    void browse()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a VST3 effect", juce::File(), "*.vst3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer (this)] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult() != juce::File())
                safe->loadPlugin (fc.getResult());
        });
    }

    PreDropAudioProcessor&             processor;
    Slot                               slot;
    juce::Array<juce::File>            installed;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String                       shownLabel { "?" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectSlotButton)
};
