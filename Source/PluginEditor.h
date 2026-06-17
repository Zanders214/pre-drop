#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "PreDropLookAndFeel.h"
#include "EffectChips.h"
#include "BuildUpCurve.h"

/**
    PreDropAudioProcessorEditor — "Direction D — Neon Clean".

    A dark panel driven by one Amount macro: a multicolour progress-ring knob with
    a centred readout, four effect status chips, a live build-up energy histogram
    (itself a second handle on Amount), and Mix / Output sliders. Everything is
    drawn natively (no WebView) and scales proportionally from the 360 px design.
*/
class PreDropAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit PreDropAudioProcessorEditor (PreDropAudioProcessor&);
    ~PreDropAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void paintHeader (juce::Graphics&, float scale);
    void paintSliderLabels (juce::Graphics&, float scale);

    PreDropAudioProcessor& processorRef;
    PreDropLookAndFeel     lnf;

    juce::Slider amountSlider, mixSlider, trimSlider;
    EffectChips  effectChips;
    BuildUpCurve buildUpCurve;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<SliderAttachment> amountAttachment, mixAttachment, trimAttachment;

    juce::Rectangle<int> headerArea, mixLabelArea, trimLabelArea;
    int   sliderSeparatorY = 0;
    float uiScale = 1.0f;

    // Cached parameter values so the live repaint only fires on change.
    float lastAmount = -1.0f, lastMix = -1.0f, lastTrim = -999.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropAudioProcessorEditor)
};
