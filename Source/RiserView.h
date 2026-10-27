#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "PluginProcessor.h"
#include "PreDropLookAndFeel.h"
#include "SettingKnob.h"
#include "PreDropTheme.h"
#include "RiserTiming.h"

/**
    NumberChip: a number in a pill with its unit beside it ("128 BPM").

    Drag up or down, or scroll, to change it in whole steps (tenths with
    shift held); double-click to type a value.
*/
class NumberChip : public juce::Slider
{
public:
    NumberChip (const juce::String& unitName, double minimum, double maximum)
        : unit (unitName)
    {
        // A relative vertical drag, like the knobs; paint() draws the chip.
        setSliderStyle (juce::Slider::RotaryVerticalDrag);
        setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        setRange (minimum, maximum, 0.01);
        setMouseDragSensitivity (juce::roundToInt ((maximum - minimum) * 4.0));   // 4 px a step
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        setRepaintsOnMouseActivity (true);

        // Hidden until a double-click; it owns the text editor while typing.
        entry.setJustificationType (juce::Justification::centred);
        entry.setColour (juce::Label::textWhenEditingColourId, PreDrop::Palette::textStrong);
        entry.setColour (juce::Label::backgroundWhenEditingColourId, PreDrop::Palette::panelBottom);
        entry.setColour (juce::Label::outlineWhenEditingColourId, PreDrop::Palette::badge);
        entry.onTextChange  = [this] { setValue (getValueFromText (entry.getText()), juce::sendNotificationSync); };
        entry.onEditorHide  = [this] { entry.setVisible (false); };
        addChildComponent (entry);
    }

    /** Opens the text entry, as a double-click does. */
    void showEntry()
    {
        entry.setFont (PreDrop::Fonts::mono (9.5f * scale()));
        entry.setText (getTextFromValue (getValue()), juce::dontSendNotification);
        entry.setVisible (true);
        entry.showEditor();
    }

    juce::Label& entryLabel() noexcept { return entry; }

    juce::String getTextFromValue (double v) override { return format (v); }

    /** Up to two decimals, without trailing zeros: 128, 127.5, 99.25. */
    static juce::String format (double v)
    {
        auto text = juce::String (std::round (v * 100.0) / 100.0, 2);
        while (text.containsChar ('.') && (text.endsWithChar ('0') || text.endsWithChar ('.')))
            text = text.dropLastCharacters (1);
        return text;
    }

    double snapValue (double attempted, juce::Slider::DragMode mode) override
    {
        if (mode == notDragging)
            return attempted;
        const double step = juce::ModifierKeys::currentModifiers.isShiftDown() ? 0.1 : 1.0;
        return std::round (attempted / step) * step;
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { showEntry(); }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        // A wheel notch is a step; a trackpad's stream of small moves adds up to one.
        auto delta = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;
        if (wheel.isReversed)
            delta = -delta;
        if (juce::exactlyEqual (delta, 0.0f))
            return;
        wheelTravel = wheel.isSmooth ? wheelTravel + delta : (delta > 0.0f ? wheelStep : -wheelStep);
        const auto steps = (int) (wheelTravel / wheelStep);
        if (steps == 0)
            return;
        wheelTravel -= (float) steps * wheelStep;

        const double step = juce::ModifierKeys::currentModifiers.isShiftDown() ? 0.1 : 1.0;
        setValue (std::round (getValue() / step) * step + steps * step, juce::sendNotificationSync);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = scale();
        const auto bounds = getLocalBounds().toFloat().reduced (juce::jmax (0.5f, s));
        const float radius = bounds.getHeight() * 0.5f;

        float fill = 0.06f;
        if (isMouseButtonDown())
            fill = 0.10f;
        else if (isMouseOver())
            fill = 0.08f;
        g.setColour (juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, fill));
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.12f));
        g.drawRoundedRectangle (bounds, radius, juce::jmax (1.0f, s));

        if (entry.isVisible())
            return;

        // The number in the riser's colour, its unit after it, centred together.
        const auto numberFont = Fonts::mono (9.5f * s);
        const auto unitFont   = Fonts::sans (8.0f * s, true, 0.08f);
        const auto number     = getTextFromValue (getValue());
        const float gap       = 4.0f * s;
        const float numberW   = widthOf (numberFont, number);
        const float unitW     = widthOf (unitFont, unit);
        const float x         = bounds.getCentreX() - (numberW + gap + unitW) * 0.5f;

        g.setFont (numberFont);
        g.setColour (Palette::amber);
        g.drawText (number, juce::Rectangle (x, bounds.getY(), numberW + 2.0f * s, bounds.getHeight()),
                    juce::Justification::centredLeft, false);
        g.setFont (unitFont);
        g.setColour (Palette::textDim);
        g.drawText (unit, juce::Rectangle (x + numberW + gap, bounds.getY(), unitW + 2.0f * s, bounds.getHeight()),
                    juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        juce::Slider::resized();
        entry.setBounds (getLocalBounds());
    }

    static float widthOf (const juce::Font& font, const juce::String& text)
    {
        juce::GlyphArrangement arrangement;
        arrangement.addLineOfText (font, text, 0.0f, 0.0f);
        return arrangement.getBoundingBox (0, -1, true).getWidth();
    }

