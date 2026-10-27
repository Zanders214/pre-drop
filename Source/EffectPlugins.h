#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <memory>

#include "HostedEffect.h"
#include "PreDropEngine.h"
#include "RtHandoff.h"

/**
    EffectPlugins: the reverb and the delay can each be a third-party VST3
    instead of the built-in effect. It runs 100% wet and PreDrop sets its level
    exactly as it would the built-in one's.

    Each slot hands its plug-in to the audio thread through an RtHandoff. The
    plug-ins (and their windows) are created and freed on the message thread
    only; saved ones restored from another thread are loaded there later.
*/
class EffectPlugins : private juce::AsyncUpdater
{
public:
    enum class Slot { reverb = 0, delay = 1 };

    EffectPlugins();
    ~EffectPlugins() override;

    // --- Choosing (message thread) ------------------------------------------
    /** Loads a VST3 into a slot. Returns why it couldn't, or empty on success. */
    juce::String load (Slot slot, const juce::File& plugin);
    void useBuiltIn (Slot slot);

    /** The slot's plug-in name, or empty for the built-in effect. */
    juce::String getName (Slot slot) const;
    /** A saved plug-in that isn't installed here (empty if none): the slot
        uses the built-in effect, and keeps the plug-in's settings for when
        the set is opened where it is installed. */
    juce::String getMissingName (Slot slot) const;
    /** True when the slot's plug-in seems to pass its dry signal through
        (it should be set to 100% wet). */
    bool leaksDry (Slot slot) const;

    void showWindow (Slot slot);
    void closeWindows();

    /** The slot's plug-in instance, or nullptr for the built-in effect. */
    juce::AudioPluginInstance* getInstance (Slot slot) const;

    /** VST3 plug-ins in the standard folders (found by name, nothing loaded). */
    static juce::Array<juce::File> findInstalledVst3();

    // --- For the processor ---------------------------------------------------
    /** Prepares the plug-ins for this rate (from prepareToPlay). */
    void prepare (double sampleRate, int blockSize);

    /** The slot's plug-in for this block, given the host's playhead, or nullptr
        for the built-in effect. Audio thread. */
    PreDropEngine::ExternalEffect* acquire (Slot slot, juce::AudioPlayHead* playHead) noexcept;

    /** Frees swapped-out plug-ins. Message thread, regularly. */
    void collect();

    /** Adds the loaded plug-ins and their settings to a state tree being saved. */
    void saveTo (juce::ValueTree& state) const;

    /** Restores the slots from a saved state tree. Any thread. */
    void restoreFrom (const juce::ValueTree& state);

    /** The state child that carries the plug-ins. */
    static const juce::Identifier stateTag;

private:
    void handleAsyncUpdate() override;
    void restoreAll (const juce::ValueTree& saved);
    void restore (Slot slot, const juce::ValueTree& saved);
    juce::String install (Slot slot, const juce::PluginDescription& description, const juce::MemoryBlock* pluginState);

    // `latest` is the plug-in most recently sent, which stays alive until a
    // newer one replaces it, so the message thread can use it (for its window
    // and its saved state). `lock` keeps loading and prepare() apart, both
    // being off the audio thread.
    struct SlotState
    {
        RtHandoff<HostedEffect>             handoff;
        HostedEffect*                       latest = nullptr;
        juce::String                        name;
        juce::ValueTree                     missing;
        std::unique_ptr<EffectPluginWindow> window;
    };

    juce::AudioPluginFormatManager formats;
    juce::CriticalSection          lock;
    std::array<SlotState, 2>       slots;
    std::atomic<double>            sampleRate { 0.0 };   // 0 until prepare()
    int                            blockSize = 512;
    juce::ValueTree                pendingRestore;   // saved plug-ins for the message thread to load

    JUCE_DECLARE_NON_COPYABLE (EffectPlugins)
};
