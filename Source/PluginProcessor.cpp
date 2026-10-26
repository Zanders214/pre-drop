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
    static constexpr const char* scope       = "scope";
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

    // The main's controls that linked tracks follow.
    constexpr std::array<const char*, 5> linkedParameters { ParamID::amount, ParamID::hpfDepth, ParamID::reverbDepth,
                                                            ParamID::delayDepth, ParamID::scope };

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

    // Publish the main's controls the moment they change (from the UI, the
    // host's automation, or a state load), not only when it next processes.
    for (auto* id : linkedParameters)
        apvts.addParameterListener (id, this);

    updateLink();
    startTimerHz (10);   // keeps the link current; frees swapped-out riser samples
}

PreDropAudioProcessor::~PreDropAudioProcessor()
{
    stopTimer();
    for (auto* id : linkedParameters)
        apvts.removeParameterListener (id, this);
    if (link.registered)
        linkHub->removeTrack (this);
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

    // Linking. Role and scope are set-up choices, not performance controls, so
    // they are kept out of the host's automation lanes.
    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamID::role, 1 }, "Role", StringArray { "Main", "Track" }, 0,
        AudioParameterChoiceAttributes().withAutomatable (false)));

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { ParamID::scope, 1 }, "Applies To", StringArray { "All tracks", "Selected tracks" }, 0,
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

    // Mix and output trim are always this instance's own.
    engine.setMix          (param (ParamID::mix));
    engine.setOutputTrimDb (param (ParamID::outputTrim));

    if (getRole() == Role::track)
    {
        // A linked track follows the main's Amount, and gets its own share of
        // the main's depth for each effect. The riser plays once, on the main.
        // With no main, or the main applying to the whole mix, everything ramps
        // to zero so the track passes its audio through.
        const auto& main  = linkHub->mainState();
        const bool active = linkHub->tracksActive();
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
        // The main applying to selected tracks leaves high-pass, reverb and
        // delay to them and adds only the riser to the mix. Any other MAIN
        // (or this one on ALL TRACKS) processes everything it receives.
        const bool tracksDoTheRest = isLinkMain() && appliesToSelectedTracks();
        if (isLinkMain())
            publishMainState();

        engine.setAmount      (param (ParamID::amount));
        engine.setHpfDepth    (tracksDoTheRest ? 0.0f : param (ParamID::hpfDepth));
        engine.setReverbDepth (tracksDoTheRest ? 0.0f : param (ParamID::reverbDepth));
        engine.setDelayDepth  (tracksDoTheRest ? 0.0f : param (ParamID::delayDepth));
        engine.setRiserDepth  (param (ParamID::riserDepth));
    }

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
    state.removeChild (sample, nullptr);
    apvts.replaceState (state);
    updateLink();   // the restored role may differ

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

void PreDropAudioProcessor::timerCallback()
{
    engine.collectRetiredRiserSamples();
    updateLink();
}

// --- Linking -------------------------------------------------------------------

PreDropAudioProcessor::Role PreDropAudioProcessor::getRole() const noexcept
{
    return apvts.getRawParameterValue (ParamID::role)->load() > 0.5f ? Role::track : Role::main;
}

void PreDropAudioProcessor::setRole (Role newRole)
{
    auto* p = apvts.getParameter (ParamID::role);
    p->setValueNotifyingHost (p->convertTo0to1 ((float) (int) newRole));
    updateLink();
}

bool PreDropAudioProcessor::appliesToSelectedTracks() const noexcept
{
    return apvts.getRawParameterValue (ParamID::scope)->load() > 0.5f;
}

void PreDropAudioProcessor::setAppliesToSelectedTracks (bool selectedOnly)
{
    auto* p = apvts.getParameter (ParamID::scope);
    p->setValueNotifyingHost (p->convertTo0to1 (selectedOnly ? 1.0f : 0.0f));
    publishMainState();
}

void PreDropAudioProcessor::parameterChanged (const juce::String&, float)
{
    publishMainState();   // atomic stores only: fine on whichever thread calls
}

void PreDropAudioProcessor::updateLink()
{
    if (getRole() == Role::track)
    {
        linkHub->releaseMain (this);
        if (! link.registered)
        {
            linkHub->addTrack (this);
            link.registered = true;
        }
    }
    else
    {
        if (link.registered)
        {
            linkHub->removeTrack (this);
            link.registered = false;
        }
        linkHub->claimMain (this);   // no-op while another instance is the main
    }

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
    main.selectedTracksOnly.store (appliesToSelectedTracks());
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
