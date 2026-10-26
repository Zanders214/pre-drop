/*
    PreDropEngine unit tests.

    Headless and framework-free: plain check() asserts plus an int main() that
    returns non-zero on failure, so it runs on any platform without a display and
    plugs straight into CTest. The macro mapping is the riskiest logic, so it gets
    the most coverage, backed by a couple of whole-engine smoke tests.
*/

#include "PreDropEngine.h"
#include "PreDropVisualModel.h"

#include <cmath>
#include <array>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace
{
int failures = 0;

void check (bool condition, const std::string& message)
{
    std::printf ("  [%s] %s\n", condition ? "PASS" : "FAIL", message.c_str());
    if (! condition)
        ++failures;
}

bool approx (float a, float b, float tolerance) { return std::abs (a - b) <= tolerance; }

juce::AudioBuffer<float> makeSine (double sampleRate, double freq, int numSamples, int numChannels = 2)
{
    juce::AudioBuffer<float> buffer (numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample (ch, i, 0.5f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * freq * i / sampleRate));
    return buffer;
}

float rms (const juce::AudioBuffer<float>& buffer, int channel, int start, int end)
{
    double sum = 0.0;
    for (int i = start; i < end; ++i)
        sum += (double) buffer.getSample (channel, i) * buffer.getSample (channel, i);
    return (float) std::sqrt (sum / juce::jmax (1, end - start));
}

// ---- Test 1: every effect follows the macro from the first percent. ----------
void testMappingBoundaries()
{
    std::printf ("Mapping boundaries:\n");

    const float eps = 1.0e-9f;
    const auto at0 = PreDropEngine::mapMacro (0.0f);
    check (approx (at0.hpfCutoffHz, 20.0f, 0.01f), "at amount=0 HPF cutoff sits at 20 Hz");
    check (at0.reverbWet  < eps, "at amount=0 reverb is silent");
    check (at0.delayWet   < eps, "at amount=0 delay is silent");
    check (at0.riserLevel < eps, "at amount=0 riser is silent");

    // No staggered start points: one percent of Amount engages all four.
    const auto at1pc = PreDropEngine::mapMacro (0.01f);
    check (at1pc.hpfCutoffHz > 20.0f && at1pc.reverbWet > 0.0f && at1pc.delayWet > 0.0f && at1pc.riserLevel > 0.0f,
           "at amount=1% every effect is already engaged");

    const auto at1 = PreDropEngine::mapMacro (1.0f);
    check (approx (at1.hpfCutoffHz, 800.0f, 0.5f),  "at amount=1 HPF cutoff reaches 800 Hz (default depth)");
    check (approx (at1.riserLevel, 0.5f, 1.0e-6f), "at amount=1 riser sits at -6 dB re the signal (default depth)");
    check (approx (at1.delayFeedback, PreDropEngine::maxDelayFeedback, 1.0e-6f) && at1.delayFeedback <= 0.6f,
           "delay feedback tops out at 0.6, bounding in-phase echo build-up");

    // Riser level is linear in Amount, so half the knob is half the level.
    check (approx (PreDropEngine::mapMacro (0.5f).riserLevel, 0.5f * at1.riserLevel, 1.0e-6f),
           "riser level is linear in Amount");

    // Monotonic build-up: more knob never means less of any effect. Reverb and
    // delay are compared by level (gain x tail / feedback build-up), since their
    // raw gains are compensated as the room and the feedback grow.
    const auto reverbLevel = [] (const PreDropEngine::Targets& x) { return x.reverbWet * PreDropEngine::reverbWetGain (x.reverbSize); };
    const auto delayLevel  = [] (const PreDropEngine::Targets& x) { return x.delayWet / std::sqrt (1.0f - x.delayFeedback * x.delayFeedback); };

    bool monotonic = true;
    PreDropEngine::Targets prev = PreDropEngine::mapMacro (0.0f);
    for (int i = 1; i <= 100; ++i)
    {
        const auto cur = PreDropEngine::mapMacro ((float) i / 100.0f);
        monotonic = monotonic
                 && cur.hpfCutoffHz   >= prev.hpfCutoffHz   - 1.0e-3f
                 && cur.reverbSize    >= prev.reverbSize    - 1.0e-6f
                 && reverbLevel (cur) >= reverbLevel (prev) - 1.0e-6f
                 && cur.delayFeedback >= prev.delayFeedback - 1.0e-6f
                 && delayLevel (cur)  >= delayLevel (prev)  - 1.0e-6f
                 && cur.riserLevel    >= prev.riserLevel    - 1.0e-6f
                 && cur.riserCutoffHz >= prev.riserCutoffHz - 1.0e-3f;
        prev = cur;
    }
    check (monotonic, "every effect rises monotonically with the macro");
}

// ---- Test 1b: per-effect depth: 0 off, 0.5 balanced default, 1 doubled. --------
void testDepthScaling()
{
    std::printf ("Per-effect depth scaling:\n");

    const float eps = 1.0e-6f;
    using Depths = PreDropEngine::Depths;

    const auto full = PreDropEngine::mapMacro (1.0f);
    const auto fullExplicit = PreDropEngine::mapMacro (1.0f, Depths {});
    check (approx (full.reverbWet, fullExplicit.reverbWet, eps)
        && approx (full.hpfCutoffHz, fullExplicit.hpfCutoffHz, eps),
        "mapMacro(amount) uses the default depths");

    const Depths defaults;
    check (approx (defaults.hpf, 0.5f, eps) && approx (defaults.reverb, 0.5f, eps)
        && approx (defaults.delay, 0.5f, eps) && approx (defaults.riser, 0.5f, eps),
        "every depth defaults to 50%");

    // Depth 0 silences just that effect; the others are untouched.
    Depths noReverb; noReverb.reverb = 0.0f;
    const auto rv0 = PreDropEngine::mapMacro (1.0f, noReverb);
    check (rv0.reverbWet < eps,                           "reverb depth 0 silences reverb at full amount");
    check (approx (rv0.delayWet, full.delayWet, eps),     "reverb depth 0 leaves delay untouched");
    check (approx (rv0.riserLevel, full.riserLevel, eps), "reverb depth 0 leaves riser untouched");

    Depths noDelay; noDelay.delay = 0.0f;
    check (PreDropEngine::mapMacro (1.0f, noDelay).delayWet < eps, "delay depth 0 silences delay");

    Depths noRiser; noRiser.riser = 0.0f;
    check (PreDropEngine::mapMacro (1.0f, noRiser).riserLevel < eps, "riser depth 0 silences riser");

    // Depth 1 doubles each gain-type effect relative to the default.
    Depths maxed; maxed.reverb = 1.0f; maxed.delay = 1.0f; maxed.riser = 1.0f; maxed.hpf = 1.0f;
    const auto mx = PreDropEngine::mapMacro (1.0f, maxed);
    check (approx (mx.reverbWet,  2.0f * full.reverbWet,  eps), "reverb depth 100% doubles the reverb");
    check (approx (mx.delayWet,   2.0f * full.delayWet,   eps), "delay depth 100% doubles the delay");
    check (approx (mx.riserLevel, 2.0f * full.riserLevel, eps), "riser depth 100% doubles the riser");

    // HPF depth moves the top of the sweep: off, 800 Hz, then two octaves more.
    Depths noHpf; noHpf.hpf = 0.0f;
    check (approx (PreDropEngine::mapMacro (1.0f, noHpf).hpfCutoffHz, 20.0f, 0.01f),
           "HPF depth 0 keeps the cutoff at 20 Hz (low-cut effectively off)");
    check (approx (mx.hpfCutoffHz, 3200.0f, 2.0f), "HPF depth 100% reaches 3.2 kHz");
}

// ---- Test 2: with the macro at zero, the effect is near-transparent. ----------
void testNearPassthroughAtZero()
{
    std::printf ("Near-passthrough at amount=0:\n");

    constexpr double sr = 48000.0;
    const int n = 8192;

    PreDropEngine engine;
    engine.prepare (sr, 2, n);
    engine.setAmount (0.0f);
    engine.setMix (1.0f);
    engine.setOutputTrimDb (0.0f);

    const auto in = makeSine (sr, 440.0, n);
    auto buffer = in;
    engine.process (buffer);

    // Compare the settled tail (skip filter warm-up) by RMS.
    const int start = n / 2;
    const float inRms  = rms (in,     0, start, n);
    const float outRms = rms (buffer, 0, start, n);

    check (approx (outRms, inRms, 0.03f), "at amount=0 output level matches input (transparent)");
}

// ---- Test 3: at full tilt the output stays finite and bounded. ----------------
void testStabilityAtFull()
{
    std::printf ("Stability at amount=1, every depth at 100%%:\n");

    constexpr double sr = 48000.0;
    const int block = 512;
    const int blocks = 200; // ~2.1 s of sustained input

    PreDropEngine engine;
    engine.prepare (sr, 2, block);
    engine.setAmount (1.0f);
    engine.setMix (1.0f);
    engine.setOutputTrimDb (0.0f);
    engine.setHpfDepth (1.0f);
    engine.setReverbDepth (1.0f);
    engine.setDelayDepth (1.0f);
    engine.setRiserDepth (1.0f);

    bool finite = true;
    float peak = 0.0f;

    for (int b = 0; b < blocks; ++b)
    {
        auto buffer = makeSine (sr, 110.0, block);
        engine.process (buffer);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < block; ++i)
            {
                const float s = buffer.getSample (ch, i);
                if (! std::isfinite (s))
                    finite = false;
                peak = juce::jmax (peak, std::abs (s));
            }
    }

    check (finite,      "output stays finite (no NaN/Inf) under sustained input at full");
    check (peak < 8.0f, "output stays bounded (feedback does not run away)");
}

