#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace ParamID
{
    static constexpr const char* amount      = "amount";
    static constexpr const char* mix         = "mix";
    static constexpr const char* outputTrim  = "outputTrim";
    static constexpr const char* reverbDepth = "reverbDepth";
    static constexpr const char* delayDepth  = "delayDepth";
    static constexpr const char* riserDepth  = "riserDepth";
    static constexpr const char* hpfDepth    = "hpfDepth";
    static constexpr const char* role        = "role";
    static constexpr const char* trackHpf    = "trackHpf";
    static constexpr const char* trackReverb = "trackReverb";
    static constexpr const char* trackDelay  = "trackDelay";
}

namespace
{
    // Child of the saved state that carries the riser sample, if one is loaded.
    const juce::Identifier riserStateTag { "RISER_SAMPLE" };

    // State property holding a track name typed into PreDrop.
    const juce::Identifier trackNameTag { "trackName" };

    // Child of the saved state with the reverb / delay plug-ins, one SLOT each.
    const juce::Identifier effectPluginsTag { "EFFECT_PLUGINS" };
    const juce::Identifier effectSlotTag    { "SLOT" };

    // The main's controls that linked tracks follow.
    constexpr std::array<const char*, 4> linkedParameters { ParamID::amount, ParamID::hpfDepth, ParamID::reverbDepth,
                                                            ParamID::delayDepth };

    // Parameter holding whether a linked track applies `effect`.
    const char* trackEffect (LinkHub::Effect effect)
    {
        switch (effect)
        {
            case LinkHub::Effect::reverb: return ParamID::trackReverb;
            case LinkHub::Effect::delay:  return ParamID::trackDelay;
            case LinkHub::Effect::hpf:    break;
        }
        return ParamID::trackHpf;
    }

    // Largest file accepted, so a stray long recording can't bloat the project.
    constexpr juce::int64 maxRiserFileBytes = 64 * 1024 * 1024;
}

PreDropAudioProcessor::PreDropAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    formats.registerBasicFormats();
    effectPlugins.formats.addFormat (new juce::VST3PluginFormat());

    // Publish the main's controls the moment they change (from the UI, the
    // host's automation, or a state load), not only when it next processes.
    for (auto* id : linkedParameters)
        apvts.addParameterListener (id, this);

    // Added to a set that already has a main: join it as a TRACK, so dropping
    // PreDrop on a track is all it takes. A saved role still wins, since the
    // host restores the state right after construction.
    if (linkHub->hasMain())
    {
        auto* role = apvts.getParameter (ParamID::role);
        role->setValueNotifyingHost (role->convertTo0to1 ((float) (int) Role::track));
    }

    updateLink();
    startTimerHz (10);   // keeps the link current; frees swapped-out riser samples
}

PreDropAudioProcessor::~PreDropAudioProcessor()
{
    stopTimer();
    cancelPendingUpdate();
    closeEffectPluginWindows();   // their editors go before their plug-ins
    for (auto* id : linkedParameters)
        apvts.removeParameterListener (id, this);
    if (link.registered)
        linkHub->removeTrack (this);
    if (link.registeredReturn)
        linkHub->removeReturn (this);
    linkHub->releaseMain (this);
}

