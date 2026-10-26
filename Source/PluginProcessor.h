#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "PreDropEngine.h"

/**
    PreDropAudioProcessor

    Thin host wrapper around PreDropEngine. It owns the parameter state and feeds
    the engine's macro controls each block; all DSP lives in PreDropEngine.

    It also owns the optional riser sample: the original file's bytes are kept
    and saved inside the plugin state, so a project carries its riser with it,
    and the decoded audio is rebuilt for the engine at the host's sample rate.
*/
class PreDropAudioProcessor : public juce::AudioProcessor,
                              private juce::Timer
{
public:
    PreDropAudioProcessor();
    ~PreDropAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;   // keep the double-precision overload visible

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState&       getState() noexcept       { return apvts; }
    const juce::AudioProcessorValueTreeState& getState() const noexcept { return apvts; }

    // --- Riser sample (message thread) ---------------------------------------
    /** Uses an audio file as the riser instead of the built-in noise. Returns an
        explanation if it can't be used, or an empty string on success. */
    juce::String loadRiserSample (const juce::File& file);

    /** Goes back to the built-in noise riser. */
    void clearRiserSample();

    /** File name of the riser sample, or empty while the built-in noise is used. */
    juce::String getRiserSampleName() const
    {
        const juce::ScopedLock lock (riserLock);
        return riser.name;
    }

    /** True for files the riser can load, judged by extension. */
    bool isSupportedRiserFile (const juce::File& file) const;

    /** Wildcard of every loadable extension, for a file chooser. */
    juce::String getRiserFileWildcard() const { return formats.getWildcardForAllFormats(); }

    static constexpr double maxRiserSeconds = 60.0;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void timerCallback() override;
    juce::String loadRiserFromData (const juce::MemoryBlock& fileData, const juce::String& name);
    void sendRiserToEngine();

    juce::AudioProcessorValueTreeState apvts;
    PreDropEngine engine;

    juce::AudioFormatManager formats;
    std::atomic<double> currentSampleRate { 0.0 };   // 0 until prepareToPlay

    // The loaded riser: original file bytes (what the plugin state saves), name,
    // and decoded audio at the file's own sample rate. Guarded by riserLock,
    // since hosts may call prepareToPlay / get/setStateInformation off the
    // message thread; never touched by the audio thread.
    juce::CriticalSection riserLock;
    struct RiserSource
    {
        juce::MemoryBlock        fileData;
        juce::String             name;
        juce::AudioBuffer<float> audio;
        double                   sampleRate = 0.0;
    };
    RiserSource riser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropAudioProcessor)
};
