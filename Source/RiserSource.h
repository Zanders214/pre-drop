#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <atomic>

#include "PreDropEngine.h"

/**
    RiserSource: the optional riser sample, in place of the built-in noise.

    The original file's bytes are kept and saved inside the plugin state, so a
    project carries its riser with it, and the decoded audio is rebuilt for the
    engine at the host's sample rate.

    Guarded by a lock, since hosts may call prepareToPlay / get/setStateInformation
    off the message thread; never touched by the audio thread (the engine gets
    its own RiserSample).
*/
class RiserSource
{
public:
    explicit RiserSource (PreDropEngine& engineToFeed);

    /** Uses an audio file as the riser. Returns an explanation if it can't be
        used, or an empty string on success. */
    juce::String load (const juce::File& file);

    /** Goes back to the built-in noise riser. */
    void clear();

    /** File name of the sample, or empty while the built-in noise is used. */
    juce::String getName() const;

    /** True for files the riser can load, judged by extension. */
    bool isSupportedFile (const juce::File& file) const;

    /** Wildcard of every loadable extension, for a file chooser. */
    juce::String getFileWildcard() const { return formats.getWildcardForAllFormats(); }

    static constexpr double maxSeconds = 60.0;

    /** Rebuilds the sample for the engine at this rate (from prepareToPlay). */
    void setSampleRate (double rate);

    /** Adds the sample (if one is loaded) to a state tree being saved. */
    void saveTo (juce::ValueTree& state) const;

    /** Restores from a saved state tree: its sample, or the noise if it has none. */
    void restoreFrom (const juce::ValueTree& state);

    /** The state child that carries the sample. */
    static const juce::Identifier stateTag;

private:
    juce::String loadFromData (const juce::MemoryBlock& fileData, const juce::String& name);
    void sendToEngine();

    PreDropEngine& engine;
    juce::AudioFormatManager formats;
    std::atomic<double> sampleRate { 0.0 };   // 0 until prepareToPlay

    // The loaded sample: original file bytes (what the plugin state saves),
    // name, and decoded audio at the file's own sample rate.
    struct Loaded
    {
        juce::MemoryBlock        fileData;
        juce::String             name;
        juce::AudioBuffer<float> audio;
        double                   sampleRate = 0.0;
    };
    juce::CriticalSection lock;
    Loaded loaded;

    JUCE_DECLARE_NON_COPYABLE (RiserSource)
};