juce::AudioProcessorValueTreeState::ParameterLayout PreDropAudioProcessor::createParameterLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto percentFromValue = [] (float value, int) { return String (roundToInt (value * 100.0f)) + " %"; };

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::amount, 1 }, "Amount",
        NormalisableRange<float> (0.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (percentFromValue)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::mix, 1 }, "Mix",
        NormalisableRange<float> (0.0f, 1.0f), 1.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (percentFromValue)));

    layout.add (std::make_unique<AudioParameterFloat> (
        ParameterID { ParamID::outputTrim, 1 }, "Output Trim",
        NormalisableRange<float> (-24.0f, 6.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    // Per-effect depth: how strong each effect gets at full build-up. The 50%
    // default is the balanced setting where every effect sits at the same level;
    // 0% switches that effect off and 100% doubles it. Amount drives them all.
    const auto addDepth = [&] (const char* id, const char* name)
    {
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { id, 1 }, name,
            NormalisableRange<float> (0.0f, 1.0f), 0.5f,
            AudioParameterFloatAttributes().withStringFromValueFunction (percentFromValue)));
    };

    addDepth (ParamID::reverbDepth, "Reverb");
    addDepth (ParamID::delayDepth,  "Delay");
    addDepth (ParamID::riserDepth,  "Riser");
    addDepth (ParamID::hpfDepth,    "High-Pass");

    // Linking. The role is a set-up choice, not a performance control, so it is
    // kept out of the host's automation lanes.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamID::role, 1 }, "Role", StringArray { "Main", "Track", "Return" }, 0,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    // How much of each effect this instance gets while it is a linked TRACK:
    // a share of the main's depth for that effect (100% = all of it, 0% = off).
    const auto addTrackLevel = [&] (const char* id, const char* name)
    {
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { id, 1 }, name,
            NormalisableRange<float> (0.0f, 1.0f), 1.0f,
            AudioParameterFloatAttributes().withStringFromValueFunction (percentFromValue)));
    };

    addTrackLevel (ParamID::trackHpf,    "Track High-Pass");
    addTrackLevel (ParamID::trackReverb, "Track Reverb");
    addTrackLevel (ParamID::trackDelay,  "Track Delay");

    return layout;
}

void PreDropAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Rebuild the riser sample for this rate first, so prepare() adopts it.
    currentSampleRate = sampleRate;
    sendRiserToEngine();
    engine.prepare (sampleRate, getTotalNumOutputChannels(), samplesPerBlock);

    // Prepare the effect plug-ins for this rate: the one the audio thread will
    // run and, if a newer one is still waiting to be picked up, that one too.
    const juce::ScopedLock lock (effectPlugins.lock);
    effectPlugins.blockSize = samplesPerBlock;
    for (auto& slot : effectPlugins.slots)
    {
        auto* running = slot.handoff.acquire();
        for (auto* fx : { running, slot.latest })
            if (fx != nullptr && ! fx->isEmpty())
                fx->prepare (sampleRate, samplesPerBlock);
    }
}

void PreDropAudioProcessor::releaseResources()
{
    // Nothing to free: the engine's buffers are sized once in prepareToPlay()
    // and reused, so there is no per-session allocation to release here.
}

bool PreDropAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainOutput = layouts.getMainOutputChannelSet();
    const auto& mainInput  = layouts.getMainInputChannelSet();

    if (mainOutput != juce::AudioChannelSet::mono()
        && mainOutput != juce::AudioChannelSet::stereo())
        return false;

    return mainInput == mainOutput;
}

void PreDropAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();

    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const auto param = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

    // A RETURN outputs only the effects of what is sent to it: the dry signal
    // already reaches the mix through each track, so Mix is fixed at fully wet.
    const auto role = getRole();
    engine.setWetOnly      (role == Role::returnTrack);
    engine.setMix          (role == Role::returnTrack ? 1.0f : param (ParamID::mix));
    engine.setOutputTrimDb (param (ParamID::outputTrim));

    if (role != Role::main)
    {
        // A linked TRACK or RETURN follows the main's Amount, and gets its own
        // share of the main's depth for each effect. The riser plays once, on
        // the main. With no main, everything ramps to zero: a track passes its
        // audio through, a return goes silent.
        const auto& main  = linkHub->mainState();
        const bool active = linkHub->linkActive();
        const auto depth  = [&] (LinkHub::Effect e)
        {
            return active ? main.depths[(size_t) e].load() * param (trackEffect (e)) : 0.0f;
        };

        engine.setAmount      (active ? main.amount.load() : 0.0f);
        engine.setHpfDepth    (depth (LinkHub::Effect::hpf));
        engine.setReverbDepth (depth (LinkHub::Effect::reverb));
        engine.setDelayDepth  (depth (LinkHub::Effect::delay));
        engine.setRiserDepth  (0.0f);
    }
    else
    {
        // With tracks or returns linked, the main leaves high-pass, reverb and
        // delay to them and adds only the riser to the mix. With nothing linked
        // (or for any other MAIN) it processes everything it receives.
        const bool tracksDoTheRest = isLinkMain() && linkHub->linkActive();
        if (isLinkMain())
            publishMainState();

        engine.setAmount      (param (ParamID::amount));
        engine.setHpfDepth    (tracksDoTheRest ? 0.0f : param (ParamID::hpfDepth));
        engine.setReverbDepth (tracksDoTheRest ? 0.0f : param (ParamID::reverbDepth));
        engine.setDelayDepth  (tracksDoTheRest ? 0.0f : param (ParamID::delayDepth));
        engine.setRiserDepth  (param (ParamID::riserDepth));
    }

    // A loaded reverb / delay plug-in takes over from the built-in one. It gets
    // the host's playhead, so tempo-synced effects follow the project's tempo
    // and transport just as they would on a track.
    const auto external = [this] (RtHandoff<HostedEffect>& handoff) -> PreDropEngine::ExternalEffect*
    {
        auto* fx = handoff.acquire();
        if (fx == nullptr || fx->isEmpty())
            return nullptr;
        fx->setPlayHead (getPlayHead());
        return fx;
    };
    engine.setExternalEffects (external (effectPlugins.slots[0].handoff), external (effectPlugins.slots[1].handoff));

    engine.process (buffer);
}

juce::AudioProcessorEditor* PreDropAudioProcessor::createEditor()
{
    return new PreDropAudioProcessorEditor (*this);   // NOSONAR(cpp:S5025): JUCE's API hands ownership of a raw pointer to the host wrapper
}

bool PreDropAudioProcessor::hasEditor() const            { return true; }

const juce::String PreDropAudioProcessor::getName() const { return JucePlugin_Name; }   // NOSONAR(cpp:S5951): signature fixed by juce::AudioProcessor

bool PreDropAudioProcessor::acceptsMidi() const          { return false; }
bool PreDropAudioProcessor::producesMidi() const         { return false; }
bool PreDropAudioProcessor::isMidiEffect() const         { return false; }
double PreDropAudioProcessor::getTailLengthSeconds() const { return 4.0; }

int PreDropAudioProcessor::getNumPrograms()              { return 1; }
int PreDropAudioProcessor::getCurrentProgram()           { return 0; }
// A single unnamed program: there are no presets to switch to or rename.
void PreDropAudioProcessor::setCurrentProgram (int)      { /* only program 0 exists */ }
const juce::String PreDropAudioProcessor::getProgramName (int) { return {}; }   // NOSONAR(cpp:S5951): signature fixed by juce::AudioProcessor
void PreDropAudioProcessor::changeProgramName (int, const juce::String&) { /* the one program has no name */ }

void PreDropAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    const juce::ScopedLock lock (riserLock);
    if (riser.fileData.getSize() > 0)
    {
        juce::ValueTree sample (riserStateTag);
        sample.setProperty ("name", riser.name, nullptr);
        sample.setProperty ("data", riser.fileData.toBase64Encoding(), nullptr);
        state.appendChild (sample, nullptr);
    }

    // Each loaded plug-in: which one it is and its own settings. A missing one
    // is written back as it was loaded, so a re-save doesn't lose it.
    {
        const juce::ScopedLock pluginLock (effectPlugins.lock);
        juce::ValueTree plugins (effectPluginsTag);
        for (int i = 0; i < (int) effectPlugins.slots.size(); ++i)
        {
            const auto& slot = effectPlugins.slots[(size_t) i];
            if (slot.latest != nullptr && ! slot.latest->isEmpty())
            {
                juce::ValueTree saved (effectSlotTag);
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

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PreDropAudioProcessor::setStateInformation (const void* data, int sizeInBytes)   // NOSONAR(cpp:S5008): signature fixed by juce::AudioProcessor
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto sample = state.getChildWithName (riserStateTag);
    const auto plugins = state.getChildWithName (effectPluginsTag);
    state.removeChild (sample, nullptr);
    state.removeChild (plugins, nullptr);
    apvts.replaceState (state);
    updateLink();   // the restored role may differ

    // Plug-ins (and any open plug-in window) may only be created and destroyed
    // on the message thread, and hosts can restore state on any thread, so the
    // slots are restored there.
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        restoreEffectPlugins (plugins);
    }
    else
    {
        {
            const juce::ScopedLock lock (effectPlugins.lock);
            effectPlugins.pendingRestore = plugins.isValid() ? plugins.createCopy() : juce::ValueTree (effectPluginsTag);
        }
        triggerAsyncUpdate();
    }

    juce::MemoryBlock fileData;
    if (sample.isValid() && fileData.fromBase64Encoding (sample["data"].toString())
        && loadRiserFromData (fileData, sample["name"].toString()).isEmpty())
        return;

    clearRiserSample();
}

// --- Riser sample ------------------------------------------------------------

bool PreDropAudioProcessor::isSupportedRiserFile (const juce::File& file) const
{
    return formats.findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

juce::String PreDropAudioProcessor::loadRiserSample (const juce::File& file)
{
    if (! file.existsAsFile())
        return "\"" + file.getFileName() + "\" could not be found.";
    if (! isSupportedRiserFile (file))
        return "\"" + file.getFileName() + "\" is not an audio file the riser can use (try WAV, AIFF or FLAC).";
    if (file.getSize() > maxRiserFileBytes)
        return "\"" + file.getFileName() + "\" is too large for a riser (64 MB max).";

    juce::MemoryBlock fileData;
    if (! file.loadFileAsData (fileData))
        return "\"" + file.getFileName() + "\" could not be read.";

    return loadRiserFromData (fileData, file.getFileName());
}

juce::String PreDropAudioProcessor::loadRiserFromData (const juce::MemoryBlock& fileData, const juce::String& name)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        formats.createReaderFor (std::make_unique<juce::MemoryInputStream> (fileData, false)));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return "\"" + name + "\" is not an audio file the riser can use (try WAV, AIFF or FLAC).";

    if ((double) reader->lengthInSamples / reader->sampleRate > maxRiserSeconds)
        return "\"" + name + "\" is longer than " + juce::String ((int) maxRiserSeconds) + " seconds.";

    RiserSource loaded;
    loaded.fileData   = fileData;
    loaded.name       = name;
    loaded.sampleRate = reader->sampleRate;
    loaded.audio.setSize ((int) juce::jmin (2u, reader->numChannels), (int) reader->lengthInSamples);
    reader->read (&loaded.audio, 0, (int) reader->lengthInSamples, 0, true, loaded.audio.getNumChannels() > 1);

    // Reject silence up front rather than leaving the riser mysteriously quiet.
    if (RiserSample::create (loaded.audio, loaded.sampleRate, loaded.sampleRate, name) == nullptr)
        return "\"" + name + "\" is silent.";

    {
        const juce::ScopedLock lock (riserLock);
        riser = std::move (loaded);
    }
    sendRiserToEngine();
    return {};
}

void PreDropAudioProcessor::clearRiserSample()
{
    {
        const juce::ScopedLock lock (riserLock);
        riser = {};
    }
    sendRiserToEngine();
}

void PreDropAudioProcessor::sendRiserToEngine()
{
    // Before prepareToPlay there is no sample rate to build for; prepareToPlay
    // calls this again once there is.
    const double rate = currentSampleRate.load();
    if (rate <= 0.0)
        return;

    const juce::ScopedLock lock (riserLock);
    auto sample = riser.audio.getNumSamples() > 0
                    ? RiserSample::create (riser.audio, riser.sampleRate, rate, riser.name)
                    : nullptr;
    engine.setRiserSample (sample != nullptr ? std::move (sample) : RiserSample::none());
}