// ---- Test 3b: reverb, delay and riser sit at the same level, in step. ----------
// Paul Kellet's economy pink-noise filter: a broadband stand-in for a mix.
struct PinkNoise
{
    juce::Random random { 2024 };
    float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f;

    float next()
    {
        const float w = random.nextFloat() * 2.0f - 1.0f;
        b0 = 0.99765f * b0 + w * 0.0990460f;
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        return (b0 + b1 + b2 + w * 0.1848f) * 0.25f;
    }
};

// Renders 6 s of the same seeded pink noise (or, for the worst case, a 400 Hz
// tone: exactly 150 cycles per 375 ms echo, so every echo lands in phase)
// through an engine and returns the output's channel 0. HPF depth 0 keeps the
// high-pass out of the measurement.
// Two seconds of seeded white noise, the stand-in for a user riser sample.
juce::AudioBuffer<float> noiseSample (int channels, int length, juce::int64 seed)
{
    juce::Random random { seed };
    juce::AudioBuffer<float> buffer (channels, length);
    for (int ch = 0; ch < channels; ++ch)
        for (int n = 0; n < length; ++n)
            buffer.setSample (ch, n, 0.3f * (random.nextFloat() * 2.0f - 1.0f));
    return buffer;
}

std::vector<float> renderNoise (float amount, PreDropEngine::Depths depths, bool alignedTone = false,
                                bool sampleRiser = false)
{
    constexpr double sr = 48000.0;
    constexpr int block = 512;
    const int blocks = (int) (6.0 * sr) / block;

    PreDropEngine engine;
    engine.setAmount (amount);
    engine.setHpfDepth (0.0f);
    engine.setReverbDepth (depths.reverb);
    engine.setDelayDepth (depths.delay);
    engine.setRiserDepth (depths.riser);
    if (sampleRiser)
        engine.setRiserSample (RiserSample::create (noiseSample (2, (int) (2.0 * sr), 7), sr, sr, "noise.wav"));
    engine.prepare (sr, 2, block);

    PinkNoise noise;
    juce::AudioBuffer<float> buffer (2, block);
    std::vector<float> out;
    out.reserve ((size_t) (blocks * block));

    for (int b = 0; b < blocks; ++b)
    {
        for (int n = 0; n < block; ++n)
        {
            const auto i = (double) (b * block + n);
            const float x = alignedTone ? 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * 400.0 * i / sr)
                                        : noise.next();
            buffer.setSample (0, n, x);
            buffer.setSample (1, n, x);
        }
        engine.process (buffer);
        for (int n = 0; n < block; ++n)
            out.push_back (buffer.getSample (0, n));
    }
    return out;
}

