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

void PresetManager::loadInit()
{
    apply (juce::ValueTree (presetTag));   // mentions nothing, so all of it returns to its default
    setCurrentName ({});
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

// --- Factory presets -------------------------------------------------------------
// Each lists only what differs from the defaults (every depth 50%, Mix 100%,
// Output 0 dB, a 95% room, a synced 1/8D delay at 60% feedback, a 12 dB
// high-pass, the noise riser sweeping 200 Hz to 8 kHz). The riser is the
// built-in noise in each.

const std::vector<PresetManager::Factory>& PresetManager::factoryPresets()
{
    static const std::vector<Factory> presets {
        { "Big Room Lift", {
            { "reverbDepth", 0.7f }, { "reverbSize", 1.0f }, { "reverbDamping", 0.3f }, { "reverbPreDelay", 30.0f },
            { "delayDepth", 0.35f }, { "delayNote", 8.0f }, { "delayFeedback", 0.45f }, { "delayTone", 6000.0f },
            { "riserDepth", 0.55f }, { "riserStart", 150.0f }, { "riserEnd", 10000.0f },
            { "hpfDepth", 0.55f }, { "outputTrim", -1.5f } } },

        { "Tight Club Riser", {
            { "hpfDepth", 0.75f }, { "hpfSlope", 1.0f }, { "hpfResonance", 0.3f },
            { "reverbDepth", 0.35f }, { "reverbSize", 0.6f }, { "reverbDamping", 0.6f },
            { "delayDepth", 0.45f }, { "delayNote", 5.0f }, { "delayFeedback", 0.45f }, { "delayTone", 8000.0f }, { "delayPingPong", 1.0f },
            { "riserDepth", 0.7f }, { "riserStart", 300.0f }, { "riserEnd", 12000.0f }, { "riserResonance", 0.5f } } },

        { "Dub Throw", {
            { "delayDepth", 0.85f }, { "delayNote", 9.0f }, { "delayFeedback", 0.8f }, { "delayTone", 2000.0f }, { "delayPingPong", 1.0f },
            { "reverbDepth", 0.3f }, { "reverbSize", 0.8f }, { "reverbDamping", 0.7f },
            { "riserDepth", 0.2f }, { "hpfDepth", 0.4f }, { "hpfResonance", 0.2f }, { "outputTrim", -2.0f } } },

        { "Resonant Sweep", {
            { "hpfDepth", 0.9f }, { "hpfSlope", 1.0f }, { "hpfResonance", 0.7f },
            { "reverbDepth", 0.25f }, { "reverbSize", 0.7f },
            { "delayDepth", 0.25f }, { "delayNote", 2.0f }, { "delayFeedback", 0.35f },
            { "riserDepth", 0.45f }, { "riserStart", 400.0f }, { "riserEnd", 14000.0f }, { "riserResonance", 0.85f } } },

        { "Ambient Swell", {
            { "reverbDepth", 0.9f }, { "reverbSize", 1.0f }, { "reverbDamping", 0.2f }, { "reverbPreDelay", 80.0f },
            { "delayDepth", 0.4f }, { "delayNote", 10.0f }, { "delayFeedback", 0.7f }, { "delayTone", 3000.0f },
            { "riserDepth", 0.6f }, { "riserStart", 80.0f }, { "riserEnd", 6000.0f }, { "riserResonance", 0.1f },
            { "hpfDepth", 0.3f }, { "mix", 0.9f }, { "outputTrim", -2.5f } } },

        { "Subtle Tension", {
            { "reverbDepth", 0.3f }, { "reverbSize", 0.75f }, { "reverbWidth", 0.8f },
            { "delayDepth", 0.25f }, { "delayNote", 5.0f }, { "delayFeedback", 0.4f }, { "delayTone", 5000.0f },
            { "riserDepth", 0.3f }, { "riserEnd", 5000.0f },
            { "hpfDepth", 0.35f } } },
    };
    return presets;
}

juce::ValueTree PresetManager::makePreset (const Factory& factory)
{
    juce::ValueTree preset (presetTag);
    preset.setProperty ("version", formatVersion, nullptr);
    for (const auto& [id, value] : factory.values)
    {
        juce::ValueTree parameter (parameterTag);
        parameter.setProperty ("id", juce::String (id), nullptr);
        parameter.setProperty ("value", value, nullptr);
        preset.appendChild (parameter, nullptr);
    }
    return preset;
}

int PresetManager::installFactoryPresets()
{
    // A marker in the folder records that they've been written.
    const auto marker = presetFolder.getChildFile (".factory-presets-1");
    if (marker.existsAsFile() || presetFolder.createDirectory().failed())
        return 0;

    int written = 0;
    for (const auto& factory : factoryPresets())
        if (const auto file = fileFor (factory.name); ! file.existsAsFile() && write (makePreset (factory), file).isEmpty())
            ++written;
    marker.create();
    return written;
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
