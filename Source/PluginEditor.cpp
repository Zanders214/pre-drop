#include "PluginEditor.h"

PreDropAudioProcessorEditor::PreDropAudioProcessorEditor (PreDropAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    auto setupRotary = [this] (juce::Slider& slider)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 20);
        addAndMakeVisible (slider);
    };

    setupRotary (amountSlider);
    setupRotary (mixSlider);
    setupRotary (trimSlider);

    titleLabel.setText ("PRE-DROP", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centred);
    titleLabel.setFont (juce::Font (juce::FontOptions().withHeight (28.0f).withStyle ("Bold")));
    addAndMakeVisible (titleLabel);

    auto setupLabel = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (label);
    };

    setupLabel (amountLabel, "Amount");
    setupLabel (mixLabel, "Mix");
    setupLabel (trimLabel, "Output");

    amountAttachment = std::make_unique<SliderAttachment> (processorRef.apvts, "amount", amountSlider);
    mixAttachment    = std::make_unique<SliderAttachment> (processorRef.apvts, "mix", mixSlider);
    trimAttachment   = std::make_unique<SliderAttachment> (processorRef.apvts, "outputTrim", trimSlider);

    setSize (360, 420);
}

PreDropAudioProcessorEditor::~PreDropAudioProcessorEditor() = default;

void PreDropAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1b1b22));
}

void PreDropAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (20);

    titleLabel.setBounds (area.removeFromTop (40));

    auto bottom = area.removeFromBottom (130);

    amountLabel.setBounds (area.removeFromTop (24));
    amountSlider.setBounds (area.reduced (10));

    const auto half = bottom.getWidth() / 2;
    auto left  = bottom.removeFromLeft (half);
    auto right = bottom;

    mixLabel.setBounds (left.removeFromTop (20));
    mixSlider.setBounds (left);
    trimLabel.setBounds (right.removeFromTop (20));
    trimSlider.setBounds (right);
}
