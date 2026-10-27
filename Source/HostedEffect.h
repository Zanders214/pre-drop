#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>

#include "PreDropEngine.h"

/**
    HostedEffect: a third-party plug-in standing in for PreDrop's built-in
    reverb or delay.

    The engine hands it the high-passed signal and sums its output in at the
    same calibrated level it would give the built-in effect, so the plug-in
    must run 100% wet. A plug-in left partly dry passes the input straight
    through, which shows up as a strong correlation between what goes in and
    what comes out (lined up by the plug-in's reported latency, since a
    plug-in with latency returns its dry signal that much later); that is
    measured here so the UI can warn about it.

    An empty one (no plug-in) is the "use the built-in effect" marker for the
    hand-over to the audio thread. Created and prepared off the audio thread;
    deleted on the message thread (plug-ins require it).
*/
class HostedEffect : public PreDropEngine::ExternalEffect
{
public:
    HostedEffect() = default;

    HostedEffect (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::PluginDescription& desc)
        : plugin (std::move (instance)), description (desc) {}

    ~HostedEffect() override
    {
        if (plugin != nullptr)
            plugin->releaseResources();
    }

    bool isEmpty() const noexcept                         { return plugin == nullptr; }
    juce::AudioPluginInstance* getPlugin() const noexcept { return plugin.get(); }
    const juce::PluginDescription& getDescription() const { return description; }

    /** Passes on the host's playhead (tempo, position, transport). Audio thread,
        before process(): a plug-in reads it while processing. */
    void setPlayHead (juce::AudioPlayHead* playHead) noexcept
    {
        if (plugin != nullptr)
            plugin->setPlayHead (playHead);
    }

    /** (Re)prepares the plug-in. Never while process() can run. */
    void prepare (double sampleRate, int maximumBlockSize)
    {
        if (plugin == nullptr)
            return;

        plugin->releaseResources();
        plugin->setRateAndBufferSizeDetails (sampleRate, maximumBlockSize);
        plugin->prepareToPlay (sampleRate, maximumBlockSize);

        const int channels = juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
        scratch.setSize (channels, juce::jmax (1, maximumBlockSize), false, true, true);
        history.setSize (1, juce::jmax (1, maximumBlockSize) + maxLatency, false, true, true);
        history.clear();
        historyPos = 0;
        midi.ensureSize (256);
    }

    void process (float* const* wet, int numChannels, int numSamples) noexcept override
    {
        if (plugin == nullptr || numSamples > scratch.getNumSamples())
        {
            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::clear (wet[ch], numSamples);
            return;
        }

        // Into the plug-in's buffer (a mono send feeds both inputs), keeping the
        // first channel's history for the dry-leak check.
        scratch.clear();
        for (int ch = 0; ch < scratch.getNumChannels() && ch < juce::jmax (2, numChannels); ++ch)
            scratch.copyFrom (ch, 0, wet[juce::jmin (ch, numChannels - 1)], numSamples);
        const int blockStart = historyPos;
        for (int n = 0; n < numSamples; ++n)
        {
            history.setSample (0, historyPos, wet[0][n]);
            historyPos = (historyPos + 1) % history.getNumSamples();
        }

        // A view of exactly this block's length, so the plug-in sees the real size.
        juce::AudioBuffer<float> block (scratch.getArrayOfWritePointers(), scratch.getNumChannels(), numSamples);
        midi.clear();
        plugin->processBlock (block, midi);

        const int outputs = juce::jmax (1, plugin->getTotalNumOutputChannels());
        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy (wet[ch], scratch.getReadPointer (juce::jmin (ch, outputs - 1)), numSamples);

        measureDryLeak (wet[0], numSamples, blockStart);
    }

    /** How strongly the output follows the input, smoothed over ~1 s: near 1
        when the plug-in passes its dry signal through, near 0 when fully wet. */
    float dryLeak() const noexcept { return leak.load(); }

    /** Above this, the UI asks for the plug-in to be set to 100% wet. */
    static constexpr float dryLeakWarning = 0.6f;

private:
    // Correlates this block's output with the input from `latency` samples
    // earlier, which is where a dry signal passing through would line up.
    void measureDryLeak (const float* output, int numSamples, int blockStart) noexcept
    {
        const int size    = history.getNumSamples();
        const int latency = juce::jlimit (0, maxLatency, plugin->getLatencySamples());
        const float* in   = history.getReadPointer (0);

        double cross     = 0.0;
        double inEnergy  = 0.0;
        double outEnergy = 0.0;
        for (int n = 0; n < numSamples; ++n)
        {
            const float x = in[((blockStart + n - latency) % size + size) % size];
            cross     += (double) x * output[n];
            inEnergy  += (double) x * x;
            outEnergy += (double) output[n] * output[n];
        }

        // Only judge blocks with signal on both sides.
        if (inEnergy < 1.0e-8 || outEnergy < 1.0e-8)
            return;

        const auto correlation = (float) (cross / std::sqrt (inEnergy * outEnergy));
        leak.store (leak.load() + 0.05f * (correlation - leak.load()));
    }

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription  description;
    static constexpr int maxLatency = 16384;   // longest plug-in latency the check lines up

    juce::AudioBuffer<float> scratch;
    juce::AudioBuffer<float> history;          // recent input (first channel)
    int                      historyPos = 0;
    juce::MidiBuffer         midi;
    std::atomic<float>       leak { 0.0f };

    JUCE_DECLARE_NON_COPYABLE (HostedEffect)
};

/** A plug-in's own editor in a window of its own, floating over the host.
    Closing it only hides it; it is deleted before its plug-in is. */
class EffectPluginWindow final : public juce::DocumentWindow
{
public:
    EffectPluginWindow (juce::AudioPluginInstance& plugin, const juce::String& title, std::function<void()> onCloseRequest)
        : DocumentWindow (title, juce::Colours::black, juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseRequest))
    {
        setUsingNativeTitleBar (true);
        std::unique_ptr<juce::AudioProcessorEditor> editor (plugin.hasEditor() ? plugin.createEditorIfNeeded() : nullptr);
        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor> (plugin);
        const bool resizable = editor->isResizable();
        setContentOwned (editor.release(), true);   // the window owns its editor from here
        setResizable (resizable, false);
        setAlwaysOnTop (true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~EffectPluginWindow() override { clearContentComponent(); }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();
    }

private:
    std::function<void()> onClose;

    JUCE_DECLARE_NON_COPYABLE (EffectPluginWindow)
};
