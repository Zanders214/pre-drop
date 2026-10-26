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
}

namespace
{
    // Child of the saved state that carries the riser sample, if one is loaded.
    const juce::Identifier riserStateTag { "RISER_SAMPLE" };

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
    startTimerHz (4);   // frees riser samples the audio thread has swapped out
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

    engine.setAmount       (apvts.getRawParameterValue (ParamID::amount)->load());
    engine.setMix          (apvts.getRawParameterValue (ParamID::mix)->load());
    engine.setOutputTrimDb (apvts.getRawParameterValue (ParamID::outputTrim)->load());
    engine.setReverbDepth  (apvts.getRawParameterValue (ParamID::reverbDepth)->load());
    engine.setDelayDepth   (apvts.getRawParameterValue (ParamID::delayDepth)->load());
    engine.setRiserDepth   (apvts.getRawParameterValue (ParamID::riserDepth)->load());
    engine.setHpfDepth     (apvts.getRawParameterValue (ParamID::hpfDepth)->load());

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
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PreDropAudioProcessor();   // NOSONAR(cpp:S5025): JUCE's plugin entry point returns an owning raw pointer
}
