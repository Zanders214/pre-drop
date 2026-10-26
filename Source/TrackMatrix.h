#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>

#include "PluginProcessor.h"
#include "PreDropTheme.h"

/**
    TrackMatrix: the main's TRACKS panel.

    Top: the ALL TRACKS / SELECTED TRACKS switch (the main's "applies to").
    With SELECTED TRACKS, one row per linked track with HPF / REV / DLY
    switches, plus a pinned ALL row that sets a column for every track. The
    switches are the tracks' own parameters, edited through LinkHub, so each
    track saves (and can automate) its own choices. Scrolls with the mouse
    wheel when there are more tracks than fit.
*/
class TrackMatrix : public juce::Component
{
public:
    explicit TrackMatrix (PreDropAudioProcessor& p) : processor (p) {}

    void setUiScale (float s) noexcept { uiScale = juce::jmax (0.1f, s); }

    /** Repaints when anything shown has changed. Called from the editor's timer. */
    void refresh()
    {
        if (const auto now = describe(); now != shown)
        {
            shown = now;
            repaint();
        }
    }

    // --- Geometry (local coordinates), shared by painting, clicks and tests ---
    juce::Rectangle<float> scopeButton (bool selectedTracks) const
    {
        auto bar = juce::Rectangle (pad(), pad(), (float) getWidth() - 2.0f * pad(), 16.0f * uiScale);
        return selectedTracks ? bar.removeFromRight (bar.getWidth() * 0.5f) : bar.removeFromLeft (bar.getWidth() * 0.5f);
    }

    /** Switch for `effect` on a row: row 0 is ALL, rows 1.. are visible tracks. */
    juce::Rectangle<float> cell (int row, LinkHub::Effect effect) const
    {
        const float colW = 40.0f * uiScale;
        const float x = (float) getWidth() - pad() - (float) (LinkHub::numEffects - (int) effect) * colW;
        const auto rowArea = juce::Rectangle (x, firstRowY() + (float) row * rowH(), colW, rowH());
        return rowArea.withSizeKeepingCentre (24.0f * uiScale, 10.0f * uiScale);
    }

    int visibleTrackRows() const noexcept
    {
        return juce::jmax (0, (int) (((float) getHeight() - pad() - firstRowY()) / rowH()) - 1);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace PreDrop;
        const float s = uiScale;

        g.setColour (Palette::chipSurface);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 12.0f * s);

        const auto message = [&] (const juce::String& text)
        {
            g.setFont (Fonts::sans (10.0f * s, false, 0.04f));
            g.setColour (Palette::textDim);
            g.drawFittedText (text, getLocalBounds().reduced ((int) (14.0f * s)).withTrimmedTop ((int) (22.0f * s)),
                              juce::Justification::centred, 3);
        };

        if (! processor.isLinkMain())
        {
            message ("Another PreDrop is the main.\nSet that one to TRACK (or remove it) to choose tracks here.");
            return;
        }

        paintScopeSwitch (g);

        if (! processor.appliesToSelectedTracks())
        {
            message ("Every effect applies to the whole mix.\nChoose SELECTED TRACKS to pick tracks.");
            return;
        }

        const auto tracks = processor.getLinkHub().getTracks();
        if (tracks.empty())
        {
            message ("No linked tracks yet. Add PreDrop to a track\nand switch it to TRACK in its header.");
            return;
        }

        // Column labels.
        g.setFont (Fonts::sans (9.0f * s, true, 0.12f));
        g.setColour (Palette::textDimmer);
        const auto labelRow = juce::Rectangle (pad(), firstRowY() - 12.0f * s, (float) getWidth() - 2.0f * pad(), 11.0f * s);
        g.drawText ("TRACK", labelRow, juce::Justification::centredLeft, false);
        for (int e = 0; e < LinkHub::numEffects; ++e)
        {
            const auto c = cell (0, (LinkHub::Effect) e);
            g.drawText (columnNames[(size_t) e], labelRow.withX (c.getX() - 8.0f * s).withWidth (c.getWidth() + 16.0f * s),
                        juce::Justification::centred, false);
        }

        // Pinned ALL row: lit when every track has the effect, dim when some do.
        paintRow (g, 0, "ALL", [&] (LinkHub::Effect e)
        {
            int on = 0;
            for (auto* t : tracks)
                on += t->isLinkEffectOn (e) ? 1 : 0;
            return on == (int) tracks.size() ? 1.0f : (on > 0 ? 0.45f : 0.0f);
        });

        const int first = juce::jlimit (0, juce::jmax (0, (int) tracks.size() - visibleTrackRows()), scroll);
        for (int r = 0; r < visibleTrackRows() && first + r < (int) tracks.size(); ++r)
        {
            auto* track = tracks[(size_t) (first + r)];
            paintRow (g, r + 1, track->getLinkName(), [track] (LinkHub::Effect e) { return track->isLinkEffectOn (e) ? 1.0f : 0.0f; });
        }

