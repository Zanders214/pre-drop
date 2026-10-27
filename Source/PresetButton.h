#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <functional>
#include <memory>

#include "PluginProcessor.h"
#include "PreDropLookAndFeel.h"
#include "PreDropTheme.h"

/**
    PresetNamePrompt: asks for a preset's name, over the editor (drawn inside
    it rather than as a window of its own, which a plug-in's host could put
    behind itself). Return saves, Escape cancels; SAVE reads REPLACE when a
    preset of that name exists.
*/
class PresetNamePrompt final : public juce::Component
{
public:
    PresetNamePrompt (const PresetManager& p, const juce::String& name, juce::LookAndFeel& pills) : presets (p)
    {
        using namespace PreDrop;
        nameField.setText (name, false);
        nameField.setSelectAllWhenFocused (true);
        nameField.setColour (juce::TextEditor::backgroundColourId, Palette::panelBottom);
        nameField.setColour (juce::TextEditor::textColourId, Palette::textStrong);
        nameField.setColour (juce::TextEditor::outlineColourId, Palette::hairline);
        nameField.setColour (juce::TextEditor::focusedOutlineColourId, Palette::badge);
        nameField.setColour (juce::CaretComponent::caretColourId, Palette::textStrong);
        nameField.onReturnKey = [this] { finish (1); };
        nameField.onEscapeKey = [this] { finish (0); };
        nameField.onTextChange = [this] { update(); };
        addAndMakeVisible (nameField);

        for (auto* b : { &saveButton, &cancelButton })
        {
            b->setLookAndFeel (&pills);
            PreDropLookAndFeel::setCompactPill (*b);
            addAndMakeVisible (*b);
        }
        cancelButton.setButtonText ("CANCEL");
        cancelButton.onClick = [this] { finish (0); };
        saveButton.onClick   = [this] { finish (1); };
        update();
    }

    ~PresetNamePrompt() override
    {
        for (auto* b : { &saveButton, &cancelButton })
            b->setLookAndFeel (nullptr);
    }

    /** Called with 1 to save, 0 to cancel. */
    void setOnFinish (std::function<void (int)> callback) { onFinish = std::move (callback); }

    juce::TextEditor& field() noexcept       { return nameField; }
    juce::TextButton& save() noexcept        { return saveButton; }

    void focus()
    {
        if (isShowing())
            nameField.grabKeyboardFocus();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = scale();
        g.fillAll (juce::Colours::black.withAlpha (0.6f));
        g.setColour (Palette::panelTop);
        g.fillRoundedRectangle (panel.toFloat(), 12.0f * s);
        g.setColour (Palette::hairline);
        g.drawRoundedRectangle (panel.toFloat(), 12.0f * s, juce::jmax (1.0f, s));
        g.setFont (Fonts::sans (10.0f * s, true, 0.12f));
        g.setColour (Palette::textMid);
        g.drawText ("SAVE PRESET", titleArea.toFloat(), juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const float s = scale();
        const auto px = [s] (float v) { return (int) std::round (v * s); };
        panel = getLocalBounds().withSizeKeepingCentre (px (260.0f), px (104.0f));
        auto area = panel.reduced (px (14.0f), px (12.0f));
        titleArea = area.removeFromTop (px (14.0f));
        area.removeFromTop (px (8.0f));
        nameField.setFont (PreDrop::Fonts::sans (13.0f * s, false, 0.0f));
        nameField.setBounds (area.removeFromTop (px (26.0f)));
        area.removeFromTop (px (10.0f));
        auto row = area.removeFromTop (px (16.0f));
        saveButton.setBounds (row.removeFromRight (px (84.0f)));
        row.removeFromRight (px (8.0f));
        cancelButton.setBounds (row.removeFromRight (px (84.0f)));
    }

    void parentSizeChanged() override
    {
        if (const auto* parent = getParentComponent())
            setBounds (parent->getLocalBounds());
    }

private:
    float scale() const noexcept { return (float) getWidth() / 360.0f; }

    void update()
    {
        const auto name = nameField.getText().trim();
        saveButton.setButtonText (name.isNotEmpty() && presets.fileFor (name).existsAsFile() ? "REPLACE" : "SAVE");
        saveButton.setEnabled (juce::File::createLegalFileName (name).isNotEmpty());
    }

    void finish (int result) const
    {
        if (result == 1 && ! saveButton.isEnabled())
            return;
        if (onFinish != nullptr)
            onFinish (result);   // deletes this
    }

    const PresetManager& presets;
    juce::TextEditor     nameField;
    juce::TextButton     saveButton;
    juce::TextButton     cancelButton;
    juce::Rectangle<int> panel;
    juce::Rectangle<int> titleArea;
    std::function<void (int)> onFinish;   // with 1 to save, 0 to cancel

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetNamePrompt)
};

