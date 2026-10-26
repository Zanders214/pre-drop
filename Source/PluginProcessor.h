#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "HostedEffect.h"
#include "LinkHub.h"
#include "PreDropEngine.h"
#include "RtHandoff.h"

/**
    PreDropAudioProcessor

    Thin host wrapper around PreDropEngine. It owns the parameter state and feeds
    the engine's macro controls each block; all DSP lives in PreDropEngine.

    It also owns the optional riser sample: the original file's bytes are kept
    and saved inside the plugin state, so a project carries its riser with it,
    and the decoded audio is rebuilt for the engine at the host's sample rate.

    Linking: each instance is MAIN (normally on the master), TRACK or RETURN.
    With nothing linked the main processes the whole mix, as a single PreDrop
    always has. Once any TRACK or RETURN links, the main only adds the riser:
    every linked TRACK applies the effects at its own level per effect, and
    every RETURN adds reverb and delay, wet only, to whatever the tracks send
    it. Both follow the main's Amount and depths. A new instance joins as a
    TRACK when a main already exists. See LinkHub.
*/
class PreDropAudioProcessor : public juce::AudioProcessor,
                              public LinkHub::LinkedTrack,
                              private juce::AudioProcessorValueTreeState::Listener,
                              private juce::Timer
{
public:
    PreDropAudioProcessor();
    ~PreDropAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;   // keep the double-precision overload visible

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState&       getState() noexcept       { return apvts; }
    const juce::AudioProcessorValueTreeState& getState() const noexcept { return apvts; }

    // --- Riser sample (message thread) ---------------------------------------
    /** Uses an audio file as the riser instead of the built-in noise. Returns an
        explanation if it can't be used, or an empty string on success. */
    juce::String loadRiserSample (const juce::File& file);

    /** Goes back to the built-in noise riser. */
    void clearRiserSample();

    /** File name of the riser sample, or empty while the built-in noise is used. */
    juce::String getRiserSampleName() const
    {
        const juce::ScopedLock lock (riserLock);
        return riser.name;
    }

    /** True for files the riser can load, judged by extension. */
    bool isSupportedRiserFile (const juce::File& file) const;

    /** Wildcard of every loadable extension, for a file chooser. */
    juce::String getRiserFileWildcard() const { return formats.getWildcardForAllFormats(); }

    static constexpr double maxRiserSeconds = 60.0;

    // --- Linking (message thread unless noted) -------------------------------
    enum class Role { main = 0, track = 1, returnTrack = 2 };

    Role getRole() const noexcept;               // any thread
    void setRole (Role newRole);

    /** True for TRACK and RETURN: instances that follow the main. */
    bool followsMain() const noexcept { return getRole() != Role::main; }

    /** True while this instance is the main that linked tracks follow. */
    bool isLinkMain() const noexcept { return linkHub->isMain (this); }
    LinkHub& getLinkHub() noexcept   { return *linkHub; }

    /** This track's name in the main's list: the name typed into PreDrop, else
        the host's track name, else "Track N". */
    juce::String getTrackName() const;
    void setTrackName (const juce::String& name);   // empty: back to the host's name

    juce::String getLinkName() const override { return getTrackName(); }
    float getLinkLevel (LinkHub::Effect effect) const override;
    void  setLinkLevel (LinkHub::Effect effect, float level) override;

    void updateTrackProperties (const TrackProperties& properties) override;

    // --- Effect plug-ins (message thread) -------------------------------------
    // The reverb and the delay can each be a third-party VST3 instead of the
    // built-in effect. It runs 100% wet and PreDrop sets its level exactly as
    // it would the built-in one's.
    enum class EffectSlot { reverb = 0, delay = 1 };

    /** Loads a VST3 into a slot. Returns why it couldn't, or empty on success. */
    juce::String loadEffectPlugin (EffectSlot slot, const juce::File& plugin);
    void useBuiltInEffect (EffectSlot slot);

    /** The slot's plug-in name, or empty for the built-in effect. */
    juce::String getEffectPluginName (EffectSlot slot) const;
    /** A saved plug-in that isn't installed here (empty if none): the slot
        uses the built-in effect, and keeps the plug-in's settings for when
        the set is opened where it is installed. */
    juce::String getMissingEffectPluginName (EffectSlot slot) const;
    /** True when the slot's plug-in seems to pass its dry signal through
        (it should be set to 100% wet). */
    bool effectPluginLeaksDry (EffectSlot slot) const;

    void showEffectPluginWindow (EffectSlot slot);
    void closeEffectPluginWindows();

    /** VST3 plug-ins in the standard folders (found by name, nothing loaded). */
    static juce::Array<juce::File> findInstalledVst3Plugins();

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    void timerCallback() override;
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    juce::String installEffectPlugin (EffectSlot slot, const juce::PluginDescription& description,
                                      const juce::MemoryBlock* pluginState);
    void restoreEffectPlugin (EffectSlot slot, const juce::ValueTree& saved);
    void updateLink();
    void publishMainState() noexcept;
    juce::String loadRiserFromData (const juce::MemoryBlock& fileData, const juce::String& name);
    void sendRiserToEngine();

    juce::AudioProcessorValueTreeState apvts;
    PreDropEngine engine;

    juce::AudioFormatManager formats;
    std::atomic<double> currentSampleRate { 0.0 };   // 0 until prepareToPlay

    // The loaded riser: original file bytes (what the plugin state saves), name,
    // and decoded audio at the file's own sample rate. Guarded by riserLock,
    // since hosts may call prepareToPlay / get/setStateInformation off the
    // message thread; never touched by the audio thread.
    juce::CriticalSection riserLock;
    struct RiserSource
    {
        juce::MemoryBlock        fileData;
        juce::String             name;
        juce::AudioBuffer<float> audio;
        double                   sampleRate = 0.0;
    };
    RiserSource riser;

    // One hub per process, shared by every PreDrop instance in it.
    juce::SharedResourcePointer<LinkHub> linkHub;

    // The host's name for this track (it may report it on any thread) and
    // whether this instance is currently in the hub's track list.
    struct LinkIdentity
    {
        juce::CriticalSection lock;
        juce::String          hostName;
        bool                  registered       = false;   // in the hub's track list
        bool                  registeredReturn = false;   // in the hub's return list
    };
    LinkIdentity link;

    // The reverb / delay plug-in slots. Each hands its plug-in to the audio
    // thread through an RtHandoff; `latest` is the one most recently sent, which
    // stays alive until a newer one replaces it, so the message thread can use
    // it (for its window and its saved state). `lock` keeps loading and
    // prepareToPlay apart, both being off the audio thread.
    struct EffectSlotState
    {
        RtHandoff<HostedEffect>             handoff;
        HostedEffect*                       latest = nullptr;
        juce::String                        name;
        juce::ValueTree                     missing;
        std::unique_ptr<EffectPluginWindow> window;
    };
    struct EffectPlugins
    {
        juce::AudioPluginFormatManager formats;
        juce::CriticalSection          lock;
        std::array<EffectSlotState, 2> slots;
        int                            blockSize = 512;
    };
    EffectPlugins effectPlugins;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PreDropAudioProcessor)
};