// One effect's wet level relative to the signal, in dB, over the settled last
// 2 s: the same render with that effect switched off is subtracted out.
float wetLevelDb (float PreDropEngine::Depths::* effect, float amount, float depth, bool alignedTone = false,
                  bool sampleRiser = false)
{
    PreDropEngine::Depths off;
    off.reverb = off.delay = off.riser = 0.0f;
    PreDropEngine::Depths on = off;
    on.*effect = depth;

    const auto dry = renderNoise (amount, off, alignedTone, sampleRiser);
    const auto wet = renderNoise (amount, on, alignedTone, sampleRiser);

    double signal = 0.0, effectOnly = 0.0;
    for (size_t i = dry.size() / 3 * 2; i < dry.size(); ++i)
    {
        signal     += (double) dry[i] * dry[i];
        effectOnly += (double) (wet[i] - dry[i]) * (wet[i] - dry[i]);
    }
    return (float) (10.0 * std::log10 (effectOnly / signal));
}

void testEffectsInSync()
{
    std::printf ("Reverb / delay / riser balance:\n");

    using D = PreDropEngine::Depths;
    const std::array<std::pair<const char*, float D::*>, 3> effects {{
        { "reverb", &D::reverb }, { "delay", &D::delay }, { "riser", &D::riser } }};

    // Expected: -6 dB at full Amount (default depth), -12 dB at half Amount,
    // 0 dB at full Amount with the depth at 100%.
    const std::array<std::array<float, 3>, 3> cases {{ { 1.0f, 0.5f, -6.02f },
                                                       { 0.5f, 0.5f, -12.04f },
                                                       { 1.0f, 1.0f,  0.0f } }};
    for (const auto& c : cases)
    {
        float lo = 1.0e9f, hi = -1.0e9f;
        for (const auto& [name, member] : effects)
        {
            const float db = wetLevelDb (member, c[0], c[1]);
            lo = std::min (lo, db);
            hi = std::max (hi, db);
            check (approx (db, c[2], 1.5f),
                   std::string (name) + " at Amount " + std::to_string ((int) (c[0] * 100)) + "%, depth "
                     + std::to_string ((int) (c[1] * 100)) + "% sits at " + std::to_string (c[2]).substr (0, 5)
                     + " dB re the signal (measured " + std::to_string (db).substr (0, 5) + ")");
        }
        check (hi - lo < 2.0f, "the three effects are within 2 dB of each other");
    }

    // Worst case for the delay: echoes that line up with the input add in
    // phase, 1 / (1 - fb) instead of the energy sum. With feedback capped at
    // 0.6 that is +6 dB over the balanced level, never more.
    const float alignedDefault = wetLevelDb (&D::delay, 1.0f, 0.5f, true);
    const float alignedMax     = wetLevelDb (&D::delay, 1.0f, 1.0f, true);
    check (alignedDefault < 0.5f,
           "in-phase echoes at the default depth stay at or below the signal level (measured "
             + std::to_string (alignedDefault).substr (0, 5) + " dB)");
    check (alignedMax < 6.5f,
           "in-phase echoes at depth 100% stay within +6 dB of the signal (measured "
             + std::to_string (alignedMax).substr (0, 5) + " dB)");
}