void PreDropAudioProcessor::handleAsyncUpdate()
{
    juce::ValueTree saved;
    {
        const juce::ScopedLock lock (effectPlugins.lock);
        std::swap (saved, effectPlugins.pendingRestore);
    }
    if (saved.isValid())
        restoreEffectPlugins (saved);
}

void PreDropAudioProcessor::restoreEffectPlugins (const juce::ValueTree& saved)
{
    for (int i = 0; i < (int) effectPlugins.slots.size(); ++i)
    {
        const auto slot = saved.getChildWithProperty ("index", i);
        if (slot.isValid())
            restoreEffectPlugin ((EffectSlot) i, slot);
        else
            useBuiltInEffect ((EffectSlot) i);
    }
}

void PreDropAudioProcessor::timerCallback()
{
    engine.collectRetiredRiserSamples();
    for (auto& slot : effectPlugins.slots)
        slot.handoff.collect();
    updateLink();
}

// --- Linking -------------------------------------------------------------------

PreDropAudioProcessor::Role PreDropAudioProcessor::getRole() const noexcept
{
    return (Role) juce::jlimit (0, 2, juce::roundToInt (apvts.getRawParameterValue (ParamID::role)->load()));
}

void PreDropAudioProcessor::setRole (Role newRole)
{
    auto* p = apvts.getParameter (ParamID::role);
    p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) newRole));
    updateLink();
}

void PreDropAudioProcessor::parameterChanged (const juce::String&, float)
{
    publishMainState();   // atomic stores only: fine on whichever thread calls
}

void PreDropAudioProcessor::updateLink()
{
    const auto role = getRole();

    if (const bool track = role == Role::track; track != link.registered)
    {
        if (track) linkHub->addTrack (this);
        else       linkHub->removeTrack (this);
        link.registered = track;
    }

    if (const bool ret = role == Role::returnTrack; ret != link.registeredReturn)
    {
        if (ret) linkHub->addReturn (this);
        else     linkHub->removeReturn (this);
        link.registeredReturn = ret;
    }

    if (role == Role::main)
        linkHub->claimMain (this);   // no-op while another instance is the main
    else
        linkHub->releaseMain (this);

    publishMainState();
}

void PreDropAudioProcessor::publishMainState() noexcept
{
    if (! isLinkMain())
        return;

    auto& main = linkHub->mainState();
    const auto param = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    main.amount.store (param (ParamID::amount));
    main.depths[(size_t) LinkHub::Effect::hpf].store    (param (ParamID::hpfDepth));
    main.depths[(size_t) LinkHub::Effect::reverb].store (param (ParamID::reverbDepth));
    main.depths[(size_t) LinkHub::Effect::delay].store  (param (ParamID::delayDepth));
}

juce::String PreDropAudioProcessor::getTrackName() const
{
    if (const auto typed = apvts.state[trackNameTag].toString(); typed.isNotEmpty())
        return typed;

    {
        const juce::ScopedLock lock (link.lock);
        if (link.hostName.isNotEmpty())
            return link.hostName;
    }

    const int index = linkHub->indexOf (this);
    return index >= 0 ? "Track " + juce::String (index + 1) : juce::String ("Track");
}

void PreDropAudioProcessor::setTrackName (const juce::String& name)
{
    apvts.state.setProperty (trackNameTag, name.trim(), nullptr);
}

float PreDropAudioProcessor::getLinkLevel (LinkHub::Effect effect) const
{
    return apvts.getRawParameterValue (trackEffect (effect))->load();
}

void PreDropAudioProcessor::setLinkLevel (LinkHub::Effect effect, float level)
{
    auto* p = apvts.getParameter (trackEffect (effect));
    p->beginChangeGesture();
    p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, level));
    p->endChangeGesture();
}

void PreDropAudioProcessor::updateTrackProperties (const TrackProperties& properties)
{
    const juce::ScopedLock lock (link.lock);
    link.hostName = properties.name;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PreDropAudioProcessor();   // NOSONAR(cpp:S5025): JUCE's plugin entry point returns an owning raw pointer
}

