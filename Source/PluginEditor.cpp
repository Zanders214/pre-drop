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
      buildUpCurve (p.getState(), "amount"),
      link (p)
{
    // --- Amount knob ---------------------------------------------------------
    amountSlider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    // 270 degree sweep from 7:30 to 4:30. JUCE wants non-negative angles, so
    // -135..135 degrees is written as the identical 225..495.
    amountSlider.setRotaryParameters (juce::degreesToRadians (225.0f),
                                      juce::degreesToRadians (495.0f), true);
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
        sl.setRotaryParameters (juce::degreesToRadians (225.0f),
                                juce::degreesToRadians (495.0f), true);
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
    effectsButton.onClick = [this] { setReveal (effectsButton.getToggleState() ? Reveal::effects : Reveal::curve); };
    addAndMakeVisible (effectsButton);

    // The engaged pill's glow extends past the button, so let it paint outside
    // its bounds; the editor repaints whenever a glow switches on or off.
    effectsButton.setPaintingIsUnclipped (true);

    // --- RISER sample pill ----------------------------------------------------
    riserUi.button.setLookAndFeel (&lnf);
    riserUi.button.setPaintingIsUnclipped (true);
    riserUi.button.onClick = [this] { showRiserMenu(); };
    addAndMakeVisible (riserUi.button);
    updateRiserButton();

    // --- Linking: TRACKS reveal toggle, MAIN / TRACK pill, the two panels -----
    link.tracksButton.setButtonText ("TRACKS");
    link.tracksButton.setClickingTogglesState (true);
    link.tracksButton.setLookAndFeel (&lnf);
    link.tracksButton.setPaintingIsUnclipped (true);
    link.tracksButton.setTooltip ("Choose which linked tracks get the high-pass, reverb and delay.");
    link.tracksButton.onClick = [this] { setReveal (link.tracksButton.getToggleState() ? Reveal::tracks : Reveal::curve); };
    addAndMakeVisible (link.tracksButton);

    link.roleButton.setLookAndFeel (&lnf);
    link.roleButton.setPaintingIsUnclipped (true);
    link.roleButton.setTooltip ("What this PreDrop does: MAIN on your master, TRACK on a track, RETURN on a return track. Click to choose.");
    link.roleButton.onClick = [this] { showRoleMenu(); };
    addAndMakeVisible (link.roleButton);

    addChildComponent (link.matrix);
    addChildComponent (link.panel);

    // --- Indicators (a TRACK shows what the main is driving it with) ----------
    effectChips.setAmountSource ([this] { return displayedAmount(); });
    using Effect = EffectChips::Effect;
    for (const auto& source : { std::pair { Effect::hpf,    "hpfDepth" },
                                std::pair { Effect::reverb, "reverbDepth" },
                                std::pair { Effect::delay,  "delayDepth" },
                                std::pair { Effect::riser,  "riserDepth" } })
    {
        const char* id = source.second;
        effectChips.setDepthSource (source.first, [this, id] { return displayedDepth (id); });
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
    applyRole();
    startTimerHz (30);
}

PreDropAudioProcessorEditor::~PreDropAudioProcessorEditor()
{
    stopTimer();
    amountSlider.setLookAndFeel (nullptr);
    mixSlider.setLookAndFeel (nullptr);
    trimSlider.setLookAndFeel (nullptr);
    effectsButton.setLookAndFeel (nullptr);
    riserUi.button.setLookAndFeel (nullptr);
    link.tracksButton.setLookAndFeel (nullptr);
    link.roleButton.setLookAndFeel (nullptr);
    for (auto& sl : depthSliders)
        sl.setLookAndFeel (nullptr);
}

void PreDropAudioProcessorEditor::setReveal (Reveal newReveal)
{
    reveal = newReveal;

    // The curve, the depth knobs and the TRACKS panel share one slot; a TRACK
    // shows its own panel there instead. The chips stay live throughout.
    const bool main = ! isTrackMode();
    buildUpCurve.setVisible (main && reveal == Reveal::curve);
    for (auto& sl : depthSliders)
        sl.setVisible (main && reveal == Reveal::effects);
    link.matrix.setVisible (main && reveal == Reveal::tracks);

    effectsButton.setToggleState (reveal == Reveal::effects, juce::dontSendNotification);
    link.tracksButton.setToggleState (reveal == Reveal::tracks, juce::dontSendNotification);

    resized();
    repaint();
}

bool PreDropAudioProcessorEditor::isTrackMode() const noexcept
{
    return processorRef.followsMain();
}

bool PreDropAudioProcessorEditor::isReturnMode() const noexcept
{
    return processorRef.getRole() == PreDropAudioProcessor::Role::returnTrack;
}

void PreDropAudioProcessorEditor::showRoleMenu()
{
    using Role = PreDropAudioProcessor::Role;
    const auto current = processorRef.getRole();

    // Item ids are the role + 1 (0 means the menu was dismissed).
    juce::PopupMenu menu;
    menu.addSectionHeader ("This PreDrop is the...");
    menu.addItem (1 + (int) Role::main,        "MAIN: on your master. Amount knob, riser, track list",          true, current == Role::main);
    menu.addItem (1 + (int) Role::track,       "TRACK: on a track. Its own high-pass, reverb and delay levels", true, current == Role::track);
    menu.addItem (1 + (int) Role::returnTrack, "RETURN: on a return track. Reverb and delay for its sends",     true, current == Role::returnTrack);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&link.roleButton),
                        [safe = juce::Component::SafePointer (this)] (int choice)
    {
        if (safe == nullptr || choice <= 0)
            return;
        safe->processorRef.setRole ((Role) (choice - 1));
        safe->applyRole();
    });
}