// ---- Test 3d: a user riser sample is prepared for clean, balanced looping. -----
void testRiserSample()
{
    std::printf ("Riser sample:\n");

    check (RiserSample::create (juce::AudioBuffer<float> (2, 0), 44100.0, 48000.0, "empty") == nullptr,
           "an empty file is rejected");
    juce::AudioBuffer<float> silent (2, 44100);
    silent.clear();
    check (RiserSample::create (silent, 44100.0, 48000.0, "silent") == nullptr, "a silent file is rejected");

    // 1 s at 44.1 kHz becomes 1 s at 48 kHz, minus the 20 ms loop crossfade.
    const auto resampled = RiserSample::create (noiseSample (2, 44100, 1), 44100.0, 48000.0, "noise.wav");
    check (resampled != nullptr && std::abs (resampled->length() - (48000 - 960)) <= 2,
           "a 44.1 kHz file is resampled to the engine's 48 kHz");
    check (resampled != nullptr && resampled->name == "noise.wav", "the sample keeps its file name");

    double sum = 0.0;
    for (int ch = 0; ch < resampled->audio.getNumChannels(); ++ch)
        for (int n = 0; n < resampled->length(); ++n)
            sum += std::pow ((double) resampled->audio.getSample (ch, n) * resampled->unitGain, 2.0);
    const double rmsAtUnitGain = std::sqrt (sum / (2.0 * resampled->length()));
    check (approx ((float) rmsAtUnitGain, 1.0f, 1.0e-3f), "unitGain brings the sample to unit RMS");

    check (RiserSample::create (noiseSample (1, 48000, 2), 48000.0, 48000.0, "mono")->audio.getNumChannels() == 1
        && RiserSample::create (noiseSample (4, 48000, 3), 48000.0, 48000.0, "quad")->audio.getNumChannels() == 2,
           "mono stays mono and surround files keep their first two channels");

    // A tone that doesn't fit a whole number of cycles in the file would click
    // at the loop point; the crossfade keeps the wrap as smooth as the tone.
    juce::AudioBuffer<float> tone (1, 48000);
    for (int n = 0; n < tone.getNumSamples(); ++n)
        tone.setSample (0, n, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 437.3 * n / 48000.0));
    const auto looped = RiserSample::create (tone, 48000.0, 48000.0, "tone");
    const float* y = looped->audio.getReadPointer (0);
    float maxStep = 0.0f;
    for (int n = 1; n < looped->length(); ++n)
        maxStep = std::max (maxStep, std::abs (y[n] - y[n - 1]));
    const float wrapStep = std::abs (y[0] - y[looped->length() - 1]);
    check (wrapStep <= maxStep * 1.5f, "the loop point is click-free (wrap step "
                                         + std::to_string (wrapStep).substr (0, 5) + " vs largest step "
                                         + std::to_string (maxStep).substr (0, 5) + ")");

    // In the engine the sample sits at the same calibrated level as the noise.
    const float db = wetLevelDb (&PreDropEngine::Depths::riser, 1.0f, 0.5f, false, true);
    check (approx (db, -6.02f, 1.5f), "a sample riser sits at -6 dB re the signal like the noise (measured "
                                        + std::to_string (db).substr (0, 5) + ")");

    // A build-up starts the sample from its very first sample, however long
    // the riser sat silent before. The sample is +1 for exactly one block after
    // its loop crossfade and -1 after that, so the first active block's riser
    // output is positive only if playback starts at the top (starting a block
    // late, as a position that kept running while silent would, reads -1).
    {
        constexpr double sr = 48000.0;
        constexpr int block = 2048;
        const int fade = (int) (0.02 * sr);
        juce::AudioBuffer<float> steps (1, 48000);
        for (int n = 0; n < steps.getNumSamples(); ++n)
            steps.setSample (0, n, n < block + fade ? 1.0f : -1.0f);

        const auto riserOutput = [&] (float riserDepth)
        {
            PreDropEngine engine;
            engine.setHpfDepth (0.0f);
            engine.setReverbDepth (0.0f);
            engine.setDelayDepth (0.0f);
            engine.setRiserDepth (riserDepth);
            engine.setRiserSample (RiserSample::create (steps, sr, sr, "steps"));
            engine.prepare (sr, 1, block);

            std::vector<float> firstActive;
            for (int b = 0; b < 6; ++b)
            {
                engine.setAmount (b < 5 ? 0.0f : 1.0f);   // five silent blocks, then build
                auto buffer = makeSine (sr, 220.0, block, 1);
                engine.process (buffer);
                if (b == 5)
                    firstActive.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + block);
            }
            return firstActive;
        };

        const auto withRiser = riserOutput (0.5f);
        const auto without   = riserOutput (0.0f);
        double riserSum = 0.0;
        for (size_t i = 0; i < withRiser.size(); ++i)
            riserSum += withRiser[i] - without[i];
        check (riserSum > 0.0, "a build-up starts the sample from the top after silence");
    }

    // Swapping samples in and out while running stays finite, and the
    // swapped-out sample is handed back for the message thread to free.
    PreDropEngine engine;
    engine.setAmount (1.0f);
    engine.prepare (48000.0, 2, 512);
    bool finite = true;
    for (int b = 0; b < 60; ++b)
    {
        if (b == 10) engine.setRiserSample (RiserSample::create (noiseSample (2, 24000, 4), 48000.0, 48000.0, "a"));
        if (b == 30) engine.setRiserSample (RiserSample::none());
        if (b == 31) engine.collectRetiredRiserSamples();
        auto buffer = makeSine (48000.0, 220.0, 512);
        engine.process (buffer);
        for (int n = 0; n < 512; ++n)
            finite = finite && std::isfinite (buffer.getSample (0, n));
    }
    check (finite, "swapping riser samples while running keeps the output finite");
}

