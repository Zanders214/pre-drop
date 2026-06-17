#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

/**
    PreDropAudioProcessorEditor

    Minimal one-knob face: a large "Amount" macro dial driving the whole build-up,
    with small Mix and Output trims underneath.
*/
class PreDropAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit PreDropAudioProcessorEditor (PreDropAudioProcessor&);
    ~PreDropAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    PreDropAudioProcessor& processorRef;

    juce::Slider amountSlider, mixSlider, trimSlider;
    juce::Label  titleLabel, amountLabel, mixLabel, trimLabel;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<SliderAttachment> amountAttachment, mixAttachment, trimAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropAudioProcessorEditor)
};
