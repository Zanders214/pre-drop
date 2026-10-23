#include "PluginEditor.h"
#include "PreDropTheme.h"
#include "PreDropVisualModel.h"

#include <array>
#include <cmath>
#include <utility>

using namespace PreDrop;

namespace
{
    constexpr float kDesignWidth  = 360.0f;
    constexpr float kDesignHeight = 540.0f;
    constexpr float kPanelPad     = 26.0f;

    float textWidth (const juce::Font& font, const juce::String& text)
    {
        juce::GlyphArrangement arrangement;
        arrangement.addLineOfText (font, text, 0.0f, 0.0f);
        return arrangement.getBoundingBox (0, -1, true).getWidth();
    }

    float paramValue (const PreDropAudioProcessor& p, const char* id) { return p.getState().getRawParameterValue (id)->load(); }

    // Per-effect depth knobs, in display order. IDs match PluginProcessor's
    // ParamID namespace; colours match the effect chips.
    constexpr std::array<const char*, 4> depthParamIds { "reverbDepth", "delayDepth", "riserDepth", "hpfDepth" };
    constexpr std::array<const char*, 4> depthNames    { "REVERB", "DELAY", "RISER", "HIGH-PASS" };
    juce::Colour depthColour (int i)
    {
        using namespace PreDrop::Palette;
        const std::array<juce::Colour, 4> cols { violet, pink, amber, cyan };
        return cols[(size_t) juce::jlimit (0, 3, i)];
    }
}