private:
    static constexpr float wheelStep = 0.05f;

    float scale() const noexcept { return juce::jmax (0.1f, (float) getHeight() / 14.0f); }   // a compact pill's height

    juce::String unit;
    juce::Label  entry;
    float        wheelTravel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NumberChip)
};

/**
    RiserWaveform: the riser sample laid out against its bars.

    Bar lines (numbered along the top) and beat lines come from the sample's
    own tempo and meter, and the sample is drawn against them: change its
    tempo and it stretches or squeezes between fixed bars. When the riser
    follows the project it loops a whole number of bars: the loop's end is
    marked, and anything past it, which doesn't play, is shaded. While the
    riser plays, a playhead shows where it is.

    Two drags change the timing (reported through onTimingChange):
      - grab the sound and drag it along: it stretches from its start, so the
        point you hold lands where you let go (on a beat line when it's
        close; hold shift to place it freely). The loop keeps its bars, so
        dragging the end of the sound onto a bar line cuts the tail after it;
      - drag the loop's end marker to set how many bars the loop is.
*/
class RiserWaveform : public juce::Component,
                      public juce::SettableTooltipClient
{
public:
    struct Shown
    {
        RiserSource::Outline outline;
        double               lengthSeconds = 0.0;
        RiserTiming          timing;
    };

    RiserWaveform() = default;

    /** Called with the timing a drag asks for. */
    std::function<void (const RiserTiming&)> onTimingChange;

    void setUiScale (float s) noexcept { uiScale = juce::jmax (0.1f, s); }

    void setSample (Shown newShown)
    {
        shown = std::move (newShown);
        peak = 0.0f;
        for (size_t i = 0; i < shown.outline.low.size(); ++i)
            peak = juce::jmax (peak, -shown.outline.low[i], shown.outline.high[i]);
        setTooltip (hasSample() ? "Drag the sound to stretch it against the bars (it snaps to beats; hold shift to place it freely). "
                                  "Drag the amber line to set how many bars loop."
                                : juce::String());
        repaint();
    }

    void setTiming (const RiserTiming& timing)
    {
        if (timing == shown.timing)
            return;
        shown.timing = timing;
        repaint();
    }

    /** The riser's place in its loop (0..1), or -1 while it isn't playing. */
    void setPlayhead (float fraction)
    {
        if (std::abs (fraction - playhead) < 1.0e-4f)
            return;
        playhead = fraction;
        repaint();
    }

    float getPlayhead() const noexcept { return playhead; }

    /** Where the loop ends, in seconds of the sample: whole bars when following
        the project, otherwise the whole sample. */
    double loopSeconds() const noexcept { return loopBars() * shown.timing.barSeconds(); }

    /** The bars the view spans (held still while dragging). */
    double spanBars() const noexcept
    {
        return drag.mode != Drag::none ? drag.span : juce::jmax (sampleBars(), loopBars(), 0.25);
    }

    /** The x position of a point `bars` into the sample, and back. */
    float xOfBars (double bars) const noexcept { return (float) (bars / spanBars()) * (float) getWidth(); }
    double barsAt (float x) const noexcept     { return x / juce::jmax (1.0f, (float) getWidth()) * spanBars(); }

    // --- Dragging ---------------------------------------------------------------
    void mouseMove (const juce::MouseEvent& e) override
    {
        if (! hasSample())
            setMouseCursor (juce::MouseCursor::NormalCursor);
        else
            setMouseCursor (nearLoopEnd (e.position.x) ? juce::MouseCursor::LeftRightResizeCursor
                                                       : juce::MouseCursor::DraggingHandCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = {};
        if (! hasSample())
            return;

        const double span   = spanBars();
        const double held   = barsAt (e.position.x);
        drag.start = shown.timing;
        if (nearLoopEnd (e.position.x))
        {
            drag.mode = Drag::loopEnd;
        }
        else if (held > 0.02 && held < sampleBars())
        {
            drag.mode        = Drag::stretch;
            drag.heldSeconds = held * shown.timing.barSeconds();
        }
        drag.span = span;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag.mode == Drag::none)
            return;

        auto t = drag.start;
        const double at = barsAt (e.position.x);
        if (drag.mode == Drag::loopEnd)
        {
            t.loopBars = juce::jlimit (1, RiserTiming::maxLoopBars, juce::roundToInt (at));
        }
        else
        {
            // The held point lands at `target` bars: a tempo that many bars
            // per its distance from the start.
            double target = juce::jmax (0.02, at);
            if (! e.mods.isShiftDown())
                target = snapToBeat (target);
            t.bpm = juce::jlimit (RiserTiming::minBpm, RiserTiming::maxBpm,
                                  t.quarterNotesPerBar() * 60.0 * target / drag.heldSeconds);
            if (t.matchProject)
                t.loopBars = drag.start.loopBarsFor (shown.lengthSeconds);   // the loop keeps its bars
        }

        if (t != shown.timing && onTimingChange != nullptr)
            onTimingChange (t);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        drag = {};
        repaint();   // the span may change now
    }

    // --- Drawing ----------------------------------------------------------------
    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = uiScale;
        const auto area = getLocalBounds().toFloat();
        g.setColour (Palette::panelBottom.withAlpha (0.55f));
        g.fillRoundedRectangle (area, 6.0f * s);

        if (! hasSample())
        {
            g.setFont (Fonts::sans (9.5f * s, false, 0.02f));
            g.setColour (Palette::textDim);
            g.drawFittedText ("The built-in noise riser rises with the build-up.\nLoad a sample, or drop one on PreDrop, to set its timing.",
                              area.reduced (10.0f * s, 4.0f * s).toNearestInt(), juce::Justification::centred, 3);
            return;
        }

        const auto ruler = area.withHeight (11.0f * s);
        const auto wave  = area.withTrimmedTop (ruler.getHeight()).reduced (0.0f, 2.0f * s);

        paintGrid (g, ruler, wave);
        paintWave (g, wave);

        // What the loop leaves out, dimmed, and where it wraps (with a tab to
        // drag it by).
        if (shown.timing.matchProject)
        {
            const float end = xOfBars (loopBars());
            if (end < area.getRight())
            {
                g.setColour (Palette::panelBottom.withAlpha (0.6f));
                g.fillRect (juce::Rectangle (end, wave.getY(), area.getRight() - end, wave.getHeight()));
            }
            g.setColour (Palette::amber.withAlpha (0.9f));
            g.fillRect (juce::Rectangle (end - juce::jmax (1.0f, s), area.getY(), juce::jmax (1.0f, s) * 1.5f, area.getHeight()));
            g.fillRoundedRectangle (juce::Rectangle (end - 6.0f * s, ruler.getY() + 1.0f * s, 6.0f * s, 9.0f * s), 1.5f * s);
        }

        if (playhead >= 0.0f)
        {
            const float x = xOfBars (playhead * loopBars());
            const auto line = juce::Rectangle (x - 0.5f * s, wave.getY(), juce::jmax (1.0f, s), wave.getHeight());
            juce::Path glow;
            glow.addRectangle (line);
            juce::DropShadow (juce::Colours::white.withAlpha (0.7f), (int) (6.0f * s), {}).drawForPath (g, glow);
            g.setColour (juce::Colours::white);
            g.fillRect (line);
        }
    }

