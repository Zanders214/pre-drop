#include "PresetManager.h"

#include <algorithm>

#include "ParamIDs.h"
#include "PluginProcessor.h"

const juce::Identifier PresetManager::presetTag { "PREDROP_PRESET" };

namespace
{
const juce::Identifier parameterTag  { "PARAM" };
const juce::Identifier currentNameId { "presetName" };   // on the processor's state, saved with the project
constexpr int formatVersion = 1;
}

PresetManager::PresetManager (PreDropAudioProcessor& p)
    : processor (p), presetFolder (defaultFolder())
{
}

juce::File PresetManager::defaultFolder()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
               .getChildFile ("Library/Audio/Presets/Zanders/PreDrop");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Zanders/PreDrop/Presets");
   #endif
}

juce::Array<juce::File> PresetManager::getPresets() const
{
    auto files = presetFolder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0;
    });
    return files;
}

juce::String PresetManager::getCurrentName() const
{
    return processor.getState().state.getProperty (currentNameId).toString();
}

void PresetManager::setCurrentName (const juce::String& name)
{
    processor.getState().state.setProperty (currentNameId, name, nullptr);
}

bool PresetManager::isSoundParameter (const juce::String& id)
{
    // Amount is the performance, the role and track levels the session's set-up.
    for (const auto* left : { ParamID::amount, ParamID::role, ParamID::trackHpf, ParamID::trackReverb, ParamID::trackDelay })
        if (id == left)
            return false;
    return true;
}

juce::ValueTree PresetManager::capture() const
{
    juce::ValueTree preset (presetTag);
    preset.setProperty ("version", formatVersion, nullptr);

    for (auto* p : processor.getParameters())
        if (auto* withId = dynamic_cast<juce::RangedAudioParameter*> (p); withId != nullptr && isSoundParameter (withId->getParameterID()))
        {
            juce::ValueTree value (parameterTag);
            value.setProperty ("id", withId->getParameterID(), nullptr);
            value.setProperty ("value", withId->convertFrom0to1 (withId->getValue()), nullptr);
            preset.appendChild (value, nullptr);
        }

    processor.getRiser().saveTo (preset);
    processor.getEffectPlugins().saveTo (preset);
    return preset;
}

bool PresetManager::apply (const juce::ValueTree& preset)
{
    if (! preset.hasType (presetTag))
        return false;

    for (auto* p : processor.getParameters())
    {
        auto* withId = dynamic_cast<juce::RangedAudioParameter*> (p);
        if (withId == nullptr || ! isSoundParameter (withId->getParameterID()))
            continue;

        const auto saved = preset.getChildWithProperty ("id", withId->getParameterID());
        const float normalised = saved.isValid() ? withId->convertTo0to1 ((float) saved.getProperty ("value"))
                                                 : withId->getDefaultValue();
        withId->beginChangeGesture();
        withId->setValueNotifyingHost (normalised);
        withId->endChangeGesture();
    }

    processor.getRiser().restoreFrom (preset);
    processor.getEffectPlugins().restoreFrom (preset);
    return true;
}

juce::File PresetManager::fileFor (const juce::String& name) const
{
    return presetFolder.getChildFile (juce::File::createLegalFileName (name.trim()) + fileExtension);
}

juce::String PresetManager::save (const juce::String& name)
{
    const auto legal = juce::File::createLegalFileName (name.trim());
    if (legal.isEmpty())
        return "Give the preset a name.";
    if (const auto made = presetFolder.createDirectory(); made.failed())
        return "Couldn't make the presets folder: " + made.getErrorMessage();

    if (const auto error = write (capture(), fileFor (legal)); error.isNotEmpty())
        return error;
    setCurrentName (legal);
    return {};
}

juce::String PresetManager::exportTo (const juce::File& file) const
{
    return write (capture(), file.withFileExtension (fileExtension));
}

juce::String PresetManager::load (const juce::File& file)
{
    if (! apply (read (file)))
        return file.getFileName() + " isn't a PreDrop preset.";
    setCurrentName (file.getFileNameWithoutExtension());
    return {};
}

juce::String PresetManager::importFrom (const juce::File& file)
{
    const auto preset = read (file);
    if (! preset.hasType (presetTag))
        return file.getFileName() + " isn't a PreDrop preset.";

    // Keep a copy beside the others, under its own name (numbered if another
    // preset has it); the same file imported again isn't copied twice.
    auto kept = file;
    if (file.getParentDirectory() != presetFolder)
    {
        if (const auto made = presetFolder.createDirectory(); made.failed())
            return "Couldn't make the presets folder: " + made.getErrorMessage();
        const auto name = file.getFileNameWithoutExtension();
        kept = fileFor (name);
        for (int n = 2; kept.existsAsFile() && ! kept.hasIdenticalContentTo (file); ++n)
            kept = fileFor (name + " (" + juce::String (n) + ")");
        if (! kept.existsAsFile() && ! file.copyFileTo (kept))
            return "Couldn't copy " + file.getFileName() + " into the presets folder.";
    }

    apply (preset);
    setCurrentName (kept.getFileNameWithoutExtension());
    return {};
}

juce::String PresetManager::write (const juce::ValueTree& preset, const juce::File& file)
{
    const auto xml = preset.createXml();
    if (xml == nullptr || ! xml->writeTo (file))
        return "Couldn't write " + file.getFullPathName() + ".";
    return {};
}

juce::ValueTree PresetManager::read (const juce::File& file)
{
    if (const auto xml = juce::XmlDocument::parse (file))
        return juce::ValueTree::fromXml (*xml);
    return {};
}