        if ((int) tracks.size() > visibleTrackRows())
        {
            g.setFont (Fonts::mono (8.5f * s));
            g.setColour (Palette::textDimmer);
            g.drawText (juce::String (first + 1) + "-" + juce::String (juce::jmin ((int) tracks.size(), first + visibleTrackRows()))
                          + " of " + juce::String ((int) tracks.size()) + "  (scroll)",
                        juce::Rectangle (pad(), labelRow.getY(), (float) getWidth() - 2.0f * pad(), labelRow.getHeight())
                            .withRight (cell (0, LinkHub::Effect::hpf).getX() - 14.0f * s),
                        juce::Justification::centredRight, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! processor.isLinkMain())
            return;

        const auto pos = e.position;
        for (const bool selected : { false, true })
            if (scopeButton (selected).contains (pos))
            {
                processor.setAppliesToSelectedTracks (selected);
                refresh();
                return;
            }

        if (! processor.appliesToSelectedTracks())
            return;

        const auto tracks = processor.getLinkHub().getTracks();
        const int first = juce::jlimit (0, juce::jmax (0, (int) tracks.size() - visibleTrackRows()), scroll);

        for (int col = 0; col < LinkHub::numEffects; ++col)
        {
            const auto effect = (LinkHub::Effect) col;

            if (cell (0, effect).expanded (4.0f * uiScale).contains (pos) && ! tracks.empty())
            {
                // ALL: switch the column on everywhere, or off if it already is.
                const bool allOn = std::all_of (tracks.begin(), tracks.end(), [effect] (auto* t) { return t->isLinkEffectOn (effect); });
                for (auto* t : tracks)
                    t->setLinkEffectOn (effect, ! allOn);
                refresh();
                return;
            }

            for (int r = 0; r < visibleTrackRows() && first + r < (int) tracks.size(); ++r)
                if (cell (r + 1, effect).expanded (4.0f * uiScale).contains (pos))
                {
                    auto* track = tracks[(size_t) (first + r)];
                    track->setLinkEffectOn (effect, ! track->isLinkEffectOn (effect));
                    refresh();
                    return;
                }
        }
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const int tracks = (int) processor.getLinkHub().getTracks().size();
        scroll = juce::jlimit (0, juce::jmax (0, tracks - visibleTrackRows()), scroll + (wheel.deltaY < 0.0f ? 1 : -1));
        refresh();
    }

private:
    static constexpr std::array<const char*, LinkHub::numEffects> columnNames { "HPF", "REV", "DLY" };

    // Compact enough for the ALL row plus three tracks in the 114 px slot.
    float pad() const noexcept       { return 8.0f * uiScale; }
    float rowH() const noexcept      { return 14.0f * uiScale; }
    float firstRowY() const noexcept { return pad() + 16.0f * uiScale + 17.0f * uiScale; }

    static juce::Colour effectColour (LinkHub::Effect e)
    {
        using namespace PreDrop::Palette;
        switch (e)
        {
            case LinkHub::Effect::reverb: return violet;
            case LinkHub::Effect::delay:  return pink;
            case LinkHub::Effect::hpf:    break;
        }
        return cyan;
    }

    void paintScopeSwitch (juce::Graphics& g) const
    {
        using namespace PreDrop;
        const float s = uiScale;
        const bool selected = processor.appliesToSelectedTracks();

        for (const bool half : { false, true })
        {
            const auto r = scopeButton (half).reduced (1.0f * s);
            const bool on = half == selected;
            g.setColour (on ? Palette::badge.withAlpha (0.28f) : juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.05f));
            g.fillRoundedRectangle (r, 8.0f * s);
            g.setColour (on ? Palette::badge : Palette::hairline);
            g.drawRoundedRectangle (r, 8.0f * s, juce::jmax (1.0f, s));
            g.setFont (Fonts::sans (9.5f * s, true, 0.12f));
            g.setColour (on ? Palette::textStrong : Palette::textDim);
            g.drawText (half ? "SELECTED TRACKS" : "ALL TRACKS", r, juce::Justification::centred, false);
        }
    }

    template <typename LitAmount>
    void paintRow (juce::Graphics& g, int row, const juce::String& name, LitAmount lit) const
    {
        using namespace PreDrop;
        const float s = uiScale;
        const auto first = cell (row, LinkHub::Effect::hpf);
        const auto nameArea = juce::Rectangle (pad(), first.getCentreY() - rowH() * 0.5f,
                                               first.getX() - pad() - 14.0f * s, rowH());

        g.setFont (row == 0 ? Fonts::sans (9.5f * s, true, 0.12f) : Fonts::sans (10.5f * s, false, 0.02f));
        g.setColour (row == 0 ? Palette::textMid : Palette::textStrong);
        g.drawText (name, nameArea, juce::Justification::centredLeft, true);

        for (int e = 0; e < LinkHub::numEffects; ++e)
        {
            const auto effect = (LinkHub::Effect) e;
            const auto pill = cell (row, effect);
            const float amount = lit (effect);
            if (amount > 0.0f)
            {
                g.setColour (effectColour (effect).withAlpha (amount));
                g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
            }
            g.setColour (amount > 0.0f ? effectColour (effect) : Palette::hairline.brighter (0.4f));
            g.drawRoundedRectangle (pill, pill.getHeight() * 0.5f, juce::jmax (1.0f, s));
        }
    }

    // Everything the panel shows, so refresh() can tell when to repaint.
    juce::String describe() const
    {
        juce::String d;
        d << (processor.isLinkMain() ? "M" : "-") << (processor.appliesToSelectedTracks() ? "S" : "A") << scroll << "|";
        for (auto* t : processor.getLinkHub().getTracks())
        {
            d << t->getLinkName() << ":";
            for (int e = 0; e < LinkHub::numEffects; ++e)
                d << (t->isLinkEffectOn ((LinkHub::Effect) e) ? "1" : "0");
            d << "|";
        }
        return d;
    }

    PreDropAudioProcessor& processor;
    float        uiScale = 1.0f;
    int          scroll  = 0;
    juce::String shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackMatrix)
};