PreDropAudioProcessorEditor::PreDropAudioProcessorEditor (PreDropAudioProcessor& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      buildUpCurve (p.getState(), "amount")
{
    // --- Amount knob ---------------------------------------------------------
    amountSlider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    amountSlider.setRotaryParameters (juce::degreesToRadians (-135.0f),
                                      juce::degreesToRadians (135.0f), true);
    amountSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    amountSlider.setMouseDragSensitivity (220);   // ~220 px of travel for full range
    const float defaultAmount = p.getState().getParameterRange ("amount")
                                    .convertFrom0to1 (p.getState().getParameter ("amount")->getDefaultValue());
    amountSlider.setDoubleClickReturnValue (true, defaultAmount);
    amountSlider.setLookAndFeel (&lnf);
    addAndMakeVisible (amountSlider);

    // --- Mix / Output sliders ------------------------------------------------
    for (auto* sl : { &mixSlider, &trimSlider })
    {
        sl->setSliderStyle (juce::Slider::LinearHorizontal);
        sl->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sl->setLookAndFeel (&lnf);
        addAndMakeVisible (*sl);
    }

    // --- Per-effect depth knobs (hidden until the EFFECTS button is pressed) --
    for (int i = 0; i < numDepths; ++i)
    {
        const auto idx = (size_t) i;
        auto& sl = depthSliders[idx];
        sl.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        sl.setRotaryParameters (juce::degreesToRadians (-135.0f),
                                juce::degreesToRadians (135.0f), true);
        sl.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sl.setColour (juce::Slider::rotarySliderFillColourId, depthColour (i));
        sl.setLookAndFeel (&depthLnf);
        const float def = p.getState().getParameterRange (depthParamIds[idx])
                              .convertFrom0to1 (p.getState().getParameter (depthParamIds[idx])->getDefaultValue());
        sl.setDoubleClickReturnValue (true, def);
        sl.onValueChange = [this] { repaint(); };
        addChildComponent (sl);   // added but not visible until the panel opens
        depthAttachments[idx] = std::make_unique<SliderAttachment> (p.getState(), depthParamIds[idx], sl);
    }

    // --- EFFECTS reveal toggle -----------------------------------------------
    effectsButton.setButtonText ("EFFECTS");
    effectsButton.setClickingTogglesState (true);
    effectsButton.setLookAndFeel (&lnf);
    effectsButton.onClick = [this] { setDepthPanelOpen (effectsButton.getToggleState()); };
    addAndMakeVisible (effectsButton);

    // --- Indicators ----------------------------------------------------------
    effectChips.setAmountSource ([this] { return paramValue (processorRef, "amount"); });
    using Effect = EffectChips::Effect;
    for (const auto& source : { std::pair { Effect::hpf,    "hpfDepth" },
                                std::pair { Effect::reverb, "reverbDepth" },
                                std::pair { Effect::delay,  "delayDepth" },
                                std::pair { Effect::riser,  "riserDepth" } })
    {
        const char* id = source.second;
        effectChips.setDepthSource (source.first, [this, id] { return paramValue (processorRef, id); });
    }
    addAndMakeVisible (effectChips);
    addAndMakeVisible (buildUpCurve);

    amountAttachment = std::make_unique<SliderAttachment> (p.getState(), "amount", amountSlider);
    mixAttachment    = std::make_unique<SliderAttachment> (p.getState(), "mix", mixSlider);
    trimAttachment   = std::make_unique<SliderAttachment> (p.getState(), "outputTrim", trimSlider);

    // The MIX/OUTPUT readouts are painted by the editor (above the tracks), so
    // repaint on value change to keep them in lock-step with the thumbs during a
    // drag rather than trailing by up to one timer tick.
    mixSlider.onValueChange  = [this] { repaint(); };
    trimSlider.onValueChange = [this] { repaint(); };

    setResizable (true, true);
    if (auto* constrainer = getConstrainer())
        constrainer->setFixedAspectRatio ((double) kDesignWidth / (double) kDesignHeight);
    setResizeLimits ((int) kDesignWidth, (int) kDesignHeight,
                     (int) (kDesignWidth * 2.0f), (int) (kDesignHeight * 2.0f));

    setSize ((int) kDesignWidth, (int) kDesignHeight);
    startTimerHz (30);
}

PreDropAudioProcessorEditor::~PreDropAudioProcessorEditor()
{
    stopTimer();
    amountSlider.setLookAndFeel (nullptr);
    mixSlider.setLookAndFeel (nullptr);
    trimSlider.setLookAndFeel (nullptr);
    effectsButton.setLookAndFeel (nullptr);
    for (auto& sl : depthSliders)
        sl.setLookAndFeel (nullptr);
}

void PreDropAudioProcessorEditor::setDepthPanelOpen (bool shouldBeOpen)
{
    depthPanelOpen = shouldBeOpen;

    // The depth knobs and the build-up curve share the same area, so swap which
    // one occupies it. The chips stay live in both states.
    buildUpCurve.setVisible (! shouldBeOpen);
    for (auto& sl : depthSliders)
        sl.setVisible (shouldBeOpen);

    resized();
    repaint();
}

void PreDropAudioProcessorEditor::timerCallback()
{
    const float a = paramValue (processorRef, "amount");
    const float m = paramValue (processorRef, "mix");
    const float t = paramValue (processorRef, "outputTrim");

    bool changed = std::abs (a - last.amount) > 1.0e-4f
                || std::abs (m - last.mix) > 1.0e-4f
                || std::abs (t - last.trim) > 1.0e-3f;

    // The HPF chip readout depends on the HPF depth even when the panel is shut,
    // and the panel's painted percent labels need refreshing while it is open.
    for (size_t i = 0; i < depthParamIds.size(); ++i)
    {
        const float d = paramValue (processorRef, depthParamIds[i]);
        if (std::abs (d - last.depths[i]) > 1.0e-4f)
        {
            last.depths[i] = d;
            changed = true;
        }
    }

    if (changed)
    {
        last.amount = a;
        last.mix    = m;
        last.trim   = t;
        repaint();
    }
}

void PreDropAudioProcessorEditor::paint (juce::Graphics& g)
{
    const float s = uiScale;
    const auto w = (float) getWidth();
    const auto h = (float) getHeight();

    // --- panel: radial gradient like the CSS backdrop -----------------------
    const juce::Point centre (w * 0.5f, -0.1f * h);
    float radius = 0.0f;
    radius = juce::jmax (radius, centre.getDistanceFrom ({ 0.0f, 0.0f }));
    radius = juce::jmax (radius, centre.getDistanceFrom ({ w,    0.0f }));
    radius = juce::jmax (radius, centre.getDistanceFrom ({ 0.0f, h    }));
    radius = juce::jmax (radius, centre.getDistanceFrom ({ w,    h    }));

    juce::ColourGradient panel (Palette::panelTop, centre.x, centre.y,
                                Palette::panelBottom, centre.x, centre.y + radius, true);
    panel.addColour (0.6, Palette::panelBottom);

    // Solid dark base first so the rounded panel's corners read as clean dark
    // rather than showing whatever the host paints behind a (not-guaranteed)
    // transparent corner.
    g.fillAll (Palette::panelBottom);

    const float cornerRadius = 16.0f * s;
    const auto  panelBounds  = getLocalBounds().toFloat().reduced (juce::jmax (1.0f, s));
    g.setGradientFill (panel);
    g.fillRoundedRectangle (panelBounds, cornerRadius);

    g.setColour (Palette::hairline);
    g.drawRoundedRectangle (panelBounds, cornerRadius, juce::jmax (1.0f, s));

    paintHeader (g, s);

    // Separator between the curve and the slider section.
    g.setColour (Palette::hairline);
    g.fillRect (juce::Rectangle (kPanelPad * s, (float) layout.sliderSeparatorY,
                                 w - 2.0f * kPanelPad * s, juce::jmax (1.0f, s)));

    paintSliderLabels (g, s);

    if (depthPanelOpen)
        paintDepthPanel (g, s);
}

void PreDropAudioProcessorEditor::paintDepthPanel (juce::Graphics& g, float s) const
{
    using namespace PreDrop;

    // Soft backing so the knobs read as a grouped panel over the curve's slot.
    const auto panel = layout.reveal.toFloat();
    g.setColour (Palette::chipSurface);
    g.fillRoundedRectangle (panel, 12.0f * s);

    // Dial convention: name + mono value stacked *beneath* the arc — the name in
    // grey (--text-2), the value in the dial's own effect colour.
    const auto nameFont  = Fonts::sans (10.0f * s, true, 0.10f);   // --tracking-data
    const auto valueFont = Fonts::mono (10.0f * s);

    for (size_t i = 0; i < depthParamIds.size(); ++i)
    {
        const auto  col    = layout.depthKnobs[i].toFloat();
        const auto  dial   = depthSliders[i].getBounds().toFloat();
        const float value  = paramValue (processorRef, depthParamIds[i]);
        const float textTop = dial.getBottom() + 2.0f * s;

        g.setFont (nameFont);
        g.setColour (Palette::textMid);
        g.drawText (depthNames[i],
                    juce::Rectangle (col.getX(), textTop, col.getWidth(), 12.0f * s),
                    juce::Justification::centredTop, false);

        g.setFont (valueFont);
        g.setColour (depthColour ((int) i));
        g.drawText (Format::percent (value),
                    juce::Rectangle (col.getX(), textTop + 12.0f * s, col.getWidth(), 13.0f * s),
                    juce::Justification::centredTop, false);
    }
}

void PreDropAudioProcessorEditor::paintHeader (juce::Graphics& g, float s) const
{
    const auto area = layout.header.toFloat();

    // Wordmark: house brand "Zanders" in primary text, tight against the pink
    // product name — CamelCase, no space, per the design-system Wordmark.
    const auto wordmarkFont = Fonts::sans (16.0f * s, true, -0.01f);   // --tracking-title
    g.setFont (wordmarkFont);

    const juce::String house ("Zanders");
    g.setColour (Palette::textStrong);
    g.drawText (house, area, juce::Justification::centredLeft, false);

    g.setColour (Palette::pink);
    g.drawText ("PreDrop", area.withTrimmedLeft (textWidth (wordmarkFont, house)),
                juce::Justification::centredLeft, false);

    const juce::String badge ("BUILD-UP");
    const auto badgeFont = Fonts::sans (10.0f * s, true, 0.14f);
    const float padX  = 11.0f * s;
    const float padY  = 4.0f * s;
    const float pillW = textWidth (badgeFont, badge) + padX * 2.0f;
    const float pillH = 10.0f * s + padY * 2.0f;

    const juce::Rectangle pill (area.getRight() - pillW, area.getCentreY() - pillH * 0.5f,
                                pillW, pillH);

    g.setColour (Palette::badge.withAlpha (0.12f));
    g.fillRoundedRectangle (pill, juce::jmin (pillH * 0.5f, 20.0f * s));

    g.setFont (badgeFont);
    g.setColour (Palette::badge);
    g.drawText (badge, pill, juce::Justification::centred, false);
}

void PreDropAudioProcessorEditor::paintSliderLabels (juce::Graphics& g, float s) const
{
    const float mix  = paramValue (processorRef, "mix");
    const float trim = paramValue (processorRef, "outputTrim");

    const auto nameFont  = Fonts::sans (10.0f * s, false, 0.08f);
    const auto valueFont = Fonts::mono (10.0f * s);

    auto drawRow = [&] (juce::Rectangle<int> area, const juce::String& name, const juce::String& value)
    {
        const auto r = area.toFloat();
        g.setFont (nameFont);
        g.setColour (Palette::textDimmer);
        g.drawText (name, r, juce::Justification::centredLeft, false);

        g.setFont (valueFont);
        g.setColour (Palette::textMid);
        g.drawText (value, r, juce::Justification::centredRight, false);
    };

    drawRow (layout.mixLabel,  "MIX",    Format::percent (mix));
    drawRow (layout.trimLabel, "OUTPUT", juce::String (trim, 1) + " dB");
}

void PreDropAudioProcessorEditor::resized()
{
    uiScale = (float) getWidth() / kDesignWidth;
    const float s = uiScale;
    const auto px = [s] (float v) { return (int) std::round (v * s); };

    lnf.setUiScale (s);
    depthLnf.setUiScale (s);
    effectChips.setUiScale (s);
    buildUpCurve.setUiScale (s);

    auto area = getLocalBounds().reduced (px (kPanelPad));

    layout.header = area.removeFromTop (px (20.0f));

    area.removeFromTop (px (14.0f));                       // gap before knob
    auto knob = area.removeFromTop (px (172.0f));
    amountSlider.setBounds (knob.withSizeKeepingCentre (px (172.0f), px (172.0f)));

    area.removeFromTop (px (16.0f));                       // gap before chips
    effectChips.setBounds (area.removeFromTop (px (40.0f)));

    // A slim row holds the EFFECTS reveal toggle, right-aligned above the slot
    // that the build-up curve and the depth panel share.
    area.removeFromTop (px (8.0f));
    auto buttonRow = area.removeFromTop (px (18.0f));
    effectsButton.setBounds (buttonRow.removeFromRight (px (84.0f)));

    area.removeFromTop (px (6.0f));                        // gap before reveal slot
    layout.reveal = area.removeFromTop (px (114.0f));
    buildUpCurve.setBounds (layout.reveal);

    // Lay out the four depth dials across the reveal slot (used when open): each
    // column is an arc on top with its name + value stacked beneath it.
    {
        auto knobs = layout.reveal.reduced (px (8.0f), px (8.0f));
        const int kGap      = px (10.0f);
        const int colW      = (knobs.getWidth() - kGap * (numDepths - 1)) / numDepths;
        const int textBlock = px (26.0f);                  // name + value lines
        const int dialSize  = juce::jmin (colW, knobs.getHeight() - textBlock);

        // Vertically centre the dial + text block within the slot.
        knobs.removeFromTop (juce::jmax (0, (knobs.getHeight() - (dialSize + textBlock)) / 2));

        for (size_t i = 0; i < depthSliders.size(); ++i)
        {
            auto col = knobs.removeFromLeft (colW);
            if (i + 1 < depthSliders.size())
                knobs.removeFromLeft (kGap);

            layout.depthKnobs[i] = col;                     // full column for name/value paint
            depthSliders[i].setBounds (col.removeFromTop (dialSize)
                                          .withSizeKeepingCentre (dialSize, dialSize));
        }
    }

    area.removeFromTop (px (20.0f));                       // gap before slider section
    layout.sliderSeparatorY = area.getY();
    area.removeFromTop (px (18.0f));                       // padding under the separator

    auto sliders = area.removeFromTop (px (38.0f));
    const int gap  = px (22.0f);
    const int colW = (sliders.getWidth() - gap) / 2;

    auto layoutColumn = [&] (juce::Rectangle<int> column, juce::Rectangle<int>& labelOut, juce::Slider& slider)
    {
        labelOut = column.removeFromTop (px (14.0f));
        column.removeFromTop (px (8.0f));
        slider.setBounds (column.removeFromTop (px (16.0f)));
    };

    auto mixColumn  = sliders.removeFromLeft (colW);
    sliders.removeFromLeft (gap);
    auto trimColumn = sliders;

    layoutColumn (mixColumn,  layout.mixLabel,  mixSlider);
    layoutColumn (trimColumn, layout.trimLabel, trimSlider);
}
