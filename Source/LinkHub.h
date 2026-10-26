#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

/**
    LinkHub

    The meeting point for PreDrop instances in one host process. Hosts such as
    Ableton Live load every instance of a plugin into the same process, so one
    shared hub (held through juce::SharedResourcePointer) lets a PreDrop on the
    master drive PreDrops on individual tracks:

      - The *main* (the first instance set to MAIN that claims the slot)
        publishes its Amount and effect depths, and whether it applies to
        selected tracks only.
      - *Tracks* (instances set to TRACK) register so the main's editor can list
        them and set how much of each effect they get.

    Threading: the published values are atomics, so audio threads read them
    without locking. The track list is only touched on the message thread (and
    guarded by a lock regardless). Track registrations are removed by the
    track's destructor, so a listed track is always alive.
*/
class LinkHub
{
public:
    /** The effects a linked track applies, each at its own level. */
    enum class Effect { hpf = 0, reverb = 1, delay = 2 };
    static constexpr int numEffects = 3;

    /** Implemented by a processor while it is set to TRACK. Message thread.
        A level is 0..1: how much of the main's depth for that effect the track
        gets (1, the default, is all of it; 0 is off). */
    struct LinkedTrack
    {
        virtual ~LinkedTrack() = default;
        virtual juce::String getLinkName() const = 0;
        virtual float getLinkLevel (Effect effect) const = 0;
        virtual void  setLinkLevel (Effect effect, float level) = 0;
    };

    /** What the main publishes for its tracks; written by the main, read by any
        thread. Depths are indexed by Effect. */
    struct MainState
    {
        std::atomic<float> amount { 0.0f };
        std::array<std::atomic<float>, numEffects> depths { 0.5f, 0.5f, 0.5f };
        std::atomic<bool>  selectedTracksOnly { false };
    };

    // --- The main slot ------------------------------------------------------
    /** Makes `owner` the main if nobody holds the slot. True if `owner` is the
        main afterwards. */
    bool claimMain (const void* owner) noexcept
    {
        const void* expected = nullptr;
        return mainOwner.compare_exchange_strong (expected, owner) || expected == owner;
    }

    /** Gives up the slot if `owner` holds it. */
    void releaseMain (const void* owner) noexcept
    {
        const void* expected = owner;
        if (mainOwner.compare_exchange_strong (expected, nullptr))
            main.selectedTracksOnly.store (false);
    }

    bool isMain (const void* owner) const noexcept { return owner != nullptr && mainOwner.load() == owner; }
    bool hasMain() const noexcept                  { return mainOwner.load() != nullptr; }

    MainState&       mainState() noexcept       { return main; }
    const MainState& mainState() const noexcept { return main; }

    /** True when tracks should process: a main exists and it is set to
        selected tracks. Safe on the audio thread. */
    bool tracksActive() const noexcept { return hasMain() && main.selectedTracksOnly.load(); }

    // --- Linked tracks (message thread) -------------------------------------
    void addTrack (LinkedTrack* track)
    {
        const juce::ScopedLock lock (tracksLock);
        if (std::find (tracks.begin(), tracks.end(), track) == tracks.end())
            tracks.push_back (track);
    }

    void removeTrack (LinkedTrack* track)
    {
        const juce::ScopedLock lock (tracksLock);
        tracks.erase (std::remove (tracks.begin(), tracks.end(), track), tracks.end());
    }

    /** The linked tracks in the order they linked. */
    std::vector<LinkedTrack*> getTracks() const
    {
        const juce::ScopedLock lock (tracksLock);
        return tracks;
    }

    /** Position of a track in the list (0-based), or -1 if it isn't linked. */
    int indexOf (const LinkedTrack* track) const
    {
        const juce::ScopedLock lock (tracksLock);
        const auto it = std::find (tracks.begin(), tracks.end(), track);
        return it == tracks.end() ? -1 : (int) std::distance (tracks.begin(), it);
    }

private:
    std::atomic<const void*> mainOwner { nullptr };
    MainState main;

    juce::CriticalSection tracksLock;
    std::vector<LinkedTrack*> tracks;
};