// --- Effect plug-ins -------------------------------------------------------------

juce::String PreDropAudioProcessor::loadEffectPlugin (EffectSlot slot, const juce::File& plugin)
{
    if (! plugin.exists())
        return "\"" + plugin.getFileName() + "\" could not be found.";

    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : effectPlugins.formats.getFormats())
        if (format->fileMightContainThisPluginType (plugin.getFullPathName()))
            format->findAllTypesForFile (types, plugin.getFullPathName());

    if (types.isEmpty())
        return "\"" + plugin.getFileName() + "\" couldn't be read as a VST3 plug-in.";

    for (const auto* type : types)
        if (! type->isInstrument)
            return installEffectPlugin (slot, *type, nullptr);

    return "\"" + types[0]->name + "\" is an instrument; choose an effect.";
}

juce::String PreDropAudioProcessor::installEffectPlugin (EffectSlot slot, const juce::PluginDescription& description,
                                                         const juce::MemoryBlock* pluginState)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const double rate = currentSampleRate.load() > 0.0 ? currentSampleRate.load() : 44100.0;
    juce::String error;
    auto instance = effectPlugins.formats.createPluginInstance (description, rate, effectPlugins.blockSize, error);
    if (instance == nullptr)
        return "\"" + description.name + "\" couldn't be opened" + (error.isNotEmpty() ? ": " + error : juce::String ("."));

    // Stereo in and out, where the plug-in allows it (set before preparing).
    juce::AudioProcessor::BusesLayout stereo;
    stereo.inputBuses.add (juce::AudioChannelSet::stereo());
    stereo.outputBuses.add (juce::AudioChannelSet::stereo());
    if (instance->checkBusesLayoutSupported (stereo))
        instance->setBusesLayout (stereo);

    if (pluginState != nullptr && pluginState->getSize() > 0)
        instance->setStateInformation (pluginState->getData(), (int) pluginState->getSize());

    auto effect = std::make_unique<HostedEffect> (std::move (instance), description);

    const juce::ScopedLock lock (effectPlugins.lock);
    if (currentSampleRate.load() > 0.0)
        effect->prepare (currentSampleRate.load(), effectPlugins.blockSize);   // ready before the audio thread sees it

    auto& state = effectPlugins.slots[(size_t) slot];
    state.window.reset();   // its editor belongs to the plug-in being replaced
    state.latest  = effect.get();
    state.name    = description.name;
    state.missing = {};
    state.handoff.send (std::move (effect));
    return {};
}

void PreDropAudioProcessor::restoreEffectPlugin (EffectSlot slot, const juce::ValueTree& saved)
{
    juce::PluginDescription description;
    if (auto xml = juce::parseXML (saved["description"].toString()); xml == nullptr || ! description.loadFromXml (*xml))
    {
        useBuiltInEffect (slot);
        return;
    }

    // Where it was saved, or a plug-in of the same name in this computer's folders.
    if (! juce::File::isAbsolutePath (description.fileOrIdentifier) || ! juce::File (description.fileOrIdentifier).exists())
    {
        const auto fileName = juce::File::createFileWithoutCheckingPath (description.fileOrIdentifier).getFileName();
        for (const auto& candidate : findInstalledVst3Plugins())
            if (candidate.getFileName() == fileName)
                description.fileOrIdentifier = candidate.getFullPathName();
    }

    juce::MemoryBlock pluginState;
    pluginState.fromBase64Encoding (saved["state"].toString());

    if (! juce::File::isAbsolutePath (description.fileOrIdentifier)
        || ! juce::File (description.fileOrIdentifier).exists()
        || installEffectPlugin (slot, description, &pluginState).isNotEmpty())
    {
        useBuiltInEffect (slot);
        const juce::ScopedLock lock (effectPlugins.lock);
        effectPlugins.slots[(size_t) slot].missing = saved.createCopy();
    }
}