private:
    struct Drag
    {
        enum Mode { none, stretch, loopEnd };
        Mode        mode        = none;
        RiserTiming start;                // the timing when the drag began
        double      span        = 1.0;    // the view's bars, held still meanwhile
        double      heldSeconds = 0.0;    // the point of the sample being stretched
    };

    bool hasSample() const noexcept { return shown.lengthSeconds > 0.0 && ! shown.outline.high.empty(); }

    double sampleBars() const noexcept { return shown.timing.barsIn (shown.lengthSeconds); }

    // The bars that play: whole bars when following the project, else all of it.
    double loopBars() const noexcept
    {
        return shown.timing.matchProject ? (double) shown.timing.loopBarsFor (shown.lengthSeconds) : sampleBars();
    }

    bool nearLoopEnd (float x) const noexcept
    {
        return shown.timing.matchProject && std::abs (x - xOfBars (loopBars())) <= 6.0f * uiScale;
    }

    // `bars`, moved onto the nearest beat line if it's within a few pixels.
    double snapToBeat (double bars) const noexcept
    {
        const double beat    = 1.0 / shown.timing.beatsPerBar;
        const double nearest = std::round (bars / beat) * beat;
        return nearest > 0.0 && std::abs (xOfBars (nearest) - xOfBars (bars)) <= 6.0f * uiScale ? nearest : bars;
    }

    void paintGrid (juce::Graphics& g, juce::Rectangle<float> ruler, juce::Rectangle<float> wave) const
    {
        using namespace PreDrop;
        const float s = uiScale;
        const auto& t = shown.timing;
        const float barPx  = ruler.getWidth() / (float) spanBars();
        const float beatPx = barPx / (float) t.beatsPerBar;

        // Number every bar, or every 2nd, 4th... when they crowd together.
        int every = 1;
        while (barPx * (float) every < 16.0f * s && every < 1024)
            every *= 2;

        g.setFont (Fonts::mono (7.5f * s));
        const int bars = (int) std::ceil (spanBars() - 1.0e-6);
        for (int bar = 0; bar < bars; ++bar)
        {
            const float x = ruler.getX() + (float) bar * barPx;
            if (beatPx >= 5.0f * s)
            {
                g.setColour (Palette::hairline);
                for (int beat = 1; beat < t.beatsPerBar; ++beat)
                    g.fillRect (juce::Rectangle (x + (float) beat * beatPx, wave.getY(), juce::jmax (1.0f, 0.5f * s), wave.getHeight()));
            }
            if (bar % every != 0)
                continue;
            g.setColour (Palette::sliderTrack);
            g.fillRect (juce::Rectangle (x, ruler.getY() + 2.0f * s, juce::jmax (1.0f, 0.75f * s), wave.getBottom() - ruler.getY() - 2.0f * s));
            g.setColour (Palette::textDimmer);
            g.drawText (juce::String (bar + 1), juce::Rectangle (x + 3.0f * s, ruler.getY(), barPx * (float) every, ruler.getHeight()),
                        juce::Justification::centredLeft, false);
        }
    }

    void paintWave (juce::Graphics& g, juce::Rectangle<float> wave) const
    {
        using namespace PreDrop;
        const auto& low  = shown.outline.low;
        const auto& high = shown.outline.high;
        const auto columns = (int) high.size();
        const float gain   = peak > 1.0e-6f ? 0.95f / peak : 1.0f;
        const float mid    = wave.getCentreY();
        const float half   = wave.getHeight() * 0.5f;

        // Outline columns per bar of the view.
        const double columnsPerBar = shown.timing.barSeconds() / shown.lengthSeconds * columns;
        const double barsPerPixel  = spanBars() / wave.getWidth();

        g.setColour (Palette::amber.withAlpha (0.8f));
        const int pixels = juce::roundToInt (wave.getWidth());
        for (int x = 0; x < pixels; ++x)
        {
            // The outline columns under this pixel; none past the sample's end.
            const double from = x * barsPerPixel * columnsPerBar;
            const double to   = (x + 1) * barsPerPixel * columnsPerBar;
            if (from >= columns)
                break;
            float lo = 0.0f;
            float hi = 0.0f;
            for (auto c = (int) from; c < juce::jmin (columns, juce::jmax ((int) from + 1, (int) std::ceil (to))); ++c)
            {
                lo = juce::jmin (lo, low[(size_t) c]);
                hi = juce::jmax (hi, high[(size_t) c]);
            }
            const float top    = mid - hi * gain * half;
            const float bottom = mid - lo * gain * half;
            g.fillRect (juce::Rectangle (wave.getX() + (float) x, top, 1.0f, juce::jmax (1.0f, bottom - top)));
        }
    }

    Shown shown;
    Drag  drag;
    float peak     = 0.0f;
    float playhead = -1.0f;
    float uiScale  = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RiserWaveform)
};