// ---- Test 3e: effects settled at "off" are skipped: bit-exact pass-through. ----
void testIdleEffectsPassThrough()
{
    std::printf ("Idle effects:\n");

    constexpr double sr = 48000.0;
    constexpr int block = 512;
    PreDropEngine engine;
    engine.prepare (sr, 2, block);

    const auto runBlocks = [&] (int count, float amount)
    {
        engine.setAmount (amount);
        float maxDiff = 0.0f;
        bool finite = true;
        for (int b = 0; b < count; ++b)
        {
            const auto in = makeSine (sr, 110.0, block);
            auto buffer = in;
            engine.process (buffer);
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < block; ++n)
                {
                    finite = finite && std::isfinite (buffer.getSample (ch, n));
                    maxDiff = std::max (maxDiff, std::abs (buffer.getSample (ch, n) - in.getSample (ch, n)));
                }
        }
        return std::pair { maxDiff, finite };
    };

    check (runBlocks (20, 0.0f).first == 0.0f, "at Amount 0 the output is bit-identical to the input");

    const auto [active, finite] = runBlocks (40, 1.0f);
    check (finite && active > 0.05f, "building up re-engages every effect from idle");

    runBlocks (10, 0.0f);   // let the 40 ms ramps settle back to off
    check (runBlocks (20, 0.0f).first == 0.0f, "back at Amount 0 it is bit-identical again once settled");

    // A linked track with nothing switched on: Amount up, every depth at 0.
    engine.setHpfDepth (0.0f);
    engine.setReverbDepth (0.0f);
    engine.setDelayDepth (0.0f);
    engine.setRiserDepth (0.0f);
    runBlocks (10, 1.0f);
    check (runBlocks (20, 1.0f).first == 0.0f, "with every depth at 0 the output is bit-identical at full Amount");
}

