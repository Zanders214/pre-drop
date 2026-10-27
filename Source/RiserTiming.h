#pragma once

#include <juce_core/juce_core.h>
#include <cmath>

/**
    RiserTiming: how a riser sample sits in musical time.

    The sample's own tempo and meter, which place its bar and beat lines, and
    how it plays against the project: at the project's tempo or its own, and,
    when it follows the project, whether its pitch stays put (time-stretched)
    or moves with the tempo (played faster or slower, like tape).

    Tempos are in quarter notes per minute, as hosts report them, whatever the
    meter.
*/
struct RiserTiming
{
    double bpm          = 120.0;   // the sample's own tempo
    int    beatsPerBar  = 4;       // meter: beats per bar ...
    int    beatUnit     = 4;       // ... of this note value (4 = quarter, 8 = eighth)
    bool   matchProject = true;    // play at the project's tempo
    bool   keepPitch    = true;    // ... time-stretched, rather than sped up / slowed down

    static constexpr double minBpm = 40.0;
    static constexpr double maxBpm = 300.0;

    double quarterNotesPerBar() const noexcept { return beatsPerBar * 4.0 / beatUnit; }
    double barSeconds() const noexcept         { return quarterNotesPerBar() * 60.0 / bpm; }
    double beatSeconds() const noexcept        { return barSeconds() / beatsPerBar; }

    /** How many bars `seconds` of the sample span at its own tempo. */
    double barsIn (double seconds) const noexcept { return seconds / barSeconds(); }

    /** The whole bars a loop of `seconds` plays as (at least one). */
    int wholeBarsIn (double seconds) const noexcept
    {
        return juce::jmax (1, (int) std::lround (barsIn (seconds)));
    }

    /** The tempo at which `seconds` of the sample is exactly `bars` bars. */
    double bpmForBars (double seconds, double bars) const noexcept
    {
        return juce::jlimit (minBpm, maxBpm, bars * quarterNotesPerBar() * 60.0 / seconds);
    }

    bool operator== (const RiserTiming& o) const noexcept
    {
        return juce::approximatelyEqual (bpm, o.bpm) && beatsPerBar == o.beatsPerBar && beatUnit == o.beatUnit
            && matchProject == o.matchProject && keepPitch == o.keepPitch;
    }
    bool operator!= (const RiserTiming& o) const noexcept { return ! (*this == o); }

    /** A first guess for a freshly loaded sample: a tempo written in its name
        ("128 BPM", "128bpm"), else a bar count in its name ("8 Bars") over its
        length, else the power-of-two bar count (in 4/4) that gives a tempo
        nearest 120. */
    static RiserTiming guess (const juce::String& fileName, double lengthSeconds)
    {
        RiserTiming t;
        if (lengthSeconds <= 0.0)
            return t;

        const auto name = juce::File::createFileWithoutCheckingPath (fileName).getFileNameWithoutExtension().toLowerCase();

        if (const double named = numberBefore (name, "bpm"); named >= minBpm && named <= maxBpm)
        {
            t.bpm = named;
            return t;
        }

        if (const double bars = numberBefore (name, "bar"); bars >= 1.0)
        {
            t.bpm = t.bpmForBars (lengthSeconds, bars);
            return t;
        }

        double best = t.bpm;
        for (int bars = 1; bars <= 64; bars *= 2)
        {
            const double candidate = bars * t.quarterNotesPerBar() * 60.0 / lengthSeconds;
            if (std::abs (std::log2 (candidate / 120.0)) < std::abs (std::log2 (best / 120.0)) || bars == 1)
                best = candidate;
        }
        t.bpm = juce::jlimit (minBpm, maxBpm, best);
        return t;
    }

private:
    // The number written just before `word` ("128 bpm", "8-bars", "128bpm"), or 0.
    static double numberBefore (const juce::String& text, const juce::String& word)
    {
        for (int at = text.indexOf (word); at >= 0; at = text.indexOf (at + 1, word))
        {
            int end = at;
            while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '-' || text[end - 1] == '_'))
                --end;
            int start = end;
            while (start > 0 && (juce::CharacterFunctions::isDigit (text[start - 1]) || text[start - 1] == '.'))
                --start;
            if (start < end)
                return text.substring (start, end).getDoubleValue();
        }
        return 0.0;
    }
};
