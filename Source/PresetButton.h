#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

#include "PluginProcessor.h"
#include "PreDropLookAndFeel.h"
#include "PreDropTheme.h"

/**
    PresetButton: the header's preset pill. It shows the preset last saved or
    loaded (PRESETS before there is one) and opens a menu: the saved presets,
    Save preset... (asks for a name), Export... and Import... (any .predrop
    file, kept in the presets folder too) and the folder itself.
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
    static constexpr int firstPresetId = 100;   // + index into the saved presets

    /** The menu the pill opens (built apart from showing it, so it can be
        checked without a screen). Re-reads the presets folder. */
    juce::PopupMenu buildMenu()
    {
        listed = presets.getPresets();

        juce::PopupMenu menu;
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
            askForName();
        else if (itemId == exportId)
            chooseExportFile();
        else if (itemId == importId)
            chooseImportFile();
        else if (itemId == folderId)
            presets.getFolder().revealToUser();
        else if (itemId >= firstPresetId && itemId - firstPresetId < listed.size())
            report (presets.load (listed[itemId - firstPresetId]), "Couldn't load that preset");
        refresh();
    }

    /** Saves under `name`, after asking before replacing a preset of that name. */
    void saveAs (const juce::String& name)
    {
        if (! presets.fileFor (name).existsAsFile() || name.trim().isEmpty())
        {
            report (presets.save (name), "Couldn't save the preset");
            refresh();
            return;
        }

        const auto options = juce::MessageBoxOptions::makeOptionsOkCancel (
            juce::MessageBoxIconType::QuestionIcon, "Replace " + name.trim() + "?",
            "A preset with that name exists. Replace it with this sound?", "Replace", "Cancel", this);
        juce::AlertWindow::showAsync (options, [safe = juce::Component::SafePointer (this), name] (int result)
        {
            if (safe != nullptr && result == 1)
            {
                safe->report (safe->presets.save (name), "Couldn't save the preset");
                safe->refresh();
            }
        });
    }

    /** The name dialog while it's open (for tests). */
    juce::AlertWindow* getNameDialog() noexcept { return nameDialog.get(); }

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

    void askForName()
    {
        nameDialog = std::make_unique<juce::AlertWindow> ("Save preset", "Name this sound:", juce::MessageBoxIconType::NoIcon, this);
        nameDialog->addTextEditor ("name", shownName);
        nameDialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        nameDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        nameDialog->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer (this)] (int result)
        {
            if (safe == nullptr || safe->nameDialog == nullptr)
                return;
            const auto name = safe->nameDialog->getTextEditorContents ("name");
            safe->nameDialog.reset();
            if (result == 1)
                safe->saveAs (name);
        }), false);
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
                safe->report (safe->presets.exportTo (fc.getResult()), "Couldn't export the preset");
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
                safe->report (safe->presets.importFrom (fc.getResult()), "Couldn't import that preset");
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
    std::unique_ptr<juce::AlertWindow> nameDialog;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String                       shownName { "?" };   // forces the first label

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetButton)
};