// ---- Test 3f: the high-pass fades in and out of idle without a click. --------
void testHpfIdleTransitionsAreSmooth()
{
    std::printf ("High-pass idle transitions:\n");

    // A 40 Hz bass line, where even a parked 20 Hz high-pass shifts the phase
    // by tens of degrees: switching straight between filtered and dry would
    // jump by a large fraction of the amplitude in one sample.
    constexpr double sr = 48000.0;
    constexpr int block = 512;
    constexpr double freq = 40.0;
    PreDropEngine engine;
    engine.setReverbDepth (0.0f);
    engine.setDelayDepth (0.0f);
    engine.setRiserDepth (0.0f);
    engine.prepare (sr, 1, block);

    long sampleIndex = 0;
    float previous = 0.0f;
    const auto largestStep = [&] (int blocks, float amount)
    {
        engine.setAmount (amount);
        float largest = 0.0f;
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buffer (1, block);
            for (int n = 0; n < block; ++n, ++sampleIndex)
                buffer.setSample (0, n, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * freq * (double) sampleIndex / sr));
            engine.process (buffer);
            for (int n = 0; n < block; ++n)
            {
                largest = std::max (largest, std::abs (buffer.getSample (0, n) - previous));
                previous = buffer.getSample (0, n);
            }
        }
        return largest;
    };

    largestStep (20, 0.0f);                                   // settled dry, high-pass idle
    const float natural = (float) (0.5 * juce::MathConstants<double>::twoPi * freq / sr);
    const float engage  = largestStep (20, 0.05f);            // HPF to ~24 Hz: blends in
    const float release = largestStep (40, 0.0f);             // back to 20 Hz: blends out, then idles

    check (engage < natural * 2.0f, "engaging the high-pass on a bass line is click-free (largest step "
                                       + std::to_string (engage / natural).substr (0, 4) + "x the tone's own)");
    check (release < natural * 2.0f, "parking the high-pass on a bass line is click-free (largest step "
                                        + std::to_string (release / natural).substr (0, 4) + "x the tone's own)");
}