void PreDropAudioProcessorEditor::applyRole()
{
    const bool track = isTrackMode();
    const bool ret   = isReturnMode();
    link.shownRole = (int) processorRef.getRole();

    link.roleButton.setButtonText (ret ? "RETURN" : (track ? "TRACK" : "MAIN"));
    link.roleButton.setToggleState (track, juce::dontSendNotification);

    riserUi.button.setVisible (! track);
    effectsButton.setVisible (! track);
    link.tracksButton.setVisible (! track);
    link.panel.setReturnMode (ret);
    link.panel.setVisible (track);

    // A return is always fully wet (its dry is the tracks' own signal).
    mixSlider.setEnabled (! ret);
    mixSlider.setAlpha (ret ? 0.35f : 1.0f);

    // As a TRACK the knob mirrors the main's Amount instead of editing this
    // instance's own (unused) one.
    if (track)
    {
        amountAttachment.reset();
        amountSlider.setInterceptsMouseClicks (false, false);
        amountSlider.setValue (displayedAmount(), juce::dontSendNotification);
    }
    else if (amountAttachment == nullptr)
    {
        amountSlider.setInterceptsMouseClicks (true, true);
        amountAttachment = std::make_unique<SliderAttachment> (processorRef.getState(), "amount", amountSlider);
    }

    setReveal (reveal);
}

float PreDropAudioProcessorEditor::displayedAmount() const
{
    if (! isTrackMode())
        return paramValue (processorRef, "amount");

    const auto& hub = processorRef.getLinkHub();
    return hub.linkActive() ? hub.mainState().amount.load() : 0.0f;
}

float PreDropAudioProcessorEditor::displayedDepth (const char* depthId) const
{
    if (! isTrackMode())
        return paramValue (processorRef, depthId);

    // A track gets its own share of the main's depth for each effect, and never
    // the riser (that plays once, on the main).
    const juce::String id (depthId);
    const auto effect = id == "hpfDepth"    ? LinkHub::Effect::hpf
                      : id == "reverbDepth" ? LinkHub::Effect::reverb
                      : LinkHub::Effect::delay;
    const auto& hub = processorRef.getLinkHub();
    if (id == "riserDepth" || ! hub.linkActive())
        return 0.0f;
    return hub.mainState().depths[(size_t) effect].load() * processorRef.getLinkLevel (effect);
}

void PreDropAudioProcessorEditor::updateRiserButton()
{
    const auto name = processorRef.getRiserSampleName();
    if (name == riserUi.shownName)
        return;

    riserUi.shownName = name;

    // Lit (accent pill) while a user sample replaces the built-in noise.
    riserUi.button.setToggleState (name.isNotEmpty(), juce::dontSendNotification);
    fitRiserLabel();
    repaint();   // the lit pill's glow reaches beyond the button
    riserUi.button.setTooltip (name.isEmpty() ? "Built-in noise riser. Click, or drop an audio file here, to use your own sample."
                                              : "Riser sample: " + name);
}

void PreDropAudioProcessorEditor::showRiserMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Load riser sample...");
    menu.addItem (2, "Use built-in noise", processorRef.getRiserSampleName().isNotEmpty());

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&riserUi.button),
                        [safe = juce::Component::SafePointer (this)] (int choice)
    {
        if (safe == nullptr)
            return;

        if (choice == 2)
        {
            safe->processorRef.clearRiserSample();
            safe->updateRiserButton();
        }
        else if (choice == 1)
        {
            safe->riserUi.chooser = std::make_unique<juce::FileChooser> ("Choose a riser sample", juce::File(),
                                                                        safe->processorRef.getRiserFileWildcard());
            safe->riserUi.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                                [safe] (const juce::FileChooser& chooser)
            {
                if (safe != nullptr && chooser.getResult().existsAsFile())
                    safe->loadRiser (chooser.getResult());
            });
        }
    });
}