/**
    PresetButton: the header's preset pill. It shows the preset last saved or
    loaded (PRESETS before there is one) and opens a menu: Init (back to the
    default sound), the saved presets,
    Save preset... (asks for a name over the editor), Export... and Import...
    (any .predrop file, kept in the presets folder too) and the folder itself.
*/
class PresetButton final : public juce::TextButton
{
public:
    PresetButton (PreDropAudioProcessor& p, juce::LookAndFeel& pills) : presets (p.getPresets())
    {
        setLookAndFeel (&pills);
        PreDropLookAndFeel::setCompactPill (*this);
        setPaintingIsUnclipped (true);
        setComponentID ("presets");
        setTooltip ("Presets: save the sound (effects, their settings, Mix, Output and the riser), "
                    "load one, or export and import .predrop files.");
        onClick = [this] { showMenu(); };
        refresh();
    }

    ~PresetButton() override { setLookAndFeel (nullptr); }

    /** Follows the current preset's name. From the editor's timer. */
    void refresh()
    {
        const auto name = presets.getCurrentName();
        if (name == shownName)
            return;
        shownName = name;
        fitLabel();
    }

    void resized() override
    {
        juce::TextButton::resized();
        fitLabel();
    }

    // The menu's item ids; choose() acts on one as if it had been picked.
    static constexpr int saveId        = 1;
    static constexpr int exportId      = 2;
    static constexpr int importId      = 3;
    static constexpr int folderId      = 4;
    static constexpr int initId        = 5;
    static constexpr int firstPresetId = 100;   // + index into the saved presets

    /** The menu the pill opens (built apart from showing it, so it can be
        checked without a screen). Re-reads the presets folder. */
    juce::PopupMenu buildMenu()
    {
        presets.installFactoryPresets();   // the first time only
        listed = presets.getPresets();

        juce::PopupMenu menu;
        menu.addItem (initId, "Init (the default sound)");
        menu.addSeparator();
        menu.addSectionHeader ("Presets");
        if (listed.isEmpty())
            menu.addItem (-1, "None saved yet", false);
        for (int i = 0; i < listed.size(); ++i)
        {
            const auto name = listed[i].getFileNameWithoutExtension();
            menu.addItem (firstPresetId + i, name, true, name == shownName);
        }
        menu.addSeparator();
        menu.addItem (saveId, "Save preset...");
        menu.addItem (exportId, "Export...");
        menu.addItem (importId, "Import...");
        menu.addSeparator();
        menu.addItem (folderId, "Show presets folder", presets.getFolder().isDirectory());
        return menu;
    }

    void choose (int itemId)
    {
        if (itemId == saveId)
            startNaming();
        else if (itemId == exportId)
            chooseExportFile();
        else if (itemId == importId)
            chooseImportFile();
        else if (itemId == folderId)
            presets.getFolder().revealToUser();
        else if (itemId == initId)
            presets.loadInit();
        else if (itemId >= firstPresetId && itemId - firstPresetId < listed.size())
            report (presets.load (listed[itemId - firstPresetId]), "Couldn't load that preset");
        refresh();
    }

    /** Saves under `name` (replacing a preset of that name). */
    void saveAs (const juce::String& name)
    {
        report (presets.save (name), "Couldn't save the preset");
        refresh();
    }

    /** Opens the name prompt over the editor, offering the current name;
        finishNaming() closes it, saving on 1. */
    PresetNamePrompt& startNaming()
    {
        auto* editor = getTopLevelComponent();
        prompt = std::make_unique<PresetNamePrompt> (presets, shownName, getLookAndFeel());
        prompt->setOnFinish ([this] (int result) { finishNaming (result); });
        editor->addAndMakeVisible (*prompt);
        prompt->setBounds (editor->getLocalBounds());
        prompt->focus();
        return *prompt;
    }

    void finishNaming (int result)
    {
        if (prompt == nullptr)
            return;
        const auto name = prompt->field().getText();
        prompt.reset();
        if (result == 1)
            saveAs (name);
    }

    PresetNamePrompt* getPrompt() noexcept { return prompt.get(); }

private:
    void fitLabel()
    {
        const auto label = shownName.isEmpty() ? juce::String ("PRESETS") : shownName.toUpperCase();
        setButtonText (PreDropLookAndFeel::fitPillLabel (label, true, (float) getWidth(),
                                                         PreDropLookAndFeel::pillScale (true, (float) getHeight())));
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

    void chooseExportFile()
    {
        const auto name = shownName.isNotEmpty() ? shownName : juce::String ("PreDrop preset");
        chooser = std::make_unique<juce::FileChooser> ("Export the preset",
                                                       juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
                                                           .getChildFile (juce::File::createLegalFileName (name) + PresetManager::fileExtension),
                                                       juce::String ("*") + PresetManager::fileExtension);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting,
                              [safe = juce::Component::SafePointer (this)] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult() != juce::File())
                report (safe->presets.exportTo (fc.getResult()), "Couldn't export the preset");
        });
    }

    void chooseImportFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Import a preset", juce::File(), juce::String ("*") + PresetManager::fileExtension);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer (this)] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult().existsAsFile())
            {
                report (safe->presets.importFrom (fc.getResult()), "Couldn't import that preset");
                safe->refresh();
            }
        });
    }

    static void report (const juce::String& error, const juce::String& title)
    {
        if (error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, error);
    }

    PresetManager&                     presets;
    juce::Array<juce::File>            listed;
    std::unique_ptr<PresetNamePrompt>  prompt;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String                       shownName { "?" };   // forces the first label

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetButton)
};