// ---- Test 3g: wet only (for a return track) drops the dry signal exactly. -----
void testWetOnly()
{
    std::printf ("Wet-only output:\n");

    constexpr double sr = 48000.0;
    constexpr int block = 512;

    // Same input through a normal and a wet-only engine; with the high-pass and
    // riser out of the way the normal output is input + effects, so the two
    // must differ by exactly the input.
    const auto run = [&] (bool wetOnly, float amount, float reverb, float delay, float& maxAbs)
    {
        PreDropEngine engine;
        engine.setHpfDepth (0.0f);
        engine.setRiserDepth (0.0f);
        engine.setReverbDepth (reverb);
        engine.setDelayDepth (delay);
        engine.setWetOnly (wetOnly);
        engine.setAmount (amount);
        engine.prepare (sr, 2, block);
        std::vector<float> out;
        maxAbs = 0.0f;
        for (int b = 0; b < 60; ++b)
        {
            auto buffer = makeSine (sr, 330.0, block);
            engine.process (buffer);
            for (int n = 0; n < block; ++n)
            {
                out.push_back (buffer.getSample (0, n));
                maxAbs = std::max (maxAbs, std::abs (buffer.getSample (0, n)));
            }
        }
        return out;
    };

    float peak = 0.0f;
    run (true, 0.0f, 0.5f, 0.5f, peak);
    check (peak == 0.0f, "at Amount 0 a wet-only engine is silent (no dry leaks through)");

    run (true, 1.0f, 0.0f, 0.0f, peak);
    check (peak == 0.0f, "with reverb and delay at 0 a wet-only engine stays silent at full Amount");

    float normalPeak = 0.0f, wetPeak = 0.0f;
    const auto normal = run (false, 1.0f, 0.5f, 0.5f, normalPeak);
    const auto wet    = run (true,  1.0f, 0.5f, 0.5f, wetPeak);
    const auto dry = makeSine (sr, 330.0, block);   // every block carries this same input
    const auto input = [&] (size_t i) { return dry.getSample (0, (int) (i % (size_t) block)); };
    float worst = 0.0f;
    for (size_t i = 0; i < normal.size(); ++i)
        worst = std::max (worst, std::abs ((normal[i] - input (i)) - wet[i]));
    check (wetPeak > 0.05f && worst < 1.0e-4f, "wet only is exactly the normal output minus the dry signal (largest difference "
                                                 + std::to_string (worst) + ")");
}

// ---- Test 3c: degenerate blocks are a safe no-op. ------------------------------
void testEmptyBlocks()
{
    std::printf ("Empty blocks:\n");

    PreDropEngine engine;
    engine.prepare (48000.0, 2, 512);
    engine.setAmount (1.0f);

    juce::AudioBuffer<float> noSamples (2, 0);
    engine.process (noSamples);
    check (noSamples.getNumSamples() == 0, "a zero-length block is left alone");

    juce::AudioBuffer<float> noChannels (0, 512);
    engine.process (noChannels);
    check (noChannels.getNumChannels() == 0, "a zero-channel block is left alone");
}

// ---- Test 4: the editor's visual model stays in lock-step with the DSP. --------
void testVisualModelMatchesEngine()
{
    std::printf ("Visual model <-> engine coupling:\n");

    bool cutoffMatches = true;
    for (const float depth : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
        for (int i = 0; i <= 100; ++i)
        {
            const float a = (float) i / 100.0f;
            PreDropEngine::Depths d; d.hpf = depth;
            cutoffMatches = cutoffMatches
                         && approx (PreDropVisualModel::cutoffHz (a, depth), PreDropEngine::mapMacro (a, d).hpfCutoffHz, 0.05f);
        }
    check (cutoffMatches, "editor cutoff readout matches the engine HPF cutoff at every amount and depth");

    // The chips light up exactly when the engine's effects switch on.
    const auto at0 = PreDropEngine::mapMacro (0.0f);
    const auto at1pc = PreDropEngine::mapMacro (0.01f);
    check (! PreDropVisualModel::engaged (0.0f, 0.5f) && at0.reverbWet <= 0.0f && at0.riserLevel <= 0.0f,
           "at amount=0 nothing is engaged in UI or DSP");
    check (PreDropVisualModel::engaged (0.01f, 0.5f) && at1pc.reverbWet > 0.0f && at1pc.riserLevel > 0.0f,
           "at amount=1% everything is engaged in UI and DSP");

    PreDropEngine::Depths noDelay; noDelay.delay = 0.0f;
    check (! PreDropVisualModel::engaged (1.0f, 0.0f) && PreDropEngine::mapMacro (1.0f, noDelay).delayWet <= 0.0f,
           "a depth of 0 is idle in both UI and DSP");
}
} // namespace

int main()
{
    std::printf ("PreDropEngine tests\n===================\n");

    testMappingBoundaries();
    testDepthScaling();
    testNearPassthroughAtZero();
    testStabilityAtFull();
    testEffectsInSync();
    testRiserSample();
    testIdleEffectsPassThrough();
    testHpfIdleTransitionsAreSmooth();
    testWetOnly();
    testEmptyBlocks();
    testVisualModelMatchesEngine();

    std::printf ("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
