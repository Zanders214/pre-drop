#include "EffectPlugins.h"

const juce::Identifier EffectPlugins::stateTag { "EFFECT_PLUGINS" };

namespace
{
    // One saved plug-in in the state tree.
    const juce::Identifier slotTag { "SLOT" };
}

EffectPlugins::EffectPlugins()
{
    formats.addFormat (new juce::VST3PluginFormat());   // the manager takes ownership
}

EffectPlugins::~EffectPlugins()
{
    cancelPendingUpdate();
    closeWindows();   // their editors go before their plug-ins
}

void EffectPlugins::prepare (double newSampleRate, int newBlockSize)
{
    // The plug-in the audio thread will run and, if a newer one is still
    // waiting to be picked up, that one too.
    sampleRate = newSampleRate;
    const juce::ScopedLock sl (lock);
    blockSize = newBlockSize;
    for (auto& slot : slots)
    {
        auto* running = slot.handoff.acquire();
        for (auto* fx : { running, slot.latest })
            if (fx != nullptr && ! fx->isEmpty())
                fx->prepare (newSampleRate, newBlockSize);
    }
}

PreDropEngine::ExternalEffect* EffectPlugins::acquire (Slot slot, juce::AudioPlayHead* playHead) noexcept
{
    // The playhead lets tempo-synced effects follow the project's tempo and
    // transport just as they would on a track.
    auto* fx = slots[(size_t) slot].handoff.acquire();
    if (fx == nullptr || fx->isEmpty())
        return nullptr;
    fx->setPlayHead (playHead);
    return fx;
}

void EffectPlugins::collect()
{
    for (auto& slot : slots)
        slot.handoff.collect();
}

// --- Saving and restoring ---------------------------------------------------

void EffectPlugins::saveTo (juce::ValueTree& state) const
{
    // Each loaded plug-in: which one it is and its own settings. A missing one
    // is written back as it was loaded, so a re-save doesn't lose it.
    const juce::ScopedLock sl (lock);
    juce::ValueTree plugins (stateTag);
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto& slot = slots[(size_t) i];
        if (slot.latest != nullptr && ! slot.latest->isEmpty())
        {
            juce::ValueTree saved (slotTag);
            saved.setProperty ("index", i, nullptr);
            if (auto xml = slot.latest->getDescription().createXml())
                saved.setProperty ("description", xml->toString (juce::XmlElement::TextFormat().singleLine()), nullptr);
            juce::MemoryBlock pluginState;
            slot.latest->getPlugin()->getStateInformation (pluginState);
            saved.setProperty ("state", pluginState.toBase64Encoding(), nullptr);
            plugins.appendChild (saved, nullptr);
        }
        else if (slot.missing.isValid())
        {
            plugins.appendChild (slot.missing.createCopy(), nullptr);
        }
    }
    if (plugins.getNumChildren() > 0)
        state.appendChild (plugins, nullptr);
}

void EffectPlugins::restoreFrom (const juce::ValueTree& state)
{
    // Plug-ins (and any open plug-in window) may only be created and destroyed
    // on the message thread, and hosts can restore state on any thread, so the
    // slots are restored there.
    const auto plugins = state.getChildWithName (stateTag);
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        restoreAll (plugins);
        return;
    }

    {
        const juce::ScopedLock sl (lock);
        pendingRestore = plugins.isValid() ? plugins.createCopy() : juce::ValueTree (stateTag);
    }
    triggerAsyncUpdate();
}

void EffectPlugins::handleAsyncUpdate()
{
    juce::ValueTree saved;
    {
        const juce::ScopedLock sl (lock);
        std::swap (saved, pendingRestore);
    }
    if (saved.isValid())
        restoreAll (saved);
}

void EffectPlugins::restoreAll (const juce::ValueTree& saved)
{
    for (int i = 0; i < (int) slots.size(); ++i)
    {
        const auto slot = saved.getChildWithProperty ("index", i);
        if (slot.isValid())
            restore ((Slot) i, slot);
        else
            useBuiltIn ((Slot) i);
    }
}

void EffectPlugins::restore (Slot slot, const juce::ValueTree& saved)
{
    juce::PluginDescription description;
    if (auto xml = juce::parseXML (saved["description"].toString()); xml == nullptr || ! description.loadFromXml (*xml))
    {
        useBuiltIn (slot);
        return;
    }

    // Where it was saved, or a plug-in of the same name in this computer's folders.
    if (! juce::File::isAbsolutePath (description.fileOrIdentifier) || ! juce::File (description.fileOrIdentifier).exists())
    {
        const auto fileName = juce::File::createFileWithoutCheckingPath (description.fileOrIdentifier).getFileName();
        for (const auto& candidate : findInstalledVst3())
            if (candidate.getFileName() == fileName)
                description.fileOrIdentifier = candidate.getFullPathName();
    }

    juce::MemoryBlock pluginState;
    pluginState.fromBase64Encoding (saved["state"].toString());

    if (! juce::File::isAbsolutePath (description.fileOrIdentifier)
        || ! juce::File (description.fileOrIdentifier).exists()
        || install (slot, description, &pluginState).isNotEmpty())
    {
        useBuiltIn (slot);
        const juce::ScopedLock sl (lock);
        slots[(size_t) slot].missing = saved.createCopy();
    }
}

// --- Choosing ----------------------------------------------------------------

