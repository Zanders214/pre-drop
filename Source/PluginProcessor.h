#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "EffectPlugins.h"
#include "LinkHub.h"
#include "PreDropEngine.h"
#include "RiserSource.h"
#include "TrackLink.h"

/**
    PreDropAudioProcessor

    Thin host wrapper around PreDropEngine. It owns the parameter state and feeds
    the engine's macro controls each block; all DSP lives in PreDropEngine.

    Three parts sit beside the engine, each in its own class:
      - the riser sample, saved inside the plugin state (RiserSource);
      - this instance's place in the track link: MAIN, TRACK or RETURN (TrackLink);
      - the optional VST3 reverb and delay (EffectPlugins).
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

    void updateTrackProperties (const TrackProperties& properties) override;

    // The parts beside the engine (see the class comment).
    using Role       = TrackLink::Role;
    using EffectSlot = EffectPlugins::Slot;

    RiserSource&     getRiser() noexcept         { return riser; }
    TrackLink&       getLink() noexcept          { return link; }
    const TrackLink& getLink() const noexcept    { return link; }
    EffectPlugins&   getEffectPlugins() noexcept { return effectPlugins; }

    /** Where the riser sample is in its loop (0..1), or -1 while it isn't playing. */
    float getRiserPlayhead() const noexcept { return engine.getRiserPlayhead(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void timerCallback() override;
    PreDropEngine::Transport readTransport() const;

    juce::AudioProcessorValueTreeState apvts;
    PreDropEngine engine;
    RiserSource   riser { engine };
    EffectPlugins effectPlugins;
    TrackLink     link { apvts };   // last: leaves the link before the rest goes

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropAudioProcessor)
};
