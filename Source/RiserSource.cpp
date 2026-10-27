#include "RiserSource.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

const juce::Identifier RiserSource::stateTag { "RISER_SAMPLE" };

namespace
{
    // Largest file accepted, so a stray long recording can't bloat the project.
    constexpr juce::int64 maxFileBytes = 64 * 1024 * 1024;

    // Columns in a sample's outline (the UI draws it at any width from these).
    constexpr int outlineColumns = 1024;

    // Smallest tempo change worth a re-render.
    constexpr double tempoTolerance = 0.01;

    // The first `length` samples of `in`, padded with silence if it is shorter.
    juce::AudioBuffer<float> fitted (const juce::AudioBuffer<float>& in, int length)
    {
        juce::AudioBuffer<float> out (in.getNumChannels(), juce::jmax (1, length));
        out.clear();
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
            out.copyFrom (ch, 0, in, ch, 0, juce::jmin (out.getNumSamples(), in.getNumSamples()));
        return out;
    }

    // Time-stretches `in` to `outLength` samples, keeping its pitch. Empty if
    // it is too short to stretch.
    juce::AudioBuffer<float> stretched (const juce::AudioBuffer<float>& in, double sampleRate, int outLength)
    {
        signalsmith::stretch::SignalsmithStretch<float> stretch;
        stretch.presetDefault (in.getNumChannels(), (float) sampleRate);

        juce::AudioBuffer<float> out (in.getNumChannels(), outLength);
        if (! stretch.exact (in.getArrayOfReadPointers(), in.getNumSamples(), out.getArrayOfWritePointers(), outLength))
            return {};
        return out;
    }

    // The sample's outline: lowest and highest value (of any channel) per column.
    RiserSource::Outline outlineOf (const juce::AudioBuffer<float>& audio)
    {
        RiserSource::Outline outline;
        outline.low.assign ((size_t) outlineColumns, 0.0f);
        outline.high.assign ((size_t) outlineColumns, 0.0f);
        const int length = audio.getNumSamples();
        for (int c = 0; c < outlineColumns; ++c)
        {
            const auto from = (int) ((juce::int64) length * c / outlineColumns);
            const int to   = juce::jmax (from + 1, (int) ((juce::int64) length * (c + 1) / outlineColumns));
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            {
                const auto range = juce::FloatVectorOperations::findMinAndMax (audio.getReadPointer (ch, from),
                                                                               juce::jmin (to, length) - from);
                outline.low[(size_t) c]  = juce::jmin (outline.low[(size_t) c],  range.getStart());
                outline.high[(size_t) c] = juce::jmax (outline.high[(size_t) c], range.getEnd());
            }
        }
        return outline;
    }
}

RiserSource::RiserSource (PreDropEngine& engineToFeed) : engine (engineToFeed)
{
    formats.registerBasicFormats();
}

