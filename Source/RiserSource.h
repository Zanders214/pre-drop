#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_data_structures/juce_data_structures.h>
#include <atomic>
#include <memory>
#include <vector>

#include "PreDropEngine.h"
#include "RiserTiming.h"

/**
    RiserSource: the optional riser sample, in place of the built-in noise.

    The original file's bytes are kept and saved inside the plugin state, so a
    project carries its riser with it, along with its timing (RiserTiming).
    From the decoded audio it renders what the engine plays: resampled to the
    host's rate and, when it follows the project, looped in whole bars and,
    to keep its pitch, time-stretched to the project's tempo.

    That stretch depends on the project's tempo, which the processor reports
    every block. When it changes the sample is re-rendered in the background;
    meanwhile the engine plays the previous rendering slightly faster or
    slower, so it stays in time. Loading, a new sample rate and a state
    restore render straight away, so playback starts with the right one.

    Guarded by a lock, since hosts may call prepareToPlay / get/setStateInformation
    off the message thread; never touched by the audio thread (the engine gets
    its own RiserSample) except for setHostTempo().
*/
class RiserSource
{
public:
    explicit RiserSource (PreDropEngine& engineToFeed);
    ~RiserSource();

    /** Uses an audio file as the riser. Returns an explanation if it can't be
        used, or an empty string on success. Its timing is guessed from its
        name and length (see RiserTiming::guess). */
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

    // --- Timing (message thread) -----------------------------------------------
    RiserTiming getTiming() const;

    /** Changes the timing; the sample is re-rendered in the background. */
    void setTiming (const RiserTiming& timing);

    /** The sample's length, or 0 for the built-in noise. */
    double getLengthSeconds() const;

    /** The sample's outline for drawing: the lowest and highest value in each
        of a fixed number of columns across it (empty for the built-in noise). */
    struct Outline
    {
        std::vector<float> low, high;
    };
    Outline getOutline() const;

    /** Changes whenever another sample is loaded (0 for the built-in noise). */
    int getSampleId() const;

    /** The project's tempo as last reported, or 0 if the host gives none. */
    double getHostTempo() const noexcept { return hostTempo.load(); }

    // --- For the processor -------------------------------------------------------
    /** Renders the sample for the engine at this rate (from prepareToPlay). */
    void setSampleRate (double rate);

    /** The project's tempo this block (0: unknown). Audio thread. */
    void setHostTempo (double bpm) noexcept { hostTempo.store (bpm); }

    /** Starts a background re-render if the project's tempo has moved away
        from the one the sample was stretched to. Message thread, regularly. */
    void update();

    /** Re-renders right away if the tempo moved, or a background rendering
        is still on its way: for offline rendering (a bounce), where waiting
        would leave the start out of time. */
    void renderIfStale();

    /** Waits for a background re-render to finish. For tests. */
    bool waitForRender (int timeoutMs);

    /** Adds the sample (if one is loaded) to a state tree being saved. */
    void saveTo (juce::ValueTree& state) const;

    /** Restores from a saved state tree: its sample, or the noise if it has none. */
    void restoreFrom (const juce::ValueTree& state);

    /** The state child that carries the sample. */
    static const juce::Identifier stateTag;

    // --- Rendering ---------------------------------------------------------------
    /** Everything one rendering needs (taken under the lock). */
    struct Request
    {
        std::shared_ptr<const juce::AudioBuffer<float>> audio;
        double       sourceRate = 0.0;
        juce::String name;
        int          sourceId   = 0;
        RiserTiming  timing;
        double       hostTempo  = 0.0;
        double       engineRate = 0.0;
        int          generation = 0;
    };

    /** What the engine plays for a request: the whole sample at its own speed;
        or, following the project, whole bars of it, time-stretched to the
        project's tempo to keep its pitch, or at its own tempo (with that tempo
        set, so the engine plays it faster or slower). Null if it's silent. */
    static std::unique_ptr<RiserSample> render (const Request& request);

private:

    // `savedTiming` (from a saved set) or, without it, a guess that follows
    // the project or not.
    juce::String loadFromData (const juce::MemoryBlock& fileData, const juce::String& name,
                               const RiserTiming* savedTiming, bool followProject);
    bool needsRender() const;            // with the lock held
    Request makeRequest();               // with the lock held
    void renderNow();
    void renderInBackground();
    void publish (std::unique_ptr<RiserSample> sample, int generation);

    PreDropEngine& engine;
    juce::AudioFormatManager formats;
    std::atomic<double> sampleRate { 0.0 };   // 0 until prepareToPlay
    std::atomic<double> hostTempo  { 0.0 };

    // The loaded sample: original file bytes (what the plugin state saves),
    // name, timing, and decoded audio at the file's own sample rate.
    struct Loaded
    {
        juce::MemoryBlock                               fileData;
        juce::String                                    name;
        std::shared_ptr<const juce::AudioBuffer<float>> audio;
        double                                          sampleRate = 0.0;
        RiserTiming                                     timing;
        Outline                                         outline;
        int                                             id = 0;
    };
    juce::CriticalSection lock;
    Loaded loaded;
    int    nextId = 1;

    // The newest rendering asked for (later ones replace earlier ones), the
    // project tempo it was for, and the newest one the engine has.
    juce::CriticalSection publishLock;
    std::atomic<int>      generation { 0 };
    std::atomic<int>      published  { 0 };
    double                requestedTempo = 0.0;

    juce::ThreadPool renderer { juce::ThreadPoolOptions{}.withNumberOfThreads (1).withThreadName ("PreDrop riser") };

    JUCE_DECLARE_NON_COPYABLE (RiserSource)
};
