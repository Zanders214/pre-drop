#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <array>
#include <utility>
#include <vector>

#include "PluginProcessor.h"
#include "PreDropTheme.h"

/**
    TrackMatrix: the main's TRACKS panel.

    Top: the ALL TRACKS / SELECTED TRACKS switch (the main's "applies to").
    With SELECTED TRACKS, one row per linked track with an HPF / REV / DLY
    level each, plus a pinned ALL row that sets a column for every track.
    A level is that track's share of the main's depth for the effect (100% =
    all of it, 0% = off): drag a cell up or down to set it, click to toggle
    between off and 100%, double-click to reset to 100%. The levels are the
    tracks' own parameters, edited through LinkHub, so each track saves (and can
    automate) its own. Scrolls with the mouse wheel when the list is long.
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

    /** Level cell for `effect` on a row: row 0 is ALL, rows 1.. are visible tracks. */
    juce::Rectangle<float> cell (int row, LinkHub::Effect effect) const
    {
        const float colW = 44.0f * uiScale;
        const float x = (float) getWidth() - pad() - (float) (LinkHub::numEffects - (int) effect) * colW;
        const auto rowArea = juce::Rectangle (x, firstRowY() + (float) row * rowH(), colW, rowH());
        return rowArea.withSizeKeepingCentre (36.0f * uiScale, 12.0f * uiScale);
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

        // Column labels, and how far the list is scrolled when it is long.
        g.setFont (Fonts::sans (9.0f * s, true, 0.12f));
        g.setColour (Palette::textDimmer);
        const auto labelRow = juce::Rectangle (pad(), firstRowY() - 12.0f * s, (float) getWidth() - 2.0f * pad(), 11.0f * s);
        g.drawText ("TRACK", labelRow, juce::Justification::centredLeft, false);
        for (int e = 0; e < LinkHub::numEffects; ++e)
        {
            const auto c = cell (0, (LinkHub::Effect) e);
            g.drawText (columnNames[(size_t) e], labelRow.withX (c.getX()).withWidth (c.getWidth()),
                        juce::Justification::centred, false);
        }

        const int first = firstVisible ((int) tracks.size());
        if ((int) tracks.size() > visibleTrackRows())
        {
            g.setFont (Fonts::mono (8.5f * s));
            g.drawText (juce::String (first + 1) + "-" + juce::String (juce::jmin ((int) tracks.size(), first + visibleTrackRows()))
                          + " of " + juce::String ((int) tracks.size()) + "  (scroll)",
                        labelRow.withRight (cell (0, LinkHub::Effect::hpf).getX() - 10.0f * s),
                        juce::Justification::centredRight, false);
        }

        // Pinned ALL row: the column's level when every track shares it, else
        // their average, dimmed.
        paintRow (g, 0, "ALL", [&] (LinkHub::Effect e) { return columnLevel (tracks, e); });

        for (int r = 0; r < visibleTrackRows() && first + r < (int) tracks.size(); ++r)
        {
            auto* track = tracks[(size_t) (first + r)];
            paintRow (g, r + 1, track->getLinkName(), [track] (LinkHub::Effect e)
            {
                return std::pair { track->getLinkLevel (e), true };
            });
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        drag = {};
        if (! processor.isLinkMain())
            return;

        for (const bool selected : { false, true })
            if (scopeButton (selected).contains (e.position))
            {
                processor.setAppliesToSelectedTracks (selected);
                refresh();
                return;
            }

        if (! processor.appliesToSelectedTracks())
            return;

        // Remember which cell was grabbed and its level, for a drag or a click.
        const auto tracks = processor.getLinkHub().getTracks();
        if (const auto hit = cellAt (e.position, tracks); hit.row >= 0)
        {
            drag = hit;
            drag.startLevel = hit.row == 0 ? columnLevel (tracks, hit.effect).first
                                           : rowTrack (tracks, hit.row)->getLinkLevel (hit.effect);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (drag.row < 0 || e.getDistanceFromDragStart() < 3)
            return;

        // 60 px of travel sweeps the full 0..100%.
        drag.moved = true;
        const float level = juce::jlimit (0.0f, 1.0f, drag.startLevel - (float) e.getDistanceFromDragStartY() / (60.0f * uiScale));
        applyLevel (drag, level);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // A click without a drag toggles: anything on goes off, off goes to 100%.
        if (drag.row >= 0 && ! drag.moved)
            applyLevel (drag, drag.startLevel > 0.0f ? 0.0f : 1.0f);
        drag = {};
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! processor.isLinkMain() || ! processor.appliesToSelectedTracks())
            return;
        if (const auto hit = cellAt (e.position, processor.getLinkHub().getTracks()); hit.row >= 0)
            applyLevel (hit, 1.0f);
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const int tracks = (int) processor.getLinkHub().getTracks().size();
        scroll = juce::jlimit (0, juce::jmax (0, tracks - visibleTrackRows()), scroll + (wheel.deltaY < 0.0f ? 1 : -1));
        refresh();
    }

private:
    static constexpr std::array<const char*, LinkHub::numEffects> columnNames { "HPF", "REV", "DLY" };
    using Tracks = std::vector<LinkHub::LinkedTrack*>;

    // A grabbed cell: row 0 is ALL, -1 is none.
    struct Grab
    {
        int             row        = -1;
        LinkHub::Effect effect     = LinkHub::Effect::hpf;
        float           startLevel = 0.0f;
        bool            moved      = false;
    };

    float pad() const noexcept       { return 8.0f * uiScale; }
    float rowH() const noexcept      { return 14.0f * uiScale; }
    float firstRowY() const noexcept { return pad() + 16.0f * uiScale + 17.0f * uiScale; }

    int firstVisible (int numTracks) const noexcept
    {
        return juce::jlimit (0, juce::jmax (0, numTracks - visibleTrackRows()), scroll);
    }

    LinkHub::LinkedTrack* rowTrack (const Tracks& tracks, int row) const
    {
        return tracks[(size_t) (firstVisible ((int) tracks.size()) + row - 1)];
    }

    Grab cellAt (juce::Point<float> pos, const Tracks& tracks) const
    {
        const int rows = juce::jmin (visibleTrackRows(), (int) tracks.size());
        for (int row = 0; ! tracks.empty() && row <= rows; ++row)
            for (int col = 0; col < LinkHub::numEffects; ++col)
                if (cell (row, (LinkHub::Effect) col).expanded (3.0f * uiScale).contains (pos))
                    return { row, (LinkHub::Effect) col, 0.0f, false };
        return {};
    }

    void applyLevel (const Grab& target, float level)
    {
        const auto tracks = processor.getLinkHub().getTracks();
        if (target.row == 0)
            for (auto* t : tracks)
                t->setLinkLevel (target.effect, level);
        else if (target.row <= juce::jmin (visibleTrackRows(), (int) tracks.size()))
            rowTrack (tracks, target.row)->setLinkLevel (target.effect, level);
        refresh();
    }

    // Average level of a column, and whether every track shares it.
    static std::pair<float, bool> columnLevel (const Tracks& tracks, LinkHub::Effect e)
    {
        if (tracks.empty())
            return { 0.0f, true };
        float sum = 0.0f, lo = 1.0f, hi = 0.0f;
        for (auto* t : tracks)
        {
            const float level = t->getLinkLevel (e);
            sum += level;
            lo = std::min (lo, level);
            hi = std::max (hi, level);
        }
        return { sum / (float) tracks.size(), hi - lo < 0.005f };
    }

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

    // One row: the name, then a level bar per effect, filled to the level in
    // the effect's colour with the percentage on top ("off" at 0).
    template <typename LevelOf>
    void paintRow (juce::Graphics& g, int row, const juce::String& name, LevelOf levelOf) const
    {
        using namespace PreDrop;
        const float s = uiScale;
        const auto first = cell (row, LinkHub::Effect::hpf);
        const auto nameArea = juce::Rectangle (pad(), first.getCentreY() - rowH() * 0.5f,
                                               first.getX() - pad() - 8.0f * s, rowH());

        g.setFont (row == 0 ? Fonts::sans (9.5f * s, true, 0.12f) : Fonts::sans (10.5f * s, false, 0.02f));
        g.setColour (row == 0 ? Palette::textMid : Palette::textStrong);
        g.drawText (name, nameArea, juce::Justification::centredLeft, true);

        for (int e = 0; e < LinkHub::numEffects; ++e)
        {
            const auto effect = (LinkHub::Effect) e;
            const auto bar = cell (row, effect);
            const auto [level, uniform] = levelOf (effect);
            const auto colour = effectColour (effect).withMultipliedAlpha (uniform ? 1.0f : 0.55f);
            const float radius = bar.getHeight() * 0.5f;

            g.setColour (juce::Colour::fromFloatRGBA (1.0f, 1.0f, 1.0f, 0.04f));
            g.fillRoundedRectangle (bar, radius);
            if (level > 0.0f)
            {
                g.setColour (colour.withMultipliedAlpha (0.7f));
                g.fillRoundedRectangle (bar.withWidth (juce::jmax (bar.getHeight(), bar.getWidth() * level)), radius);
            }
            g.setColour (level > 0.0f ? colour : Palette::hairline.brighter (0.4f));
            g.drawRoundedRectangle (bar, radius, juce::jmax (1.0f, s));

            g.setFont (Fonts::mono (8.0f * s));
            g.setColour (level > 0.0f ? Palette::textStrong : Palette::textDimmer);
            g.drawText (level > 0.0f ? juce::String (juce::roundToInt (level * 100.0f)) + "%" : juce::String ("off"),
                        bar, juce::Justification::centred, false);
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
                d << juce::roundToInt (t->getLinkLevel ((LinkHub::Effect) e) * 100.0f) << ",";
            d << "|";
        }
        return d;
    }

    PreDropAudioProcessor& processor;
    float        uiScale = 1.0f;
    int          scroll  = 0;
    Grab         drag;
    juce::String shown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackMatrix)
};
