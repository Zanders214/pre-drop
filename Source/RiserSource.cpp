#include "RiserSource.h"

const juce::Identifier RiserSource::stateTag { "RISER_SAMPLE" };

namespace
{
    // Largest file accepted, so a stray long recording can't bloat the project.
    constexpr juce::int64 maxFileBytes = 64 * 1024 * 1024;
}

RiserSource::RiserSource (PreDropEngine& engineToFeed) : engine (engineToFeed)
{
    formats.registerBasicFormats();
}

bool RiserSource::isSupportedFile (const juce::File& file) const
{
    return formats.findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

juce::String RiserSource::getName() const
{
    const juce::ScopedLock sl (lock);
    return loaded.name;
}

juce::String RiserSource::load (const juce::File& file)
{
    if (! file.existsAsFile())
        return "\"" + file.getFileName() + "\" could not be found.";
    if (! isSupportedFile (file))
        return "\"" + file.getFileName() + "\" is not an audio file the riser can use (try WAV, AIFF or FLAC).";
    if (file.getSize() > maxFileBytes)
        return "\"" + file.getFileName() + "\" is too large for a riser (64 MB max).";

    juce::MemoryBlock fileData;
    if (! file.loadFileAsData (fileData))
        return "\"" + file.getFileName() + "\" could not be read.";

    return loadFromData (fileData, file.getFileName());
}

juce::String RiserSource::loadFromData (const juce::MemoryBlock& fileData, const juce::String& name)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        formats.createReaderFor (std::make_unique<juce::MemoryInputStream> (fileData, false)));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return "\"" + name + "\" is not an audio file the riser can use (try WAV, AIFF or FLAC).";

    if ((double) reader->lengthInSamples / reader->sampleRate > maxSeconds)
        return "\"" + name + "\" is longer than " + juce::String ((int) maxSeconds) + " seconds.";

    Loaded fresh;
    fresh.fileData   = fileData;
    fresh.name       = name;
    fresh.sampleRate = reader->sampleRate;
    fresh.audio.setSize ((int) juce::jmin (2u, reader->numChannels), (int) reader->lengthInSamples);
    reader->read (&fresh.audio, 0, (int) reader->lengthInSamples, 0, true, fresh.audio.getNumChannels() > 1);

    // Reject silence up front rather than leaving the riser mysteriously quiet.
    if (RiserSample::create (fresh.audio, fresh.sampleRate, fresh.sampleRate, name) == nullptr)
        return "\"" + name + "\" is silent.";

    {
        const juce::ScopedLock sl (lock);
        loaded = std::move (fresh);
    }
    sendToEngine();
    return {};
}

void RiserSource::clear()
{
    {
        const juce::ScopedLock sl (lock);
        loaded = {};
    }
    sendToEngine();
}

void RiserSource::setSampleRate (double rate)
{
    sampleRate = rate;
    sendToEngine();
}

void RiserSource::sendToEngine()
{
    // Before prepareToPlay there is no sample rate to build for; prepareToPlay
    // calls this again once there is.
    const double rate = sampleRate.load();
    if (rate <= 0.0)
        return;

    const juce::ScopedLock sl (lock);
    auto sample = loaded.audio.getNumSamples() > 0
                    ? RiserSample::create (loaded.audio, loaded.sampleRate, rate, loaded.name)
                    : nullptr;
    engine.setRiserSample (sample != nullptr ? std::move (sample) : RiserSample::none());
}

void RiserSource::saveTo (juce::ValueTree& state) const
{
    const juce::ScopedLock sl (lock);
    if (loaded.fileData.isEmpty())
        return;

    juce::ValueTree sample (stateTag);
    sample.setProperty ("name", loaded.name, nullptr);
    sample.setProperty ("data", loaded.fileData.toBase64Encoding(), nullptr);
    state.appendChild (sample, nullptr);
}

void RiserSource::restoreFrom (const juce::ValueTree& state)
{
    const auto sample = state.getChildWithName (stateTag);
    if (juce::MemoryBlock fileData; sample.isValid() && fileData.fromBase64Encoding (sample["data"].toString())
                                    && loadFromData (fileData, sample["name"].toString()).isEmpty())
        return;

    clear();
}