/**
    RiserView: the RISER view, opened from the riser's source pill under its
    knob in EFFECTS. It fills the slot below the chips, button row included.

    Along the top: BACK (to EFFECTS), the source pill (load a sample or go back
    to the built-in noise) and the project's tempo. Then the sample with its
    bars and beats (drag it to stretch it, or its loop end to set the loop),
    and its timing: its own tempo (BPM); its meter; whether it plays at the
    project's tempo (MATCH PROJECT), locked to the project's bars, and if so
    how many bars it loops (BARS) and whether it keeps its pitch (KEEP PITCH)
    or speeds up / slows down like tape.

    With the built-in noise instead of a sample it shows the noise's sweep:
    where it STARTs and ENDs, and its RESONANCE.
*/
class RiserView : public juce::Component
{
public:
    explicit RiserView (PreDropAudioProcessor& p) : processor (p)
    {
        for (auto* b : { &backButton, &sourcePill, &meterPill, &matchPill, &pitchPill })
        {
            b->setLookAndFeel (&pills);
            PreDropLookAndFeel::setCompactPill (*b);
            b->setPaintingIsUnclipped (true);   // the lit pill's glow
            addAndMakeVisible (*b);
        }

        backButton.setButtonText ("BACK");
        backButton.setTooltip ("Back to the effect depths.");
        backButton.onClick = [this] { if (onBack != nullptr) onBack(); };

        sourcePill.setComponentID ("riserViewSource");
        sourcePill.onClick = [this] { showSourceMenu(); };

        tempoChip.setTooltip ("The sample's own tempo, which places its bars and beats. Drag, scroll or double-click to type.");
        tempoChip.onValueChange = [this] { changeTiming ([this] (RiserTiming& t) { t.bpm = tempoChip.getValue(); }); };
        barsChip.setTooltip ("How many bars loop, following the project: past them is cut, short of them is silence. "
                             "Drag, scroll or double-click to type.");
        barsChip.onValueChange = [this]
        {
            changeTiming ([this] (RiserTiming& t) { t.loopBars = juce::roundToInt (barsChip.getValue()); });
        };
        addAndMakeVisible (tempoChip);
        addAndMakeVisible (barsChip);

        meterPill.setTooltip ("The sample's meter.");
        meterPill.onClick = [this] { showMeterMenu(); };

        matchPill.setButtonText ("MATCH PROJECT");
        matchPill.setTooltip ("Play at the project's tempo, starting on the next bar line and looping in whole bars.");
        matchPill.onClick = [this] { changeTiming ([] (RiserTiming& t) { t.matchProject = ! t.matchProject; }); };

        pitchPill.setButtonText ("KEEP PITCH");
        pitchPill.setTooltip ("On: time-stretched to the project's tempo, at its own pitch. Off: sped up or slowed down, like tape.");
        pitchPill.onClick = [this] { changeTiming ([] (RiserTiming& t) { t.keepPitch = ! t.keepPitch; }); };

        waveform.onTimingChange = [this] (const RiserTiming& wanted)
        {
            changeTiming ([&wanted] (RiserTiming& t) { t = wanted; });
        };
        addAndMakeVisible (waveform);

        using Id = EffectSettings::Id;
        for (const auto& [id, tip] : { std::pair { Id::riserStart, "Where the noise's sweep starts, at the bottom of the build-up." },
                                       std::pair { Id::riserEnd, "Where the noise's sweep ends, at full Amount." },
                                       std::pair { Id::riserResonance, "Narrows the noise into a whistle." } })
        {
            noiseKnobs.push_back (std::make_unique<SettingKnob> (p.getState(), id, PreDrop::Palette::amber, knobLookAndFeel, tip));
            addChildComponent (*noiseKnobs.back());
        }
        refresh();
    }

