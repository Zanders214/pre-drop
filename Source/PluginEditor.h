#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>

#include "PluginProcessor.h"
#include "PreDropLookAndFeel.h"
#include "EffectChips.h"
#include "BuildUpCurve.h"
#include "TrackMatrix.h"
#include "TrackPanel.h"

/**
    PreDropAudioProcessorEditor — "Direction D — Neon Clean".

    A dark panel driven by one Amount macro: a multicolour progress-ring knob with
    a centred readout, four effect status chips, a live build-up energy histogram
    (itself a second handle on Amount), and Mix / Output sliders. Everything is
    drawn natively (no WebView) and scales proportionally from the 360 px design.

    The header's MAIN / TRACK pill switches the instance's role. As MAIN, the
    slot under the chips shows the curve, the EFFECTS depth knobs or the TRACKS
    panel; as TRACK it shows the track's link status and effect switches, and
    the knob and chips mirror the main's Amount and depths.
*/
class PreDropAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    public juce::FileDragAndDropTarget,
                                    private juce::Timer
{
public:
    explicit PreDropAudioProcessorEditor (PreDropAudioProcessor&);
    ~PreDropAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    // Drop an audio file anywhere on the editor to use it as the riser.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void showRiserMenu();
    void loadRiser (const juce::File& file);
    void updateRiserButton();
    void fitRiserLabel();
    void paintHeader (juce::Graphics&, float scale) const;
    void paintSliderLabels (juce::Graphics&, float scale) const;
    void paintDepthPanel (juce::Graphics&, float scale) const;

    // What fills the slot under the chips while this instance is MAIN.
    enum class Reveal { curve, effects, tracks };
    void setReveal (Reveal newReveal);

    // Linking. "Track mode" covers TRACK and RETURN: both follow the main.
    bool isTrackMode() const noexcept;
    bool isReturnMode() const noexcept;
    void showRoleMenu();
    void applyRole();
    float displayedAmount() const;                 // the main's, as a TRACK
    float displayedDepth (const char* depthId) const;

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

    Reveal reveal  = Reveal::curve;
    float  uiScale = 1.0f;

    // Cached parameter values so the live repaint only fires on change.
    struct LastValues
    {
        float amount = -1.0f;
        float mix    = -1.0f;
        float trim   = -999.0f;
        std::array<float, numDepths> depths { -1.0f, -1.0f, -1.0f, -1.0f };
    };
    LastValues last;

    // The RISER pill beside EFFECTS: shows the sample in use, opens the
    // load / back-to-noise menu, and owns the file chooser while it is open.
    struct RiserControl
    {
        juce::TextButton                   button;
        std::unique_ptr<juce::FileChooser> chooser;
        juce::String                       shownName { "?" };   // forces the first update
        bool                               dragActive = false;
    };
    RiserControl riserUi;

    // The MAIN / TRACK pill, the main's TRACKS button and panel, and the panel
    // a TRACK shows instead.
    struct LinkControls
    {
        explicit LinkControls (PreDropAudioProcessor& p) : matrix (p), panel (p) {}

        juce::TextButton roleButton;
        juce::TextButton tracksButton;
        TrackMatrix      matrix;
        TrackPanel       panel;
        int              shownRole = -1;   // forces the first applyRole
    };
    LinkControls link;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropAudioProcessorEditor)
};
