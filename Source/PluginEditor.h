#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

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
    void paintHeader (juce::Graphics&, float scale) const;
    void paintSliderLabels (juce::Graphics&, float scale) const;
    void paintDepthPanel (juce::Graphics&, float scale) const;
    void setDepthPanelOpen (bool shouldBeOpen);

    PreDropAudioProcessor& processorRef;
    PreDropLookAndFeel     lnf;
    DepthKnobLookAndFeel   depthLnf;

    juce::Slider amountSlider;
    juce::Slider mixSlider;
    juce::Slider trimSlider;
    juce::TextButton effectsButton;
    EffectChips  effectChips;
    BuildUpCurve buildUpCurve;

    // Per-effect depth knobs, revealed by the EFFECTS button. Index order is
    // Reverb, Delay, Riser, High-Pass (matches depthParamIds / depthNames below).
    static constexpr int numDepths = 4;
    std::array<juce::Slider, numDepths> depthSliders;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<SliderAttachment> amountAttachment;
    std::unique_ptr<SliderAttachment> mixAttachment;
    std::unique_ptr<SliderAttachment> trimAttachment;
    std::array<std::unique_ptr<SliderAttachment>, numDepths> depthAttachments;

    // Painted regions computed by resized() and read back by paint().
    struct Layout
    {
        juce::Rectangle<int> header;
        juce::Rectangle<int> mixLabel;
        juce::Rectangle<int> trimLabel;
        juce::Rectangle<int> reveal;                                 // shared by curve / depth panel
        std::array<juce::Rectangle<int>, numDepths> depthKnobs;      // used when the panel is open
        int sliderSeparatorY = 0;
    };
    Layout layout;

    bool  depthPanelOpen = false;
    float uiScale = 1.0f;

    // Cached parameter values so the live repaint only fires on change.
    struct LastValues
    {
        float amount = -1.0f;
        float mix    = -1.0f;
        float trim   = -999.0f;
        std::array<float, numDepths> depths { -1.0f, -1.0f, -1.0f, -1.0f };
    };
    LastValues last;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropAudioProcessorEditor)
};