    ~RiserView() override
    {
        for (auto* b : { &backButton, &sourcePill, &meterPill, &matchPill, &pitchPill })
            b->setLookAndFeel (nullptr);
    }

    /** Called by the BACK button. */
    std::function<void()> onBack;

    void setUiScale (float s) noexcept
    {
        uiScale = juce::jmax (0.1f, s);
        pills.setUiScale (uiScale);
        waveform.setUiScale (uiScale);
        knobLookAndFeel.setUiScale (uiScale);
        for (auto& k : noiseKnobs)
            k->setUiScale (uiScale);
    }

    /** Follows the riser: its sample, timing and playhead. From the editor's timer. */
    void refresh()
    {
        auto& riser = processor.getRiser();

        if (const int id = riser.getSampleId(); id != shownId)
        {
            shownId     = id;
            shownLength = riser.getLengthSeconds();
            shownName   = riser.getName();
            waveform.setSample ({ riser.getOutline(), shownLength, riser.getTiming() });
            updateSourcePill();
            resized();
        }

        showTiming (riser.getTiming());
        waveform.setPlayhead (processor.getRiserPlayhead());

        if (const auto status = statusText(); status != shownStatus)
        {
            shownStatus = status;
            repaint();
        }
    }

    /** Uses an audio file as the riser, explaining in an alert if it can't. */
    void loadFile (const juce::File& file)
    {
        if (const auto error = processor.getRiser().load (file); error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't use that riser", error);
        refresh();
    }

    /** Sets the sample's meter, as the METER menu does. */
    void setMeter (int beatsPerBar, int beatUnit)
    {
        changeTiming ([=] (RiserTiming& t)
        {
            t.beatsPerBar = beatsPerBar;
            t.beatUnit    = beatUnit;
        });
    }

    static constexpr std::array<std::pair<int, int>, 7> meters { { { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 12, 8 } } };

    // For tests.
    NumberChip&       tempo() noexcept       { return tempoChip; }
    NumberChip&       bars() noexcept        { return barsChip; }
    juce::TextButton& back() noexcept        { return backButton; }
    juce::TextButton& source() noexcept      { return sourcePill; }
    juce::TextButton& meter() noexcept       { return meterPill; }
    juce::TextButton& match() noexcept       { return matchPill; }
    juce::TextButton& keepPitch() noexcept   { return pitchPill; }
    RiserWaveform&    wave() noexcept        { return waveform; }
    SettingKnob*      noiseKnob (EffectSettings::Id id) noexcept
    {
        for (auto& k : noiseKnobs)
            if (k->settingId() == id)
                return k.get();
        return nullptr;
    }
    juce::String      getStatus() const      { return statusText(); }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = uiScale;
        g.setColour (Palette::chipSurface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f * s);

        g.setFont (Fonts::sans (8.5f * s, true, 0.08f));
        g.setColour (processor.getRiser().getTiming().matchProject ? Palette::textMid : Palette::textDimmer);
        g.drawText (shownStatus, statusArea.toFloat(), juce::Justification::centredRight, false);

        if (shownLength <= 0.0)
        {
            g.setFont (Fonts::sans (9.0f * s, false, 0.02f));
            g.setColour (Palette::textDim);
            g.drawFittedText ("The built-in noise sweeps from START to END as Amount rises.\n"
                              "Load a sample, or drop one on PreDrop, to use your own.",
                              noteArea, juce::Justification::centred, 2);
        }
    }