void PreDropAudioProcessorEditor::loadRiser (const juce::File& file)
{
    if (const auto error = processorRef.loadRiserSample (file); error.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't use that riser", error);

    updateRiserButton();
}

void PreDropAudioProcessorEditor::fitRiserLabel()
{
    // Shorten the sample's name until "RISER: NAME" fits the pill, which
    // shares its row with TRACKS and EFFECTS.
    const auto& name = riserUi.shownName;
    auto label = name.isEmpty() ? juce::String ("NOISE")
                                : juce::File::createFileWithoutCheckingPath (name).getFileNameWithoutExtension().toUpperCase();

    // Before the first layout the pill has no width to fit to yet.
    if (riserUi.button.getWidth() > 0)
    {
        const auto font = Fonts::sans (10.0f * uiScale, true, 0.16f);         // the pill's caption font
        const float room = (float) riserUi.button.getWidth() - 30.0f * uiScale; // dot, gap and padding
        // From the code point: a "\u2026" literal isn't UTF-8 under MSVC's default code page.
        const auto ellipsis = juce::String::charToString ((juce::juce_wchar) 0x2026);
        auto kept = label;
        while (kept.isNotEmpty() && textWidth (font, "RISER: " + label) > room)
        {
            kept = kept.dropLastCharacters (1);
            label = kept + ellipsis;
        }
    }

    riserUi.button.setButtonText ("RISER: " + label);
}

bool PreDropAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return ! isTrackMode() && files.size() == 1 && processorRef.isSupportedRiserFile (juce::File (files[0]));
}

void PreDropAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    riserUi.dragActive = true;
    repaint();
}

void PreDropAudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
    riserUi.dragActive = false;
    repaint();
}

void PreDropAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    riserUi.dragActive = false;
    repaint();
    loadRiser (juce::File (files[0]));
}

void PreDropAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    if (! riserUi.dragActive)
        return;

    // Drop target: dim the face and invite the drop, in the accent colour.
    const float s = uiScale;
    const auto area = getLocalBounds().toFloat().reduced (8.0f * s);
    g.setColour (Palette::panelBottom.withAlpha (0.85f));
    g.fillRoundedRectangle (area, 14.0f * s);
    g.setColour (Palette::badge);
    g.drawRoundedRectangle (area, 14.0f * s, juce::jmax (1.0f, 2.0f * s));
    g.setFont (Fonts::sans (13.0f * s, true, 0.14f));
    g.drawText ("DROP TO USE AS RISER", area, juce::Justification::centred, false);
}

void PreDropAudioProcessorEditor::timerCallback()
{
    updateRiserButton();

    if ((int) processorRef.getRole() != link.shownRole)
        applyRole();
    if (link.matrix.isVisible())
        link.matrix.refresh();
    if (link.panel.isVisible())
        link.panel.refresh();

    const float a = displayedAmount();
    if (isTrackMode() && std::abs ((float) amountSlider.getValue() - a) > 1.0e-4f)
        amountSlider.setValue (a, juce::dontSendNotification);

    const float m = paramValue (processorRef, "mix");
    const float t = paramValue (processorRef, "outputTrim");

    bool changed = std::abs (a - last.amount) > 1.0e-4f
                || std::abs (m - last.mix) > 1.0e-4f
                || std::abs (t - last.trim) > 1.0e-3f;

    // The HPF chip readout depends on the HPF depth even when the panel is shut,
    // and the panel's painted percent labels need refreshing while it is open.
    for (size_t i = 0; i < depthParamIds.size(); ++i)
    {
        const float d = displayedDepth (depthParamIds[i]);
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

    if (reveal == Reveal::effects && ! isTrackMode())
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

    // The MAIN / TRACK pill (a button) sits at the header's right edge.
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

    drawRow (layout.mixLabel,  "MIX",    isReturnMode() ? juce::String ("WET ONLY") : Format::percent (mix));
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
    link.roleButton.setBounds (layout.header.withTrimmedLeft (layout.header.getWidth() - px (78.0f))
                                            .withSizeKeepingCentre (px (78.0f), px (20.0f)));

    area.removeFromTop (px (14.0f));                       // gap before knob
    auto knob = area.removeFromTop (px (172.0f));
    amountSlider.setBounds (knob.withSizeKeepingCentre (px (172.0f), px (172.0f)));

    area.removeFromTop (px (16.0f));                       // gap before chips
    effectChips.setBounds (area.removeFromTop (px (40.0f)));

    // A slim row holds RISER, TRACKS and EFFECTS above the slot that the curve,
    // the depth knobs and the TRACKS panel share.
    area.removeFromTop (px (8.0f));
    auto buttonRow = area.removeFromTop (px (18.0f));
    const int buttonRowTop = buttonRow.getY();
    effectsButton.setBounds (buttonRow.removeFromRight (px (84.0f)));
    buttonRow.removeFromRight (px (8.0f));
    link.tracksButton.setBounds (buttonRow.removeFromRight (px (76.0f)));
    buttonRow.removeFromRight (px (8.0f));
    riserUi.button.setBounds (buttonRow);
    fitRiserLabel();

    area.removeFromTop (px (6.0f));                        // gap before reveal slot
    layout.reveal = area.removeFromTop (px (114.0f));
    buildUpCurve.setBounds (layout.reveal);
    link.matrix.setUiScale (s);
    link.matrix.setBounds (layout.reveal);

    // A TRACK has no button row: its panel takes the row and the slot.
    link.panel.setUiScale (s);
    link.panel.setBounds (layout.reveal.withTop (buttonRowTop));

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