juce::String EffectPlugins::load (Slot slot, const juce::File& plugin)
{
    if (! plugin.exists())
        return "\"" + plugin.getFileName() + "\" could not be found.";

    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formats.getFormats())
        if (format->fileMightContainThisPluginType (plugin.getFullPathName()))
            format->findAllTypesForFile (types, plugin.getFullPathName());

    if (types.isEmpty())
        return "\"" + plugin.getFileName() + "\" couldn't be read as a VST3 plug-in.";

    for (const auto* type : types)
        if (! type->isInstrument)
            return install (slot, *type, nullptr);

    return "\"" + types[0]->name + "\" is an instrument; choose an effect.";
}

juce::String EffectPlugins::install (Slot slot, const juce::PluginDescription& description,
                                     const juce::MemoryBlock* pluginState)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const double rate = sampleRate.load() > 0.0 ? sampleRate.load() : 44100.0;
    juce::String error;
    auto instance = formats.createPluginInstance (description, rate, blockSize, error);
    if (instance == nullptr)
        return "\"" + description.name + "\" couldn't be opened" + (error.isNotEmpty() ? ": " + error : juce::String ("."));

    // Stereo in and out, where the plug-in allows it (set before preparing).
    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (juce::AudioChannelSet::stereo());
    stereo.outputBuses.add (juce::AudioChannelSet::stereo());
    if (instance->checkBusesLayoutSupported (stereo))
        instance->setBusesLayout (stereo);

    if (pluginState != nullptr && ! pluginState->isEmpty())
        instance->setStateInformation (pluginState->getData(), (int) pluginState->getSize());

    auto effect = std::make_unique<HostedEffect> (std::move (instance), description);

    const juce::ScopedLock sl (lock);
    if (sampleRate.load() > 0.0)
        effect->prepare (sampleRate.load(), blockSize);   // ready before the audio thread sees it

    auto& state = slots[(size_t) slot];
    state.window.reset();   // its editor belongs to the plug-in being replaced
    state.latest  = effect.get();
    state.name    = description.name;
    state.missing = {};
    state.handoff.send (std::move (effect));
    return {};
}

void EffectPlugins::useBuiltIn (Slot slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto none = std::make_unique<HostedEffect>();

    const juce::ScopedLock sl (lock);
    auto& state = slots[(size_t) slot];
    state.window.reset();
    state.latest  = none.get();
    state.name    = {};
    state.missing = {};
    state.handoff.send (std::move (none));
}

juce::String EffectPlugins::getName (Slot slot) const
{
    const juce::ScopedLock sl (lock);
    return slots[(size_t) slot].name;
}

juce::String EffectPlugins::getMissingName (Slot slot) const
{
    const juce::ScopedLock sl (lock);
    const auto& missing = slots[(size_t) slot].missing;
    if (! missing.isValid())
        return {};

    juce::PluginDescription description;
    if (auto xml = juce::parseXML (missing["description"].toString()); xml != nullptr && description.loadFromXml (*xml))
        return description.name;
    return "a plug-in";
}

bool EffectPlugins::leaksDry (Slot slot) const
{
    const juce::ScopedLock sl (lock);
    const auto* fx = slots[(size_t) slot].latest;
    return fx != nullptr && ! fx->isEmpty() && fx->dryLeak() > HostedEffect::dryLeakWarning;
}

void EffectPlugins::showWindow (Slot slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto& state = slots[(size_t) slot];
    if (state.latest == nullptr || state.latest->isEmpty())
        return;

    if (state.window == nullptr)
    {
        const juce::String title = state.name + (slot == Slot::reverb ? " (PreDrop reverb)" : " (PreDrop delay)");
        auto* owner = &state;
        state.window = std::make_unique<EffectPluginWindow> (*state.latest->getPlugin(), title,
                                                             [owner] { owner->window->setVisible (false); });
    }

    state.window->setVisible (true);
    state.window->toFront (true);
}

juce::AudioPluginInstance* EffectPlugins::getInstance (Slot slot) const
{
    const juce::ScopedLock sl (lock);
    const auto* fx = slots[(size_t) slot].latest;
    return fx != nullptr ? fx->getPlugin() : nullptr;
}

void EffectPlugins::closeWindows()
{
    JUCE_ASSERT_MESSAGE_THREAD
    for (auto& slot : slots)
        slot.window.reset();
}

juce::Array<juce::File> EffectPlugins::findInstalledVst3()
{
    // The standard VST3 folders, and one level of vendor sub-folders in them.
    // Only names are listed: nothing is loaded until one is chosen.
    juce::Array<juce::File> found;
    const auto add = [&found] (const juce::File& f)
    {
        if (f.hasFileExtension ("vst3") && ! f.getFileNameWithoutExtension().equalsIgnoreCase (JucePlugin_Name))
            found.addIfNotAlreadyThere (f);
    };
    const auto addEntry = [&add] (const juce::File& f)
    {
        if (f.hasFileExtension ("vst3"))
        {
            add (f);
            return;
        }
        if (f.isDirectory())
            for (const auto& inner : juce::RangedDirectoryIterator (f, false, "*.vst3", juce::File::findFilesAndDirectories))
                add (inner.getFile());
    };

    const auto folders = juce::VST3PluginFormat().getDefaultLocationsToSearch();
    for (int i = 0; i < folders.getNumPaths(); ++i)
        for (const auto& entry : juce::RangedDirectoryIterator (folders[i], false, "*", juce::File::findFilesAndDirectories))
            addEntry (entry.getFile());

    std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileNameWithoutExtension().compareIgnoreCase (b.getFileNameWithoutExtension()) < 0;
    });
    return found;
}