    void resized() override
    {
        const float s = uiScale;
        const auto px = [s] (float v) { return (int) std::round (v * s); };
        const bool sample = shownLength > 0.0;

        auto area = getLocalBounds().reduced (px (10.0f), px (8.0f));
        const int gap = px (6.0f);

        auto header = area.removeFromTop (px (14.0f));
        backButton.setBounds (header.removeFromLeft (px (46.0f)));
        header.removeFromLeft (gap);
        statusArea = header.removeFromRight (px (84.0f));
        header.removeFromRight (gap);
        sourcePill.setBounds (header);
        updateSourcePill();

        // Two rows of timing controls under the sample (none for the noise).
        for (juce::Component* c : { (juce::Component*) &tempoChip, (juce::Component*) &barsChip,
                                    (juce::Component*) &meterPill, (juce::Component*) &matchPill, (juce::Component*) &pitchPill })
            c->setVisible (sample);

        waveform.setVisible (sample);
        for (auto& k : noiseKnobs)
            k->setVisible (! sample);
        if (! sample)
        {
            // The noise's sweep: a line on what it does, then its three knobs.
            area.removeFromTop (px (4.0f));
            noteArea = area.removeFromTop (px (24.0f));
            const int colW   = area.getWidth() / 4;
            const int height = juce::jmin (area.getHeight(), px (72.0f));
            auto row = area.withSizeKeepingCentre (colW * (int) noiseKnobs.size(), height);
            for (auto& k : noiseKnobs)
                k->setBounds (row.removeFromLeft (colW).reduced (px (4.0f), 0));
            return;
        }

        auto toggles = area.removeFromBottom (px (14.0f));
        area.removeFromBottom (px (5.0f));
        auto numbers = area.removeFromBottom (px (14.0f));
        area.removeFromBottom (px (6.0f));

        const int third = (numbers.getWidth() - 2 * gap) / 3;
        tempoChip.setBounds (numbers.removeFromLeft (third));
        numbers.removeFromLeft (gap);
        barsChip.setBounds (numbers.removeFromLeft (third));
        numbers.removeFromLeft (gap);
        meterPill.setBounds (numbers);

        const int halfW = (toggles.getWidth() - gap) / 2;
        matchPill.setBounds (toggles.removeFromLeft (halfW));
        toggles.removeFromLeft (gap);
        pitchPill.setBounds (toggles);

        area.removeFromTop (px (6.0f));
        waveform.setBounds (area);
    }

private:
    // Applies a change to the riser's timing, then shows the result.
    template <typename Change>
    void changeTiming (Change&& change)
    {
        if (updating)
            return;
        auto t = processor.getRiser().getTiming();
        change (t);
        t.bpm = juce::jlimit (RiserTiming::minBpm, RiserTiming::maxBpm, t.bpm);
        processor.getRiser().setTiming (t);
        showTiming (processor.getRiser().getTiming());
        repaint();
    }

