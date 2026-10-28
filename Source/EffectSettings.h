#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

/**
    EffectSettings: how each built-in effect sounds, beside how far Amount and
    its depth take it.

      - REVERB: SIZE (the room at full Amount; it grows towards it), DAMPING,
        WIDTH, PRE-DELAY.
      - DELAY: TIME (a note value at the project's tempo, or milliseconds),
        FEEDBACK (at full Amount; it grows towards it), TONE (a low-pass in the
        repeats) and PING-PONG.
      - HIGH-PASS: SLOPE (12 or 24 dB per octave) and RESONANCE.
      - RISER (the built-in noise): where its sweep STARTs and ENDs, and its
        RESONANCE.
      - DOWNLIFTER (after the drop): whether there is one, its LENGTH in bars,
        its LEVEL, where its noise FALLs TO, and how much of the build-up's
        REVERB and DELAY ring on through it (see DropRelease).

    Each is a plugin parameter (specs lists them, with their ranges and
    defaults, which are the sound PreDrop had before they existed). The values
    are kept as the parameters' plain values: a toggle is 0 / 1 and a choice
    its index. Plain data, so the audio thread can copy it.
*/
struct EffectSettings
{
    enum class Id
    {
        reverbSize, reverbDamping, reverbWidth, reverbPreDelay,
        delaySync, delayNote, delayTime, delayFeedback, delayTone, delayPingPong,
        hpfSlope, hpfResonance,
        riserStart, riserEnd, riserResonance,
        dropOn, dropLength, dropLevel, dropEnd, dropReverb, dropDelay,
        count
    };
    static constexpr size_t count = static_cast<size_t> (Id::count);

    enum class Kind { percent, milliseconds, hertz, toggle, note, slope, bars };

    struct Spec
    {
        const char* id;            // parameter ID
        const char* name;          // its name in the host
        const char* label;         // its name on PreDrop's face
        Kind        kind;
        float       min;
        float       max;
        float       defaultValue;
        float       centre;        // the value mid-way along a control, or 0 for even steps
    };

    // The delay's note values, in quarter notes (T: triplet, D: dotted).
    static constexpr int numNotes = 13;
    static constexpr std::array<const char*, numNotes> noteNames { "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D",
                                                                   "1/4T", "1/4", "1/4D", "1/2", "1/2D", "1/1" };
    static constexpr std::array<double, numNotes> noteQuarters { 0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75,
                                                                 2.0 / 3.0, 1.0, 1.5, 2.0, 3.0, 4.0 };

    // The downlifter's lengths, in bars.
    static constexpr int numLengths = 4;
    static constexpr std::array<const char*, numLengths> lengthNames { "1/2 BAR", "1 BAR", "2 BARS", "4 BARS" };
    static constexpr std::array<double, numLengths> lengthBars { 0.5, 1.0, 2.0, 4.0 };

    static constexpr float toneOpenHz       = 20000.0f;   // TONE at the top: no filtering
    static constexpr double maxDelaySeconds = 4.0;        // the longest echo (1/1 at 60 BPM)
    static constexpr double fallbackBpm     = 120.0;      // a synced delay with no project tempo

    static constexpr std::array<Spec, count> specs { {
        { "reverbSize",     "Reverb Size",      "SIZE",      Kind::percent,      0.1f,    1.0f,     0.95f,     0.0f },
        { "reverbDamping",  "Reverb Damping",   "DAMPING",   Kind::percent,      0.0f,    1.0f,     0.4f,      0.0f },
        { "reverbWidth",    "Reverb Width",     "WIDTH",     Kind::percent,      0.0f,    1.0f,     1.0f,      0.0f },
        { "reverbPreDelay", "Reverb Pre-Delay", "PRE-DELAY", Kind::milliseconds, 0.0f,    250.0f,   0.0f,      50.0f },
        { "delaySync",      "Delay Sync",       "SYNC",      Kind::toggle,       0.0f,    1.0f,     1.0f,      0.0f },
        { "delayNote",      "Delay Note",       "TIME",      Kind::note,         0.0f,    12.0f,    6.0f,      0.0f },
        { "delayTime",      "Delay Time",       "TIME",      Kind::milliseconds, 10.0f,   2000.0f,  375.0f,    300.0f },
        { "delayFeedback",  "Delay Feedback",   "FEEDBACK",  Kind::percent,      0.0f,    0.9f,     0.6f,      0.0f },
        { "delayTone",      "Delay Tone",       "TONE",      Kind::hertz,        500.0f,  20000.0f, 20000.0f,  3000.0f },
        { "delayPingPong",  "Delay Ping-Pong",  "PING-PONG", Kind::toggle,       0.0f,    1.0f,     0.0f,      0.0f },
        { "hpfSlope",       "High-Pass Slope",  "SLOPE",     Kind::slope,        0.0f,    1.0f,     0.0f,      0.0f },
        { "hpfResonance",   "High-Pass Resonance", "RESONANCE", Kind::percent,   0.0f,    1.0f,     0.0f,      0.0f },
        { "riserStart",     "Riser Start",      "START",     Kind::hertz,        20.0f,   2000.0f,  200.0f,    200.0f },
        { "riserEnd",       "Riser End",        "END",       Kind::hertz,        1000.0f, 16000.0f, 8000.0f,   4000.0f },
        { "riserResonance", "Riser Resonance",  "RESONANCE", Kind::percent,      0.0f,    1.0f,     0.396241f, 0.0f },   // Q 1.5
        { "dropOn",         "Downlifter",       "DOWNLIFTER", Kind::toggle,      0.0f,    1.0f,     1.0f,      0.0f },
        { "dropLength",     "Downlifter Length", "LENGTH",   Kind::bars,         0.0f,    3.0f,     2.0f,      0.0f },
        { "dropLevel",      "Downlifter Level", "LEVEL",     Kind::percent,      0.0f,    1.0f,     0.5f,      0.0f },
        { "dropEnd",        "Downlifter Fall To", "FALL TO", Kind::hertz,        30.0f,   2000.0f,  150.0f,    200.0f },
        { "dropReverb",     "Downlifter Reverb", "REVERB",   Kind::percent,      0.0f,    1.0f,     1.0f,      0.0f },
        { "dropDelay",      "Downlifter Delay", "DELAY",     Kind::percent,      0.0f,    1.0f,     1.0f,      0.0f },
    } };

