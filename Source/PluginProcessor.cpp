#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace ParamID
{
    static constexpr const char* amount     = "amount";
    static constexpr const char* mix        = "mix";
    static constexpr const char* outputTrim = "outputTrim";
}

PreDropAudioProcessor::PreDropAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

PreDropAudioProcessor::~PreDropAudioProcessor() = default;

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

    return layout;
}

void PreDropAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, getTotalNumOutputChannels(), samplesPerBlock);
}

void PreDropAudioProcessor::releaseResources()
{
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

    engine.process (buffer);
}

juce::AudioProcessorEditor* PreDropAudioProcessor::createEditor()
{
    return new PreDropAudioProcessorEditor (*this);
}

bool PreDropAudioProcessor::hasEditor() const            { return true; }

const juce::String PreDropAudioProcessor::getName() const { return JucePlugin_Name; }

bool PreDropAudioProcessor::acceptsMidi() const          { return false; }
bool PreDropAudioProcessor::producesMidi() const         { return false; }
bool PreDropAudioProcessor::isMidiEffect() const         { return false; }
double PreDropAudioProcessor::getTailLengthSeconds() const { return 4.0; }

int PreDropAudioProcessor::getNumPrograms()              { return 1; }
int PreDropAudioProcessor::getCurrentProgram()           { return 0; }
void PreDropAudioProcessor::setCurrentProgram (int)      {}
const juce::String PreDropAudioProcessor::getProgramName (int) { return {}; }
void PreDropAudioProcessor::changeProgramName (int, const juce::String&) {}

void PreDropAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void PreDropAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PreDropAudioProcessor();
}
