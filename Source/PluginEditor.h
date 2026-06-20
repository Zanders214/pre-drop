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
    void paintDepthPanel (juce::Graphics&, float scale);
    void setDepthPanelOpen (bool shouldBeOpen);

    PreDropAudioProcessor& processorRef;
    PreDropLookAndFeel     lnf;
    DepthKnobLookAndFeel   depthLnf;

    juce::Slider amountSlider, mixSlider, trimSlider;
    juce::TextButton effectsButton;
    EffectChips  effectChips;
    BuildUpCurve buildUpCurve;

    // Per-effect depth knobs, revealed by the EFFECTS button. Index order is
    // Reverb, Delay, Riser, High-Pass (matches depthParamIds / depthNames below).
    static constexpr int numDepths = 4;
    juce::Slider depthSliders[numDepths];

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<SliderAttachment> amountAttachment, mixAttachment, trimAttachment;
    std::unique_ptr<SliderAttachment> depthAttachments[numDepths];

    juce::Rectangle<int> headerArea, mixLabelArea, trimLabelArea;
    juce::Rectangle<int> revealArea;                 // area shared by curve / depth panel
    juce::Rectangle<int> depthKnobAreas[numDepths];  // laid out when the panel is open
    bool  depthPanelOpen   = false;
    int   sliderSeparatorY = 0;
    float uiScale = 1.0f;

    // Cached parameter values so the live repaint only fires on change.
    float lastAmount = -1.0f, lastMix = -1.0f, lastTrim = -999.0f;
    float lastDepths[numDepths] = { -1.0f, -1.0f, -1.0f, -1.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropAudioProcessorEditor)
};