    void showTiming (const RiserTiming& t)
    {
        const juce::ScopedValueSetter<bool> quiet (updating, true);

        // The chip being dragged or typed into keeps its own value.
        if (! tempoChip.isMouseButtonDown() && ! tempoChip.entryLabel().isBeingEdited())
            tempoChip.setValue (t.bpm, juce::dontSendNotification);
        // BARS: how many loop, following the project; otherwise, dimmed, how
        // many the whole sample spans.
        if (! barsChip.isMouseButtonDown() && ! barsChip.entryLabel().isBeingEdited() && shownLength > 0.0)
            barsChip.setValue (t.matchProject ? (double) t.loopBarsFor (shownLength) : t.barsIn (shownLength),
                               juce::dontSendNotification);
        barsChip.setEnabled (t.matchProject);
        barsChip.setAlpha (t.matchProject ? 1.0f : 0.35f);
        tempoChip.repaint();
        barsChip.repaint();

        meterPill.setButtonText (juce::String (t.beatsPerBar) + "/" + juce::String (t.beatUnit));
        matchPill.setToggleState (t.matchProject, juce::dontSendNotification);
        pitchPill.setToggleState (t.matchProject && t.keepPitch, juce::dontSendNotification);
        pitchPill.setEnabled (t.matchProject);
        pitchPill.setAlpha (t.matchProject ? 1.0f : 0.35f);
        waveform.setTiming (t);
    }