RiserSource::~RiserSource()
{
    renderer.removeAllJobs (true, 10000);
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

RiserTiming RiserSource::getTiming() const
{
    const juce::ScopedLock sl (lock);
    return loaded.timing;
}

double RiserSource::getLengthSeconds() const
{
    const juce::ScopedLock sl (lock);
    return loaded.audio != nullptr ? loaded.audio->getNumSamples() / loaded.sampleRate : 0.0;
}

RiserSource::Outline RiserSource::getOutline() const
{
    const juce::ScopedLock sl (lock);
    return loaded.outline;
}

int RiserSource::getSampleId() const
{
    const juce::ScopedLock sl (lock);
    return loaded.id;
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

    return loadFromData (fileData, file.getFileName(), nullptr, true);
}

juce::String RiserSource::loadFromData (const juce::MemoryBlock& fileData, const juce::String& name,
                                        const RiserTiming* savedTiming, bool followProject)
{
    std::unique_ptr<juce::AudioFormatReader> reader (
        formats.createReaderFor (std::make_unique<juce::MemoryInputStream> (fileData, false)));

    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return "\"" + name + "\" is not an audio file the riser can use (try WAV, AIFF or FLAC).";

    const double seconds = (double) reader->lengthInSamples / reader->sampleRate;
    if (seconds > maxSeconds)
        return "\"" + name + "\" is longer than " + juce::String ((int) maxSeconds) + " seconds.";

    auto audio = std::make_shared<juce::AudioBuffer<float>> ((int) juce::jmin (2u, reader->numChannels),
                                                             (int) reader->lengthInSamples);
    reader->read (audio.get(), 0, (int) reader->lengthInSamples, 0, true, audio->getNumChannels() > 1);

    // Reject silence up front rather than leaving the riser mysteriously quiet.
    if (RiserSample::create (*audio, reader->sampleRate, reader->sampleRate, name) == nullptr)
        return "\"" + name + "\" is silent.";

    Loaded fresh;
    fresh.fileData   = fileData;
    fresh.name       = name;
    fresh.sampleRate = reader->sampleRate;
    fresh.timing     = savedTiming != nullptr ? *savedTiming : RiserTiming::guess (name, seconds);
    if (savedTiming == nullptr)
        fresh.timing.matchProject = followProject;
    fresh.outline    = outlineOf (*audio);
    fresh.audio      = std::move (audio);

    {
        const juce::ScopedLock sl (lock);
        fresh.id = nextId++;
        loaded = std::move (fresh);
    }
    renderNow();
    return {};
}

void RiserSource::clear()
{
    {
        const juce::ScopedLock sl (lock);
        loaded = {};
    }
    renderNow();
}

void RiserSource::setTiming (const RiserTiming& timing)
{
    {
        const juce::ScopedLock sl (lock);
        if (loaded.audio == nullptr || loaded.timing == timing)
            return;
        loaded.timing = timing;
    }
    renderInBackground();
}

void RiserSource::setSampleRate (double rate)
{
    sampleRate = rate;
    renderNow();
}

// --- Rendering ----------------------------------------------------------------

bool RiserSource::needsRender() const
{
    const double tempo = hostTempo.load();
    return loaded.audio != nullptr && loaded.timing.matchProject && loaded.timing.keepPitch
        && tempo > 0.0 && std::abs (tempo - requestedTempo) > tempoTolerance;
}

RiserSource::Request RiserSource::makeRequest()
{
    Request r;
    r.audio      = loaded.audio;
    r.sourceRate = loaded.sampleRate;
    r.name       = loaded.name;
    r.sourceId   = loaded.id;
    r.timing     = loaded.timing;
    r.hostTempo  = hostTempo.load();
    r.engineRate = sampleRate.load();
    r.generation = ++generation;
    requestedTempo = r.hostTempo;
    return r;
}

void RiserSource::update()
{
    {
        const juce::ScopedLock sl (lock);
        if (! needsRender())
            return;
    }
    renderInBackground();
}

void RiserSource::renderIfStale()
{
    {
        // Stale too while a background rendering (for a new tempo or timing)
        // hasn't reached the engine yet: a bounce can't wait for it.
        const juce::ScopedLock sl (lock);
        if (! needsRender() && published.load() == generation.load())
            return;
    }
    renderNow();
}

void RiserSource::renderNow()
{
    // Before prepareToPlay there is no sample rate to build for; prepareToPlay
    // renders again once there is.
    if (sampleRate.load() <= 0.0)
        return;

    Request request;
    {
        const juce::ScopedLock sl (lock);
        request = makeRequest();
    }
    publish (render (request), request.generation);
}

void RiserSource::renderInBackground()
{
    if (sampleRate.load() <= 0.0)
        return;

    Request request;
    {
        const juce::ScopedLock sl (lock);
        request = makeRequest();
    }
    renderer.addJob ([this, request]
    {
        if (request.generation == generation.load())   // nothing newer asked for since
            publish (render (request), request.generation);
    });
}

bool RiserSource::waitForRender (int timeoutMs) const
{
    const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (renderer.getNumJobs() > 0)
    {
        if (juce::Time::getMillisecondCounter() > until)
            return false;
        juce::Thread::sleep (2);
    }
    return true;
}

void RiserSource::publish (std::unique_ptr<RiserSample> sample, int forGeneration)
{
    // Only the newest rendering reaches the engine, and one at a time.
    const juce::ScopedLock sl (publishLock);
    if (forGeneration == generation.load())
    {
        engine.setRiserSample (sample != nullptr ? std::move (sample) : RiserSample::none());
        published = forGeneration;
    }
}

std::unique_ptr<RiserSample> RiserSource::render (const Request& r)
{
    if (r.audio == nullptr || r.engineRate <= 0.0)
        return nullptr;

    const auto& t = r.timing;
    std::unique_ptr<RiserSample> sample;
    if (! t.matchProject)
    {
        // Its own tempo, its whole length.
        sample = RiserSample::create (*r.audio, r.sourceRate, r.engineRate, r.name);
    }
    else
    {
        // A whole number of bars from the top (trimmed, or padded with
        // silence), at the project's tempo.
        const double seconds = r.audio->getNumSamples() / r.sourceRate;
        const double loopSeconds = t.loopBarsFor (seconds) * t.barSeconds();
        const auto loop = fitted (*r.audio, (int) std::lround (loopSeconds * r.sourceRate));

        if (t.keepPitch && r.hostTempo > 0.0)
        {
            const double speed = r.hostTempo / t.bpm;
            if (const auto audio = stretched (loop, r.sourceRate, (int) std::lround (loop.getNumSamples() / speed));
                audio.getNumSamples() > 0)
                sample = RiserSample::create (audio, r.sourceRate, r.engineRate, r.name, r.hostTempo, loopSeconds / speed);
        }

        // Pitch follows the tempo (or the project's tempo isn't known yet, or
        // the sample is too short to stretch): its own tempo, which the engine
        // speeds up or slows down to the project's.
        if (sample == nullptr)
            sample = RiserSample::create (loop, r.sourceRate, r.engineRate, r.name, t.bpm, loopSeconds);
    }

    if (sample != nullptr)
        sample->sourceId = r.sourceId;
    return sample;
}

// --- State ----------------------------------------------------------------------

void RiserSource::saveTo (juce::ValueTree& state) const
{
    const juce::ScopedLock sl (lock);
    if (loaded.fileData.isEmpty())
        return;

    juce::ValueTree sample (stateTag);
    sample.setProperty ("name", loaded.name, nullptr);
    sample.setProperty ("data", loaded.fileData.toBase64Encoding(), nullptr);
    sample.setProperty ("bpm", loaded.timing.bpm, nullptr);
    sample.setProperty ("beatsPerBar", loaded.timing.beatsPerBar, nullptr);
    sample.setProperty ("beatUnit", loaded.timing.beatUnit, nullptr);
    sample.setProperty ("matchProject", loaded.timing.matchProject, nullptr);
    sample.setProperty ("keepPitch", loaded.timing.keepPitch, nullptr);
    sample.setProperty ("loopBars", loaded.timing.loopBars, nullptr);
    state.appendChild (sample, nullptr);
}

void RiserSource::restoreFrom (const juce::ValueTree& state)
{
    const auto sample = state.getChildWithName (stateTag);
    juce::MemoryBlock fileData;
    if (! sample.isValid() || ! fileData.fromBase64Encoding (sample["data"].toString()))
    {
        clear();
        return;
    }

    // Sets saved before riser timing existed play as they did: at their own tempo.
    RiserTiming timing;
    timing.bpm          = juce::jlimit (RiserTiming::minBpm, RiserTiming::maxBpm, (double) sample.getProperty ("bpm", timing.bpm));
    timing.beatsPerBar  = juce::jlimit (1, 16, (int) sample.getProperty ("beatsPerBar", timing.beatsPerBar));
    timing.beatUnit     = (int) sample.getProperty ("beatUnit", timing.beatUnit) == 8 ? 8 : 4;
    timing.matchProject = sample.getProperty ("matchProject", false);
    timing.keepPitch    = sample.getProperty ("keepPitch", true);
    timing.loopBars     = juce::jlimit (0, RiserTiming::maxLoopBars, (int) sample.getProperty ("loopBars", 0));
    const bool hasTiming = sample.hasProperty ("bpm");

    if (loadFromData (fileData, sample["name"].toString(), hasTiming ? &timing : nullptr, false).isNotEmpty())
        clear();
}
