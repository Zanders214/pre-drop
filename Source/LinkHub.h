#pragma once

#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

#include "EffectSettings.h"

/**
    LinkHub

    The meeting point for PreDrop instances in one host process. Hosts such as
    Ableton Live load every instance of a plugin into the same process, so one
    shared hub (held through juce::SharedResourcePointer) lets a PreDrop on the
    master drive PreDrops on individual tracks:

      - The *main* (the first instance set to MAIN that claims the slot)
        publishes its Amount and effect depths. While nothing is linked it
        works on the whole mix; as soon as a TRACK or RETURN links, those
        take over the high-pass, reverb and delay and the main keeps the riser.
      - *Tracks* (instances set to TRACK) register so the main's editor can list
        them and set how much of each effect they get.
      - *Returns* (instances set to RETURN, on a send / return track) register
        so the main's editor can say they are linked; how much each track gets
        is that track's send knob in the host.

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

    /** A PreDrop instance, whatever its role: what the hub knows it by. */
    struct Member
    {
        virtual ~Member() = default;
    };

    /** Implemented by every instance, and listed while it is set to TRACK.
        Message thread. A level is 0..1: how much of the main's depth for that
        effect the track gets (1, the default, is all of it; 0 is off). */
    struct LinkedTrack : Member
    {
        virtual juce::String getLinkName() const = 0;
        virtual float getLinkLevel (Effect effect) const = 0;
        virtual void  setLinkLevel (Effect effect, float level) = 0;
    };

    /** What the main publishes for its tracks; written by the main, read by any
        thread. Depths are indexed by Effect, settings by EffectSettings::Id. */
    struct MainState
    {
        MainState() noexcept
        {
            const EffectSettings defaults;
            for (size_t i = 0; i < settings.size(); ++i)
                settings[i].store (defaults.values[i]);
        }

        std::atomic<float> amount { 0.0f };
        std::array<std::atomic<float>, numEffects> depths { 0.5f, 0.5f, 0.5f };
        std::array<std::atomic<float>, EffectSettings::count> settings;

        /** The main's effect settings, as its tracks use them. */
        EffectSettings effectSettings() const noexcept
        {
            EffectSettings s;
            for (size_t i = 0; i < settings.size(); ++i)
                s.values[i] = settings[i].load();
            return s;
        }
    };

    // --- The main slot ------------------------------------------------------
    /** Makes `owner` the main if nobody holds the slot. True if `owner` is the
        main afterwards. */
    bool claimMain (const Member* owner) noexcept
    {
        const Member* expected = nullptr;
        return mainOwner.compare_exchange_strong (expected, owner) || expected == owner;
    }

    /** Gives up the slot if `owner` holds it. */
    void releaseMain (const Member* owner) noexcept
    {
        const Member* expected = owner;
        mainOwner.compare_exchange_strong (expected, nullptr);
    }

    bool isMain (const Member* owner) const noexcept { return owner != nullptr && mainOwner.load() == owner; }
    bool hasMain() const noexcept                  { return mainOwner.load() != nullptr; }

    MainState&       mainState() noexcept       { return main; }
    const MainState& mainState() const noexcept { return main; }

    /** True when the link is in use: a main exists and at least one TRACK or
        RETURN is linked to it. Then the followers apply the high-pass, reverb
        and delay and the main leaves them out. Safe on the audio thread. */
    bool linkActive() const noexcept { return hasMain() && numFollowers.load() > 0; }

    // --- Linked tracks (message thread) -------------------------------------
    void addTrack (LinkedTrack* track)
    {
        const juce::ScopedLock lock (tracksLock);
        if (std::find (tracks.begin(), tracks.end(), track) == tracks.end())
            tracks.push_back (track);
        countFollowers();
    }

    void removeTrack (LinkedTrack* track)
    {
        const juce::ScopedLock lock (tracksLock);
        tracks.erase (std::remove (tracks.begin(), tracks.end(), track), tracks.end());
        countFollowers();
    }

    /** The linked tracks in the order they linked. */
    std::vector<LinkedTrack*> getTracks() const
    {
        const juce::ScopedLock lock (tracksLock);
        return tracks;
    }

    // --- Linked returns (message thread) -------------------------------------
    void addReturn (const Member* owner)
    {
        const juce::ScopedLock lock (tracksLock);
        if (std::find (returns.begin(), returns.end(), owner) == returns.end())
            returns.push_back (owner);
        countFollowers();
    }

    void removeReturn (const Member* owner)
    {
        const juce::ScopedLock lock (tracksLock);
        returns.erase (std::remove (returns.begin(), returns.end(), owner), returns.end());
        countFollowers();
    }

    int numReturns() const
    {
        const juce::ScopedLock lock (tracksLock);
        return (int) returns.size();
    }

    /** Position of a track in the list (0-based), or -1 if it isn't linked. */
    int indexOf (const LinkedTrack* track) const
    {
        const juce::ScopedLock lock (tracksLock);
        const auto it = std::find (tracks.begin(), tracks.end(), track);
        return it == tracks.end() ? -1 : (int) std::distance (tracks.begin(), it);
    }

private:
    // Called with tracksLock held: mirrors the list sizes for the audio threads.
    void countFollowers() noexcept { numFollowers.store ((int) (tracks.size() + returns.size())); }

    std::atomic<const Member*> mainOwner { nullptr };
    std::atomic<int>         numFollowers { 0 };
    MainState main;

    juce::CriticalSection tracksLock;
    std::vector<LinkedTrack*> tracks;
    std::vector<const Member*> returns;
};