void PreDropAudioProcessor::useBuiltInEffect (EffectSlot slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto none = std::make_unique<HostedEffect>();

    const juce::ScopedLock lock (effectPlugins.lock);
    auto& state = effectPlugins.slots[(size_t) slot];
    state.window.reset();
    state.latest  = none.get();
    state.name    = {};
    state.missing = {};
    state.handoff.send (std::move (none));
}

juce::String PreDropAudioProcessor::getEffectPluginName (EffectSlot slot) const
{
    const juce::ScopedLock lock (effectPlugins.lock);
    return effectPlugins.slots[(size_t) slot].name;
}

juce::String PreDropAudioProcessor::getMissingEffectPluginName (EffectSlot slot) const
{
    const juce::ScopedLock lock (effectPlugins.lock);
    const auto& missing = effectPlugins.slots[(size_t) slot].missing;
    if (! missing.isValid())
        return {};

    juce::PluginDescription description;
    if (auto xml = juce::parseXML (missing["description"].toString()); xml != nullptr && description.loadFromXml (*xml))
        return description.name;
    return "a plug-in";
}

bool PreDropAudioProcessor::effectPluginLeaksDry (EffectSlot slot) const
{
    const juce::ScopedLock lock (effectPlugins.lock);
    const auto* fx = effectPlugins.slots[(size_t) slot].latest;
    return fx != nullptr && ! fx->isEmpty() && fx->dryLeak() > HostedEffect::dryLeakWarning;
}

void PreDropAudioProcessor::showEffectPluginWindow (EffectSlot slot)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto& state = effectPlugins.slots[(size_t) slot];
    if (state.latest == nullptr || state.latest->isEmpty())
        return;

    if (state.window == nullptr)
    {
        const juce::String title = state.name + (slot == EffectSlot::reverb ? " (PreDrop reverb)" : " (PreDrop delay)");
        auto* owner = &state;
        state.window = std::make_unique<EffectPluginWindow> (*state.latest->getPlugin(), title,
                                                             [owner] { owner->window->setVisible (false); });
    }

    state.window->setVisible (true);
    state.window->toFront (true);
}

juce::AudioPluginInstance* PreDropAudioProcessor::getEffectPluginInstance (EffectSlot slot) const
{
    const juce::ScopedLock lock (effectPlugins.lock);
    const auto* fx = effectPlugins.slots[(size_t) slot].latest;
    return fx != nullptr ? fx->getPlugin() : nullptr;
}

void PreDropAudioProcessor::closeEffectPluginWindows()
{
    JUCE_ASSERT_MESSAGE_THREAD
    for (auto& slot : effectPlugins.slots)
        slot.window.reset();
}

juce::Array<juce::File> PreDropAudioProcessor::findInstalledVst3Plugins()
{
    // The standard VST3 folders, and one level of vendor sub-folders in them.
    // Only names are listed: nothing is loaded until one is chosen.
    juce::Array<juce::File> found;
    const auto add = [&found] (const juce::File& f)
    {
        if (f.hasFileExtension ("vst3") && ! f.getFileNameWithoutExtension().equalsIgnoreCase (JucePlugin_Name))
            found.addIfNotAlreadyThere (f);
    };

    const auto folders = juce::VST3PluginFormat().getDefaultLocationsToSearch();
    for (int i = 0; i < folders.getNumPaths(); ++i)
        for (const auto& entry : juce::RangedDirectoryIterator (folders[i], false, "*", juce::File::findFilesAndDirectories))
        {
            const auto f = entry.getFile();
            if (f.hasFileExtension ("vst3"))
                add (f);
            else if (f.isDirectory())
                for (const auto& inner : juce::RangedDirectoryIterator (f, false, "*.vst3", juce::File::findFilesAndDirectories))
                    add (inner.getFile());
        }

    std::sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileNameWithoutExtension().compareIgnoreCase (b.getFileNameWithoutExtension()) < 0;
    });
    return found;
}