    EffectSettings() noexcept
    {
        for (size_t i = 0; i < values.size(); ++i)
            values[i] = specs[i].defaultValue;
    }

    float operator[] (Id id) const noexcept { return values[(size_t) id]; }
    void  set (Id id, float value) noexcept { values[(size_t) id] = juce::jlimit (specs[(size_t) id].min, specs[(size_t) id].max, value); }
    bool  isOn (Id id) const noexcept       { return values[(size_t) id] >= 0.5f; }

    friend bool operator== (const EffectSettings& a, const EffectSettings& b) noexcept { return a.values == b.values; }

    // --- What the values mean -------------------------------------------------
    int note() const noexcept { return juce::jlimit (0, numNotes - 1, juce::roundToInt ((*this)[Id::delayNote])); }

    /** The delay time: the note at `bpm` (or a stand-in tempo while there is
        none) when synced, else the milliseconds; never past the longest. */
    double delaySeconds (double bpm) const noexcept
    {
        double seconds = (*this)[Id::delayTime] / 1000.0;
        if (isOn (Id::delaySync))
        {
            const double tempo = bpm > 0.0 ? bpm : fallbackBpm;
            seconds = noteQuarters[(size_t) note()] * 60.0 / tempo;
        }
        return juce::jlimit (0.001, maxDelaySeconds, seconds);
    }

    bool  hpfSteep() const noexcept  { return isOn (Id::hpfSlope); }

    /** The high-pass's Q: 0.71 (flat, no peak) up to 8.5 at full resonance. */
    float hpfQ() const noexcept      { return 0.70710678f * std::pow (12.0f, (*this)[Id::hpfResonance]); }

    /** The downlifter's length in bars. */
    double dropBars() const noexcept
    {
        return lengthBars[(size_t) juce::jlimit (0, numLengths - 1, juce::roundToInt ((*this)[Id::dropLength]))];
    }

    /** The riser band-pass's Q: 0.5 (wide) to 8 (a whistle); 1.5 by default. */
    float riserQ() const noexcept    { return 0.5f * std::pow (16.0f, (*this)[Id::riserResonance]); }

    /** A value as PreDrop shows it: "95%", "120 ms", "1/8D", "4.0 kHz"... */
    static juce::String format (Id id, float value)
    {
        switch (specs[(size_t) id].kind)
        {
            case Kind::percent:      return juce::String (juce::roundToInt (value * 100.0f)) + "%";
            case Kind::milliseconds: return juce::String (juce::roundToInt (value)) + " ms";
            case Kind::toggle:       return value >= 0.5f ? "ON" : "OFF";
            case Kind::note:         return noteNames[(size_t) juce::jlimit (0, numNotes - 1, juce::roundToInt (value))];
            case Kind::slope:        return value >= 0.5f ? "24 dB" : "12 dB";
            case Kind::bars:         return lengthNames[(size_t) juce::jlimit (0, numLengths - 1, juce::roundToInt (value))];
            case Kind::hertz:        break;
        }
        if (id == Id::delayTone && value >= toneOpenHz - 1.0f)
            return "OPEN";
        if (value < 1000.0f)
            return juce::String (juce::roundToInt (value)) + " Hz";
        const int decimals = value >= 10000.0f ? 1 : 2;
        return juce::String (value / 1000.0f, decimals) + " kHz";
    }

    std::array<float, count> values {};
};