    juce::String statusText() const
    {
        auto& riser = processor.getRiser();
        if (shownLength <= 0.0)
            return {};   // the source pill already says NOISE
        if (! riser.getTiming().matchProject)
            return "OWN TEMPO";
        const double project = riser.getHostTempo();
        return project > 0.0 ? "PROJECT " + NumberChip::format (project) + " BPM" : "NO PROJECT TEMPO";
    }

    void updateSourcePill()
    {
        // The sample's name, shortened to fit; lit while a sample is in use.
        const auto label = shownName.isEmpty() ? juce::String ("NOISE")
                                               : juce::File::createFileWithoutCheckingPath (shownName).getFileNameWithoutExtension().toUpperCase();
        sourcePill.setToggleState (shownName.isNotEmpty(), juce::dontSendNotification);
        sourcePill.setButtonText (PreDropLookAndFeel::fitPillLabel (label, true, (float) sourcePill.getWidth(),
                                                                    PreDropLookAndFeel::pillScale (true, (float) sourcePill.getHeight())));
        sourcePill.setTooltip (shownName.isEmpty() ? "Built-in noise riser. Click to load a sample." : "Riser sample: " + shownName + ". Click to change it.");
    }

    void showSourceMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Load riser sample...");
        menu.addItem (2, "Use built-in noise", shownName.isNotEmpty());

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sourcePill),
                            [safe = juce::Component::SafePointer (this)] (int choice)
        {
            if (safe == nullptr)
                return;
            if (choice == 2)
            {
                safe->processor.getRiser().clear();
                safe->refresh();
            }
            else if (choice == 1)
            {
                safe->chooseFile();
            }
        });
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a riser sample", juce::File(),
                                                       processor.getRiser().getFileWildcard());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer (this)] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult().existsAsFile())
                safe->loadFile (fc.getResult());
        });
    }

    void showMeterMenu()
    {
        const auto t = processor.getRiser().getTiming();
        juce::PopupMenu menu;
        menu.addSectionHeader ("The sample's meter");
        for (size_t i = 0; i < meters.size(); ++i)
        {
            const auto [beats, unit] = meters[i];
            menu.addItem ((int) i + 1, juce::String (beats) + "/" + juce::String (unit), true,
                          t.beatsPerBar == beats && t.beatUnit == unit);
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&meterPill),
                            [safe = juce::Component::SafePointer (this)] (int choice)
        {
            if (safe != nullptr && choice > 0)
                safe->setMeter (meters[(size_t) choice - 1].first, meters[(size_t) choice - 1].second);
        });
    }

    PreDropAudioProcessor& processor;
    PreDropLookAndFeel     pills;
    DepthKnobLookAndFeel   knobLookAndFeel;

    juce::TextButton backButton;
    juce::TextButton sourcePill;
    NumberChip       tempoChip { "BPM", RiserTiming::minBpm, RiserTiming::maxBpm };
    NumberChip       barsChip  { "BARS", 1.0, (double) RiserTiming::maxLoopBars };
    juce::TextButton meterPill;
    juce::TextButton matchPill;
    juce::TextButton pitchPill;
    RiserWaveform    waveform;
    std::vector<std::unique_ptr<SettingKnob>> noiseKnobs;   // START, END, RESONANCE
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Rectangle<int> statusArea;
    juce::Rectangle<int> noteArea;
    juce::String shownName;
    juce::String shownStatus;
    double       shownLength = 0.0;
    int          shownId     = -1;   // forces the first refresh to read the sample
    bool         updating    = false;
    float        uiScale     = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RiserView)
};
