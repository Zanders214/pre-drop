#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ParamIDs.h"

PreDropAudioProcessor::PreDropAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    startTimerHz (10);   // keeps the link current; frees swapped-out riser samples and plug-ins
}

PreDropAudioProcessor::~PreDropAudioProcessor()
{
    stopTimer();
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
    riser.setSampleRate (sampleRate);
    engine.prepare (sampleRate, getTotalNumOutputChannels(), samplesPerBlock);
    effectPlugins.prepare (sampleRate, samplesPerBlock);
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
    const auto role = link.getRole();
    engine.setWetOnly      (role == Role::returnTrack);
    engine.setMix          (role == Role::returnTrack ? 1.0f : param (ParamID::mix));
    engine.setOutputTrimDb (param (ParamID::outputTrim));

    const auto& hub = link.getHub();
    if (role != Role::main)
    {
        // A linked TRACK or RETURN follows the main's Amount, and gets its own
        // share of the main's depth for each effect. The riser plays once, on
        // the main. With no main, everything ramps to zero: a track passes its
        // audio through, a return goes silent.
        const auto& main  = hub.mainState();
        const bool active = hub.linkActive();
        const auto depth  = [&] (LinkHub::Effect e)
        {
            return active ? main.depths[(size_t) e].load() * param (ParamID::trackLevel (e)) : 0.0f;
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
        const bool tracksDoTheRest = link.isMain() && hub.linkActive();
        link.publishMainState();

        engine.setAmount      (param (ParamID::amount));
        engine.setHpfDepth    (tracksDoTheRest ? 0.0f : param (ParamID::hpfDepth));
        engine.setReverbDepth (tracksDoTheRest ? 0.0f : param (ParamID::reverbDepth));
        engine.setDelayDepth  (tracksDoTheRest ? 0.0f : param (ParamID::delayDepth));
        engine.setRiserDepth  (param (ParamID::riserDepth));
    }

    // Where the host's transport is: a riser sample that follows the project
    // plays at its tempo and starts on its bar lines.
    const auto transport = readTransport();
    engine.setTransport (transport);
    riser.setHostTempo (transport.bpm);
    if (isNonRealtime())
        riser.renderIfStale();   // a bounce can't wait for a background re-render

    // A loaded reverb / delay plug-in takes over from the built-in one.
    engine.setExternalEffects (effectPlugins.acquire (EffectSlot::reverb, getPlayHead()),
                               effectPlugins.acquire (EffectSlot::delay,  getPlayHead()));

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
    riser.saveTo (state);
    effectPlugins.saveTo (state);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PreDropAudioProcessor::setStateInformation (const void* data, int sizeInBytes)   // NOSONAR(cpp:S5008): signature fixed by juce::AudioProcessor
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    const auto saved = juce::ValueTree::fromXml (*xml);
    auto parameters = saved.createCopy();
    parameters.removeChild (parameters.getChildWithName (RiserSource::stateTag), nullptr);
    parameters.removeChild (parameters.getChildWithName (EffectPlugins::stateTag), nullptr);
    apvts.replaceState (parameters);
    link.update();   // the restored role may differ

    effectPlugins.restoreFrom (saved);
    riser.restoreFrom (saved);
}

void PreDropAudioProcessor::updateTrackProperties (const TrackProperties& properties)
{
    link.setHostTrackName (properties.name);
}

PreDropEngine::Transport PreDropAudioProcessor::readTransport() const
{
    PreDropEngine::Transport transport;
    const auto* playHead = getPlayHead();
    const auto position = playHead != nullptr ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo>();
    if (! position.hasValue())
        return transport;

    transport.playing = position->getIsPlaying();
    transport.bpm     = position->getBpm().orFallback (0.0);
    transport.ppq     = position->getPpqPosition().orFallback (0.0);

    // Bars in the host's meter, counted from its last bar line (or from the
    // start, if it doesn't say where that was).
    if (const auto meter = position->getTimeSignature(); meter.hasValue() && meter->numerator > 0 && meter->denominator > 0)
        transport.barLength = meter->numerator * 4.0 / meter->denominator;
    transport.barStart = position->getPpqPositionOfLastBarStart()
                             .orFallback (std::floor (transport.ppq / transport.barLength) * transport.barLength);
    return transport;
}

void PreDropAudioProcessor::timerCallback()
{
    engine.collectRetiredRiserSamples();
    effectPlugins.collect();
    link.update();
    riser.update();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PreDropAudioProcessor();   // NOSONAR(cpp:S5025): JUCE's plugin entry point returns an owning raw pointer
}
