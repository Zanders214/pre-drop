/*
    PreDrop plugin tests.

    Drives the real PreDropAudioProcessor and its editor headlessly: parameter
    layout, bus support, audio, state save/restore, and offscreen renders of the
    whole editor (both panel states, two sizes) so every paint / layout path runs
    exactly as it does in a host. The DSP maths has its own tests in
    PreDropEngineTests.cpp; this file covers the host-facing wrapper and the UI.

    Same plain check() + int main() shape as the other tests, so it plugs into
    CTest without a framework. On Linux CTest runs it under xvfb-run.
*/

#include <juce_audio_utils/juce_audio_utils.h>

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "PreDropLookAndFeel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
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

float param (const PreDropAudioProcessor& p, const char* id)
{
    return p.getState().getRawParameterValue (id)->load();
}

void setParam (PreDropAudioProcessor& p, const char* id, float value)
{
    auto* prm = p.getState().getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

// Paints a component (and its visible children) into an offscreen software
// image, so the render is identical on every platform and needs no window.
juce::Image render (juce::Component& c)
{
    juce::Image image (juce::Image::ARGB, c.getWidth(), c.getHeight(), true, juce::SoftwareImageType());
    juce::Graphics g (image);
    c.paintEntireComponent (g, true);
    return image;
}

// True when a render drew real content rather than a flat fill.
bool hasDetail (const juce::Image& image)
{
    const auto reference = image.getPixelAt (0, 0);
    int differing = 0;
    for (int y = 0; y < image.getHeight(); y += 4)
        for (int x = 0; x < image.getWidth(); x += 4)
            if (image.getPixelAt (x, y) != reference)
                ++differing;
    return differing > 100;
}

template <typename ComponentType>
ComponentType* findChild (juce::Component& parent)
{
    for (auto* child : parent.getChildren())
        if (auto* match = dynamic_cast<ComponentType*> (child))
            return match;
    return nullptr;
}

// All direct children of a type, in the order the editor added them.
template <typename ComponentType>
std::vector<ComponentType*> findChildren (juce::Component& parent)
{
    std::vector<ComponentType*> found;
    for (auto* child : parent.getChildren())
        if (auto* match = dynamic_cast<ComponentType*> (child))
            found.push_back (match);
    return found;
}

// How many sampled pixels differ between two same-sized renders.
int pixelsChanged (const juce::Image& a, const juce::Image& b)
{
    int changed = 0;
    for (int y = 0; y < a.getHeight(); y += 2)
        for (int x = 0; x < a.getWidth(); x += 2)
            if (a.getPixelAt (x, y) != b.getPixelAt (x, y))
                ++changed;
    return changed;
}

// Runs a sine through the processor's real audio callback and returns how far
// the final block's output is from its input (RMS of the difference), by which
// time the 40 ms parameter ramps have settled.
float settledChange (PreDropAudioProcessor& p, int block, int blocks)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, block);
    juce::AudioBuffer<float> input (2, block);

    for (int b = 0; b < blocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < block; ++n)
                input.setSample (ch, n, 0.5f * std::sin (0.05f * (float) (b * block + n)));

        buffer.makeCopyOf (input, true);
        p.processBlock (buffer, midi);
    }

    double sum = 0.0;
    for (int n = 0; n < block; ++n)
    {
        const double d = (double) buffer.getSample (0, n) - input.getSample (0, n);
        sum += d * d;
    }
    return (float) std::sqrt (sum / block);
}

// Runs a sine through the processor and returns the RMS of the final block's
// output itself (for a return, whose output is only the effects).
float settledOutputRms (PreDropAudioProcessor& p, int block, int blocks)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, block);
    for (int b = 0; b < blocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < block; ++n)
                buffer.setSample (ch, n, 0.5f * std::sin (0.05f * (float) (b * block + n)));
        p.processBlock (buffer, midi);
    }
    double sum = 0.0;
    for (int n = 0; n < block; ++n)
        sum += (double) buffer.getSample (0, n) * buffer.getSample (0, n);
    return (float) std::sqrt (sum / block);
}

// Writes a 16-bit WAV of `seconds` of noise (or silence) to the temp folder.
juce::File writeWav (const juce::String& name, double sampleRate, double seconds, int channels, bool silent = false)
{
    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    file.deleteFile();

    juce::AudioBuffer<float> audio (channels, (int) (sampleRate * seconds));
    juce::Random random { 5 };
    for (int ch = 0; ch < channels; ++ch)
        for (int n = 0; n < audio.getNumSamples(); ++n)
            audio.setSample (ch, n, silent ? 0.0f : 0.2f * (random.nextFloat() * 2.0f - 1.0f));

    juce::WavAudioFormat wav;
    if (auto writer = std::unique_ptr<juce::AudioFormatWriter> (wav.createWriterFor (
            new juce::FileOutputStream (file), sampleRate, (unsigned int) channels, 16, {}, 0)))
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    return file;
}

void pumpMessages (int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
}

// Pumps the message loop until `done` holds or ~2 s pass; true if it held.
// For changes the editor only picks up on its 30 Hz timer, which a busy CI
// runner can delay well past one tick.
template <typename Condition>
bool pumpUntil (Condition done)
{
    for (int i = 0; i < 100 && ! done(); ++i)
        pumpMessages (20);
    return done();
}

// A mouse event at `pos`; for a drag, `downPos` is where the button went down.
juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, bool dragged,
                          std::optional<juce::Point<float>> downPos = std::nullopt, int clicks = 1)
{
    const auto now = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), pos, {},
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &c, &c, now, downPos.value_or (pos), now, clicks, dragged };
}

// Clicks (down + up, no drag) at `pos`.
void click (juce::Component& c, juce::Point<float> pos)
{
    c.mouseDown (mouseAt (c, pos, false));
    c.mouseUp (mouseAt (c, pos, false));
}

// ---- Processor --------------------------------------------------------------
void testProcessorBasics()
{
    std::printf ("Processor basics:\n");
    PreDropAudioProcessor p;

    check (p.getName() == "ZandersPreDrop", "plugin reports its product name");
    check (p.hasEditor(), "plugin has an editor");
    check (! p.acceptsMidi() && ! p.producesMidi() && ! p.isMidiEffect(), "plugin is a pure audio effect");
    check (approx ((float) p.getTailLengthSeconds(), 4.0f, 1.0e-6f), "tail covers the reverb / delay decay");

    check (p.getNumPrograms() == 1 && p.getCurrentProgram() == 0, "a single program");
    p.setCurrentProgram (0);
    p.changeProgramName (0, "ignored");
    check (p.getProgramName (0).isEmpty(), "the program is unnamed");

    check (p.getParameters().size() == 11, "eleven parameters are exposed");
    check (! p.getState().getParameter ("role")->isAutomatable(), "the MAIN / TRACK / RETURN role stays out of automation");
    check (approx (param (p, "amount"), 0.0f, 1.0e-6f), "amount defaults to 0");
    check (approx (param (p, "mix"), 1.0f, 1.0e-6f), "mix defaults to fully wet");
    check (approx (param (p, "outputTrim"), 0.0f, 1.0e-6f), "output trim defaults to 0 dB");
    for (auto* id : { "reverbDepth", "delayDepth", "riserDepth", "hpfDepth" })
        check (approx (param (p, id), 0.5f, 1.0e-6f), std::string (id) + " defaults to the balanced 50%");

    check (p.getState().getParameter ("amount")->getText (0.5f, 16) == "50 %", "amount displays as a percentage");
}

void testBusLayouts()
{
    std::printf ("Bus layouts:\n");
    PreDropAudioProcessor p;

    const auto layout = [] (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (in);
        l.outputBuses.add (out);
        return l;
    };

    using Set = juce::AudioChannelSet;
    check (p.checkBusesLayoutSupported (layout (Set::stereo(), Set::stereo())), "stereo in / stereo out is supported");
    check (p.checkBusesLayoutSupported (layout (Set::mono(), Set::mono())), "mono in / mono out is supported");
    check (! p.checkBusesLayoutSupported (layout (Set::mono(), Set::stereo())), "mismatched in / out is rejected");
    check (! p.checkBusesLayoutSupported (layout (Set::create5point1(), Set::create5point1())), "surround is rejected");
}

void testProcessing()
{
    std::printf ("Processing:\n");
    PreDropAudioProcessor p;
    constexpr int block = 512;
    p.prepareToPlay (48000.0, block);
    setParam (p, "amount", 1.0f);

    juce::MidiBuffer midi;
    bool  finite = true;
    float peak   = 0.0f;

    for (int b = 0; b < 40; ++b)
    {
        juce::AudioBuffer<float> buffer (2, block);
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < block; ++n)
                buffer.setSample (ch, n, 0.5f * std::sin (0.05f * (float) (b * block + n)));

        p.processBlock (buffer, midi);

        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < block; ++n)
            {
                const float s = buffer.getSample (ch, n);
                finite = finite && std::isfinite (s);
                peak   = juce::jmax (peak, std::abs (s));
            }
    }

    check (finite, "output stays finite at full build-up");
    check (peak > 0.01f && peak < 8.0f, "output is audible and bounded");

    // Prove the callback really runs the effect chain: at full Amount the output
    // must clearly differ from the input, and pulling Mix to 0 must return the
    // untouched dry signal once the smoothers settle.
    const float wetChange = settledChange (p, block, 40);
    check (wetChange > 0.05f, "at full Amount the processor audibly changes the signal");

    setParam (p, "mix", 0.0f);
    const float dryChange = settledChange (p, block, 20);
    check (dryChange < 1.0e-5f, "Mix at 0 passes the dry signal through untouched");
    p.releaseResources();
}

void testStateRoundTrip()
{
    std::printf ("State save / restore:\n");
    PreDropAudioProcessor source;
    setParam (source, "amount", 0.7f);
    setParam (source, "mix", 0.4f);
    setParam (source, "riserDepth", 0.25f);

    juce::MemoryBlock state;
    source.getStateInformation (state);
    check (state.getSize() > 0, "state serialises to a non-empty block");

    PreDropAudioProcessor restored;
    restored.setStateInformation (state.getData(), (int) state.getSize());
    check (approx (param (restored, "amount"), 0.7f, 1.0e-4f), "amount is restored");
    check (approx (param (restored, "mix"), 0.4f, 1.0e-4f), "mix is restored");
    check (approx (param (restored, "riserDepth"), 0.25f, 1.0e-4f), "riser depth is restored");

    // A state from some other plugin (wrong root tag) must be ignored.
    juce::MemoryBlock foreign;
    juce::AudioProcessor::copyXmlToBinary (juce::XmlElement ("SOMETHING_ELSE"), foreign);
    PreDropAudioProcessor untouched;
    untouched.setStateInformation (foreign.getData(), (int) foreign.getSize());
    check (approx (param (untouched, "amount"), 0.0f, 1.0e-6f), "a foreign state is ignored");

    // Bytes that are not a saved state at all must be ignored too.
    const char junk[] = "not a plugin state";
    untouched.setStateInformation (junk, (int) sizeof (junk));
    check (approx (param (untouched, "amount"), 0.0f, 1.0e-6f), "malformed state data is ignored");
}

// ---- Editor -----------------------------------------------------------------
void testEditor()
{
    std::printf ("Editor:\n");
    PreDropAudioProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());

    check (editor != nullptr, "createEditor returns an editor");
    check (editor->getWidth() == 360 && editor->getHeight() == 540, "editor opens at the 360 x 540 design size");

    // Render across the build-up, with the sliders at both ends of their range,
    // so every arc / fill branch of the look-and-feel is drawn.
    bool allDetailed = true;
    for (const float amount : { 0.0f, 0.3f, 0.65f, 1.0f })
    {
        setParam (p, "amount", amount);
        setParam (p, "mix", amount);
        setParam (p, "outputTrim", amount > 0.5f ? -12.0f : 3.0f);
        pumpMessages (60);   // let the attachments + 30 Hz timer pick the values up
        allDetailed = allDetailed && hasDetail (render (*editor));
    }
    check (allDetailed, "editor renders across the whole Amount range");

    // Controls and parameters must be wired both ways: automation moves the
    // knob / sliders, and moving a control writes its parameter. Children are in
    // the order the editor adds them: Amount knob, Mix, Output, then the depths.
    const auto sliders = findChildren<juce::Slider> (*editor);
    check (sliders.size() == 7, "editor has the Amount knob, two sliders and four depth knobs");
    if (sliders.size() != 7)
        return;

    auto* knob = sliders[0];
    auto* mix  = sliders[1];
    auto* trim = sliders[2];
    auto* reverbDepth = sliders[3];
    check (knob->getSliderStyle() == juce::Slider::RotaryVerticalDrag && knob->isVisible()
             && mix->getSliderStyle() == juce::Slider::LinearHorizontal
             && trim->getSliderStyle() == juce::Slider::LinearHorizontal,
           "controls are laid out as knob, Mix, Output");

    setParam (p, "amount", 0.42f);
    setParam (p, "mix", 0.3f);
    setParam (p, "outputTrim", -6.0f);
    setParam (p, "reverbDepth", 0.2f);
    pumpMessages (60);
    check (approx ((float) knob->getValue(), 0.42f, 1.0e-3f), "Amount automation moves the knob");
    check (approx ((float) mix->getValue(), 0.3f, 1.0e-3f), "Mix automation moves the Mix slider");
    check (approx ((float) trim->getValue(), -6.0f, 0.05f), "Output automation moves the Output slider");
    check (approx ((float) reverbDepth->getValue(), 0.2f, 1.0e-3f), "depth automation moves its knob");

    knob->setValue (0.8, juce::sendNotificationSync);
    mix->setValue (0.6, juce::sendNotificationSync);
    trim->setValue (2.0, juce::sendNotificationSync);
    reverbDepth->setValue (0.9, juce::sendNotificationSync);
    check (approx (param (p, "amount"), 0.8f, 1.0e-3f), "turning the knob sets Amount");
    check (approx (param (p, "mix"), 0.6f, 1.0e-3f), "moving the Mix slider sets Mix");
    check (approx (param (p, "outputTrim"), 2.0f, 0.05f), "moving the Output slider sets Output Trim");
    check (approx (param (p, "reverbDepth"), 0.9f, 1.0e-3f), "turning a depth knob sets its depth");

    // The knob itself must visibly track Amount (ring arc, pointer, readout).
    setParam (p, "amount", 0.0f);
    pumpMessages (60);
    const auto knobAtZero = render (*knob);
    setParam (p, "amount", 1.0f);
    pumpMessages (60);
    check (pixelsChanged (knobAtZero, render (*knob)) > 200, "the Amount knob redraws when Amount changes");

    auto* button = findChild<juce::TextButton> (*editor);
    auto* curve  = findChild<BuildUpCurve> (*editor);
    check (button != nullptr && curve != nullptr, "EFFECTS button and build-up curve are present");
    if (button == nullptr || curve == nullptr)
        return;

    // Open the per-effect depth panel: it replaces the curve in the same slot.
    button->triggerClick();
    pumpMessages (60);
    check (button->getToggleState() && ! curve->isVisible(), "EFFECTS opens the depth panel in place of the curve");

    setParam (p, "reverbDepth", 0.0f);
    setParam (p, "hpfDepth", 0.5f);
    pumpMessages (60);
    check (hasDetail (render (*editor)), "editor renders with the depth panel open");

    button->triggerClick();
    pumpMessages (60);
    check (! button->getToggleState() && curve->isVisible(), "EFFECTS again restores the curve");

    // Resizing re-runs the proportional layout at double scale.
    editor->setSize (720, 1080);
    check (curve->getWidth() > 400, "layout scales with the editor size");
    check (hasDetail (render (*editor)), "editor renders at 2x");
}

void testBuildUpCurveDrag()
{
    std::printf ("Build-up curve dragging:\n");
    PreDropAudioProcessor p;
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    auto* curve = findChild<BuildUpCurve> (*editor);
    check (curve != nullptr, "build-up curve is present");
    if (curve == nullptr)
        return;

    const auto w = (float) curve->getWidth();
    const auto y = (float) curve->getHeight() * 0.5f;

    curve->mouseDown (mouseAt (*curve, { w * 0.25f, y }, false));
    check (approx (param (p, "amount"), 0.25f, 0.02f), "clicking the curve sets Amount to that position");

    curve->mouseDrag (mouseAt (*curve, { w * 0.75f, y }, true));
    check (approx (param (p, "amount"), 0.75f, 0.02f), "dragging the curve follows the pointer");

    curve->mouseDrag (mouseAt (*curve, { w * 2.0f, y }, true));
    check (approx (param (p, "amount"), 1.0f, 1.0e-4f), "dragging past the end clamps at 100%");

    curve->mouseUp (mouseAt (*curve, { w * 2.0f, y }, true));
}

void testRiserSampleLoading()
{
    std::printf ("Riser sample (processor):\n");

    const auto riserFile = writeWav ("predrop-test-riser.wav", 44100.0, 0.5, 2);
    PreDropAudioProcessor p;
    p.prepareToPlay (48000.0, 512);

    check (p.getRiserSampleName().isEmpty(), "the built-in noise riser is the default");
    check (p.isSupportedRiserFile (riserFile) && ! p.isSupportedRiserFile (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("notes.txt")),
           "audio files are accepted and other files are not");

    const auto error = p.loadRiserSample (riserFile);
    check (error.isEmpty() && p.getRiserSampleName() == "predrop-test-riser.wav",
           "a WAV loads as the riser (" + (error.isEmpty() ? juce::String ("ok") : error).toStdString() + ")");

    setParam (p, "amount", 1.0f);
    const float change = settledChange (p, 512, 40);
    check (std::isfinite (change) && change > 0.05f, "the sample riser plays through the processor");

    // The sample travels inside the saved state, so a project keeps its riser.
    juce::MemoryBlock state;
    p.getStateInformation (state);
    riserFile.deleteFile();   // gone from disk: the restore must use the embedded copy

    PreDropAudioProcessor restored;
    restored.setStateInformation (state.getData(), (int) state.getSize());
    check (restored.getRiserSampleName() == "predrop-test-riser.wav", "the riser sample is restored from the saved state");
    restored.prepareToPlay (48000.0, 512);
    check (std::isfinite (settledChange (restored, 512, 10)), "a restored riser plays once the host prepares it");

    // A state saved with the built-in noise clears a loaded sample.
    PreDropAudioProcessor noiseOnly;
    juce::MemoryBlock noiseState;
    noiseOnly.getStateInformation (noiseState);
    restored.setStateInformation (noiseState.getData(), (int) noiseState.getSize());
    check (restored.getRiserSampleName().isEmpty(), "restoring a noise-riser state goes back to noise");

    p.clearRiserSample();
    check (p.getRiserSampleName().isEmpty(), "clearing goes back to the built-in noise");

    // Files that can't be used are refused with a reason, leaving the riser as it was.
    const auto silent  = writeWav ("predrop-test-silent.wav", 44100.0, 0.5, 1, true);
    const auto tooLong = writeWav ("predrop-test-long.wav", 8000.0, PreDropAudioProcessor::maxRiserSeconds + 1.0, 1);
    const auto text    = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("predrop-test.txt");
    text.replaceWithText ("not audio");

    check (p.loadRiserSample (silent).contains ("silent"), "a silent file is refused");
    check (p.loadRiserSample (tooLong).contains ("longer than"), "an over-long file is refused");
    check (p.loadRiserSample (text).isNotEmpty(), "a non-audio file is refused");
    check (p.loadRiserSample (juce::File ("/no/such/riser.wav")).contains ("could not be found"), "a missing file is refused");
    check (p.getRiserSampleName().isEmpty(), "a refused file leaves the riser unchanged");

    for (const auto& f : { silent, tooLong, text })
        f.deleteFile();
}

void testRiserSampleEditor()
{
    std::printf ("Riser sample (editor):\n");

    const auto riserFile = writeWav ("predrop-editor-riser.wav", 48000.0, 0.5, 2);
    PreDropAudioProcessor p;
    p.prepareToPlay (48000.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    auto* ed = dynamic_cast<PreDropAudioProcessorEditor*> (editor.get());

    juce::TextButton* riserButton = nullptr;
    for (auto* button : findChildren<juce::TextButton> (*editor))
        if (button->getButtonText().startsWith ("RISER"))
            riserButton = button;

    check (ed != nullptr && riserButton != nullptr, "the editor has a RISER button");
    if (ed == nullptr || riserButton == nullptr)
        return;

    check (riserButton->getButtonText() == "RISER: NOISE" && ! riserButton->getToggleState(),
           "it shows the built-in noise by default");

    check (ed->isInterestedInFileDrag ({ riserFile.getFullPathName() }), "dragging one audio file is accepted");
    check (! ed->isInterestedInFileDrag ({ "/tmp/notes.txt" }), "dragging a non-audio file is not");
    check (! ed->isInterestedInFileDrag ({ riserFile.getFullPathName(), riserFile.getFullPathName() }),
           "dragging several files is not");

    // While a file hovers, the face shows a drop target over everything.
    const auto before = render (*editor);
    ed->fileDragEnter ({ riserFile.getFullPathName() }, 10, 10);
    check (pixelsChanged (before, render (*editor)) > 2000, "hovering a file shows the drop overlay");
    ed->fileDragExit ({ riserFile.getFullPathName() });
    check (pixelsChanged (before, render (*editor)) == 0, "leaving removes the overlay again");

    ed->fileDragEnter ({ riserFile.getFullPathName() }, 10, 10);
    ed->filesDropped ({ riserFile.getFullPathName() }, 10, 10);
    check (p.getRiserSampleName() == "predrop-editor-riser.wav", "dropping the file loads it as the riser");
    const auto label = riserButton->getButtonText();
    check (label.startsWith ("RISER: PRED") && label.endsWith (juce::String::charToString ((juce::juce_wchar) 0x2026)),
           "the button shows the sample's name, shortened to fit (" + label.toStdString() + ")");
    check (riserButton->getToggleState(), "the button lights up while a sample is in use");

    // (The button's menu is a native popup window, which can't be opened
    // headlessly; its "Use built-in noise" item calls clearRiserSample.)
    p.clearRiserSample();
    check (pumpUntil ([&] { return riserButton->getButtonText() == "RISER: NOISE"; })
             && ! riserButton->getToggleState(),
           "going back to noise resets the button");

    riserFile.deleteFile();
}

// ---- Linking: MAIN on the master, TRACKs on chosen tracks -------------------
juce::AudioProcessor::TrackProperties hostTrack (const juce::String& name)
{
    juce::AudioProcessor::TrackProperties props;
    props.name = name;
    return props;
}

void testLinkHub()
{
    std::printf ("Link hub:\n");
    LinkHub hub;
    const int a = 0, b = 0;

    check (! hub.hasMain() && hub.claimMain (&a) && hub.isMain (&a), "the first instance to claim becomes the main");
    check (! hub.claimMain (&b) && ! hub.isMain (&b), "a second claim is refused while the main exists");
    check (hub.claimMain (&a), "claiming again as the main is fine");

    check (! hub.linkActive(), "with nothing linked the link is idle: the main works on the whole mix");
    hub.addReturn (&b);
    check (hub.linkActive(), "linking a follower activates it");
    hub.releaseMain (&b);
    check (hub.isMain (&a), "only the main itself can release the slot");
    hub.releaseMain (&a);
    check (! hub.hasMain() && ! hub.linkActive(), "releasing the main deactivates the link");
    hub.removeReturn (&b);
    check (hub.claimMain (&b), "the slot is free for the next main");
}

void testLinkedProcessing()
{
    std::printf ("Linked processing:\n");
    using Role = PreDropAudioProcessor::Role;
    using Effect = LinkHub::Effect;

    // A TRACK with no main anywhere passes its audio through untouched.
    {
        PreDropAudioProcessor orphan;
        orphan.setRole (Role::track);
        orphan.prepareToPlay (48000.0, 512);
        setParam (orphan, "amount", 1.0f);
        check (settledChange (orphan, 512, 20) == 0.0f, "a TRACK with no main passes audio through untouched");
    }

    PreDropAudioProcessor master;
    check (master.getRole() == Role::main && master.isLinkMain(), "the first PreDrop is a MAIN and takes the link");
    master.prepareToPlay (48000.0, 512);
    setParam (master, "amount", 1.0f);
    setParam (master, "riserDepth", 0.0f);   // keeps the main's own output comparable
    check (! master.getLinkHub().linkActive() && settledChange (master, 512, 40) > 0.05f,
           "with nothing linked the main processes the whole mix");

    // Adding PreDrop to tracks is all it takes: they join as TRACKs.
    PreDropAudioProcessor guitar, drums;
    check (guitar.getRole() == Role::track && drums.getRole() == Role::track,
           "PreDrops added while a main exists join as TRACKs");
    check (master.getLinkHub().getTracks().size() == 2 && master.getLinkHub().linkActive(), "they appear in the main's list");
    check (guitar.getTrackName() == "Track 1" && drums.getTrackName() == "Track 2", "unnamed tracks are numbered");

    drums.updateTrackProperties (hostTrack ("Drums"));
    guitar.updateTrackProperties (hostTrack ("Audio 3"));
    guitar.setTrackName ("Guitar");
    check (drums.getTrackName() == "Drums", "the host's track name is used");
    check (guitar.getTrackName() == "Guitar", "a name typed into PreDrop wins over the host's");

    for (auto e : { Effect::hpf, Effect::reverb, Effect::delay })
        drums.setLinkLevel (e, 0.0f);

    for (auto* p : { &guitar, &drums })
        p->prepareToPlay (48000.0, 512);

    // Linked: tracks apply their effects; the main adds only the riser.
    check (settledChange (master, 512, 40) == 0.0f, "once tracks link, the main leaves high-pass, reverb and delay to them");
    check (settledChange (guitar, 512, 40) > 0.05f, "a track with its effects on follows the main's Amount");
    check (settledChange (drums, 512, 40) == 0.0f, "a track with every effect off is untouched");

    drums.setLinkLevel (Effect::reverb, 1.0f);
    check (settledChange (drums, 512, 40) > 0.01f, "raising one effect's level for a track applies it");

    // A track's level is its share of the main's depth: two tracks fed the same
    // signal, reverb only, one at 100% and one at 50%, differ by exactly half.
    {
        PreDropAudioProcessor full, half;
        for (auto* p : { &full, &half })
        {
            p->setRole (Role::track);
            p->setLinkLevel (Effect::hpf, 0.0f);
            p->setLinkLevel (Effect::delay, 0.0f);
            p->prepareToPlay (48000.0, 512);
        }
        half.setLinkLevel (Effect::reverb, 0.5f);
        const float fullReverb = settledChange (full, 512, 60);
        const float halfReverb = settledChange (half, 512, 60);
        check (fullReverb > 0.01f && approx (halfReverb / fullReverb, 0.5f, 0.02f),
               "a track at 50% gets half the reverb of one at 100% (ratio "
                 + std::to_string (halfReverb / fullReverb).substr (0, 5) + ")");
    }

    setParam (master, "amount", 0.0f);
    settledChange (guitar, 512, 10);                         // let the ramps settle
    check (settledChange (guitar, 512, 20) == 0.0f, "Amount at 0 on the main returns the tracks to dry");

    // Roles survive a save / reload, and so do a track's name and switches.
    juce::MemoryBlock state;
    drums.getStateInformation (state);
    PreDropAudioProcessor reloaded;
    reloaded.setStateInformation (state.getData(), (int) state.getSize());
    check (reloaded.getRole() == Role::track && approx (reloaded.getLinkLevel (Effect::reverb), 1.0f, 1.0e-6f)
             && approx (reloaded.getLinkLevel (Effect::delay), 0.0f, 1.0e-6f) && master.getLinkHub().getTracks().size() == 3,
           "a reloaded TRACK relinks with its levels");

    // A second MAIN (chosen explicitly) works on its own until the first steps down.
    PreDropAudioProcessor second;
    second.setRole (Role::main);
    check (! second.isLinkMain(), "a second MAIN does not take over the link");
    master.setRole (Role::track);
    check (pumpUntil ([&] { return second.isLinkMain(); }), "when the main steps down, another MAIN takes the link");
}

void testLinkingEditors()
{
    std::printf ("Linking editors:\n");
    using Role = PreDropAudioProcessor::Role;
    using Effect = LinkHub::Effect;

    PreDropAudioProcessor master, guitar, keys;
    guitar.setRole (Role::track);
    keys.setRole (Role::track);
    guitar.setTrackName ("Guitar");
    keys.setTrackName ("Keys");
    setParam (master, "amount", 0.7f);

    std::unique_ptr<juce::AudioProcessorEditor> mainEditor (master.createEditor());
    juce::TextButton* roleButton = nullptr;
    juce::TextButton* tracksButton = nullptr;
    for (auto* b : findChildren<juce::TextButton> (*mainEditor))
    {
        if (b->getButtonText() == "MAIN")   roleButton = b;
        if (b->getButtonText() == "TRACKS") tracksButton = b;
    }
    auto* matrix = findChild<TrackMatrix> (*mainEditor);
    check (roleButton != nullptr && tracksButton != nullptr && matrix != nullptr,
           "the main shows a MAIN pill, a TRACKS button and the tracks panel");
    if (roleButton == nullptr || tracksButton == nullptr || matrix == nullptr)
        return;

    tracksButton->triggerClick();
    pumpMessages (40);
    check (matrix->isVisible() && ! findChild<BuildUpCurve> (*mainEditor)->isVisible(), "TRACKS opens the panel in the curve's place");
    matrix->refresh();
    check (hasDetail (render (*matrix)), "the panel lists the linked tracks");

    click (*matrix, matrix->cell (1, Effect::reverb).getCentre());
    check (guitar.getLinkLevel (Effect::reverb) == 0.0f && keys.getLinkLevel (Effect::reverb) == 1.0f,
           "clicking a track's REV level turns reverb off for that track only");
    click (*matrix, matrix->cell (1, Effect::reverb).getCentre());
    check (guitar.getLinkLevel (Effect::reverb) == 1.0f, "clicking it again brings it back to 100%");

    // Dragging a cell down by 30 px (half the 60 px travel) sets it to 50%.
    const auto delayCell = matrix->cell (1, Effect::delay).getCentre();
    matrix->mouseDown (mouseAt (*matrix, delayCell, false));
    matrix->mouseDrag (mouseAt (*matrix, delayCell.translated (0.0f, 30.0f), true, delayCell));
    matrix->mouseUp (mouseAt (*matrix, delayCell.translated (0.0f, 30.0f), true, delayCell));
    check (approx (guitar.getLinkLevel (Effect::delay), 0.5f, 0.02f) && keys.getLinkLevel (Effect::delay) == 1.0f,
           "dragging a track's DLY level sets it (to "
             + std::to_string (guitar.getLinkLevel (Effect::delay)).substr (0, 4) + ")");
    matrix->mouseDoubleClick (mouseAt (*matrix, delayCell, false, std::nullopt, 2));
    check (guitar.getLinkLevel (Effect::delay) == 1.0f, "double-clicking a level resets it to 100%");

    click (*matrix, matrix->cell (0, Effect::delay).getCentre());
    check (guitar.getLinkLevel (Effect::delay) == 0.0f && keys.getLinkLevel (Effect::delay) == 0.0f,
           "clicking ALL turns a column off for every track");
    click (*matrix, matrix->cell (0, Effect::delay).getCentre());
    check (guitar.getLinkLevel (Effect::delay) == 1.0f && keys.getLinkLevel (Effect::delay) == 1.0f, "and back to 100%");

    const auto allHpf = matrix->cell (0, Effect::hpf).getCentre();
    matrix->mouseDown (mouseAt (*matrix, allHpf, false));
    matrix->mouseDrag (mouseAt (*matrix, allHpf.translated (0.0f, 45.0f), true, allHpf));
    matrix->mouseUp (mouseAt (*matrix, allHpf.translated (0.0f, 45.0f), true, allHpf));
    check (approx (guitar.getLinkLevel (Effect::hpf), 0.25f, 0.02f) && approx (keys.getLinkLevel (Effect::hpf), 0.25f, 0.02f),
           "dragging ALL sets a column for every track");
    matrix->refresh();
    check (hasDetail (render (*mainEditor)), "the main renders with the tracks list");

    // More tracks than fit: the list scrolls with the mouse wheel.
    std::array<PreDropAudioProcessor, 3> more;
    for (auto& extra : more)
        extra.setRole (Role::track);
    matrix->refresh();
    const auto top = render (*matrix);
    juce::MouseWheelDetails wheel {};
    wheel.deltaY = -1.0f;
    matrix->mouseWheelMove (mouseAt (*matrix, { 20.0f, 60.0f }, false), wheel);
    check (pixelsChanged (top, render (*matrix)) > 100, "a long track list scrolls");

    // A second MAIN explains that another instance holds the link.
    PreDropAudioProcessor otherMain;
    otherMain.setRole (Role::main);
    std::unique_ptr<juce::AudioProcessorEditor> otherEditor (otherMain.createEditor());
    if (auto* otherMatrix = findChild<TrackMatrix> (*otherEditor))
    {
        otherMatrix->setBounds (matrix->getBounds());
        click (*otherMatrix, otherMatrix->cell (1, Effect::reverb).getCentre());
        check (guitar.getLinkLevel (Effect::reverb) == 1.0f && hasDetail (render (*otherMatrix)),
               "a MAIN that isn't the link shows why and ignores clicks");
    }

    // A TRACK's face: the main's Amount on a read-only knob, its own switches.
    std::unique_ptr<juce::AudioProcessorEditor> trackEditor (guitar.createEditor());
    auto* panel = findChild<TrackPanel> (*trackEditor);
    const auto sliders = findChildren<juce::Slider> (*trackEditor);
    check (panel != nullptr && panel->isVisible() && ! sliders.empty(), "a TRACK shows its link panel");
    if (panel == nullptr || sliders.empty())
        return;

    pumpMessages (60);
    check (approx ((float) sliders[0]->getValue(), 0.7f, 1.0e-3f), "a TRACK's knob shows the main's Amount");
    check (panel->nameLabel().getText() == "Guitar", "a TRACK shows its name");

    panel->nameLabel().setText ("Lead Guitar", juce::sendNotificationSync);
    check (guitar.getTrackName() == "Lead Guitar", "renaming in the panel renames the track in the main's list");

    panel->levelKnob (Effect::reverb).setValue (0.3, juce::sendNotificationSync);
    check (approx (guitar.getLinkLevel (Effect::reverb), 0.3f, 1.0e-3f), "a TRACK's own REVERB knob sets its level");
    setParam (guitar, "trackDelay", 0.6f);
    check (pumpUntil ([&] { return std::abs (panel->levelKnob (Effect::delay).getValue() - 0.6) < 1.0e-3; }),
           "a level changed from the main moves the TRACK's knob");
    check (hasDetail (render (*trackEditor)), "a TRACK renders its face");

    // Changing the role (the pill opens a native menu, chosen here directly)
    // swaps the face over on the editor's next tick.
    master.setRole (Role::track);
    check (pumpUntil ([&] { return roleButton->getButtonText() == "TRACK"; })
             && findChild<TrackPanel> (*mainEditor)->isVisible() && ! tracksButton->isVisible(),
           "becoming a TRACK swaps to the track face");
    master.setRole (Role::returnTrack);
    check (pumpUntil ([&] { return roleButton->getButtonText() == "RETURN"; })
             && findChild<TrackPanel> (*mainEditor)->isVisible() && ! findChild<TrackPanel> (*mainEditor)->nameLabel().isVisible()
             && ! findChildren<juce::Slider> (*mainEditor)[1]->isEnabled(),
           "becoming a RETURN shows the sends panel, with Mix locked to wet");
    check (hasDetail (render (*mainEditor)), "a RETURN renders its face");
    master.setRole (Role::main);
    check (pumpUntil ([&] { return roleButton->getButtonText() == "MAIN"; })
             && tracksButton->isVisible() && findChildren<juce::Slider> (*mainEditor)[1]->isEnabled(),
           "and back to MAIN");
}

void testReturnRole()
{
    std::printf ("RETURN role:\n");
    using Role = PreDropAudioProcessor::Role;
    using Effect = LinkHub::Effect;

    // With no main yet, a RETURN is silent.
    PreDropAudioProcessor fx;
    fx.setRole (Role::returnTrack);
    fx.prepareToPlay (48000.0, 512);
    auto& hub = fx.getLinkHub();
    check (fx.followsMain() && hub.numReturns() == 1 && hub.getTracks().empty(),
           "a RETURN links as a return, not as a row in the track list");
    check (settledOutputRms (fx, 512, 40) == 0.0f, "with no main a return is silent");

    PreDropAudioProcessor master;
    check (master.isLinkMain() && hub.linkActive(), "a MAIN added later takes the link with the return on it");
    master.prepareToPlay (48000.0, 512);
    setParam (master, "amount", 1.0f);
    check (settledOutputRms (fx, 512, 60) > 0.01f, "linked, a return adds reverb and delay to what is sent");

    // Wet only: the send itself never comes out of the return, even with Mix down.
    setParam (fx, "mix", 0.0f);
    check (settledChange (fx, 512, 40) > 0.1f, "a return ignores Mix: it stays fully wet");
    fx.setLinkLevel (Effect::reverb, 0.0f);
    fx.setLinkLevel (Effect::delay, 0.0f);
    settledOutputRms (fx, 512, 10);   // let the ramps settle to off
    check (settledOutputRms (fx, 512, 20) == 0.0f, "with its reverb and delay at 0 a return is silent: no dry leaks through");

    juce::MemoryBlock state;
    fx.getStateInformation (state);
    PreDropAudioProcessor reloaded;
    reloaded.setStateInformation (state.getData(), (int) state.getSize());
    check (reloaded.getRole() == Role::returnTrack && hub.numReturns() == 2, "a reloaded RETURN relinks");

    fx.setRole (Role::main);
    check (hub.numReturns() == 1, "switching a RETURN to another role unlinks it");

    // The main's panel says how many returns are linked.
    TrackMatrix matrix (master);
    matrix.setBounds (0, 0, 308, 114);
    matrix.refresh();
    check (hasDetail (render (matrix)), "with only returns linked the panel explains the sends");
}

void testEmptyTrackList()
{
    std::printf ("Empty track list:\n");
    PreDropAudioProcessor master;
    TrackMatrix matrix (master);
    matrix.setBounds (0, 0, 308, 114);
    matrix.refresh();
    check (! master.getLinkHub().linkActive() && hasDetail (render (matrix)),
           "with nothing linked the panel says the whole mix is processed, and how to pick tracks");
}

// ---- Effect plug-ins: a VST3 as the reverb or delay ---------------------------
// PreDrop's own VST3 build stands in for a third-party effect: at its default
// Amount of 0 it passes audio through bit for bit, so as the "reverb" its wet
// output is exactly the signal, which makes the expected result exact.

// Largest |out - factor x in| over the last of `blocks` sine blocks.
float deviationFrom (PreDropAudioProcessor& p, float factor, int blocks)
{
    juce::MidiBuffer midi;
    float worst = 0.0f;
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> in (2, 512);
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < 512; ++n)
                in.setSample (ch, n, 0.5f * std::sin (0.05f * (float) (b * 512 + n)));
        auto out = in;
        p.processBlock (out, midi);
        if (b == blocks - 1)
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < 512; ++n)
                    worst = std::max (worst, std::abs (out.getSample (ch, n) - factor * in.getSample (ch, n)));
    }
    return worst;
}

void testEffectPlugins()
{
    std::printf ("Effect plug-ins:\n");
    using Slot = PreDropAudioProcessor::EffectSlot;

    const juce::File vst3 (PREDROP_TEST_VST3);
    check (vst3.exists(), "PreDrop's own VST3 (the stand-in plug-in) has been built");
    if (! vst3.exists())
        return;

    PreDropAudioProcessor p;
    p.prepareToPlay (48000.0, 512);
    setParam (p, "amount", 1.0f);
    for (auto* id : { "hpfDepth", "delayDepth", "riserDepth" })
        setParam (p, id, 0.0f);

    check (p.getEffectPluginName (Slot::reverb).isEmpty() && p.getEffectPluginName (Slot::delay).isEmpty(),
           "reverb and delay start as the built-in effects");

    const auto error = p.loadEffectPlugin (Slot::reverb, vst3);
    check (error.isEmpty() && p.getEffectPluginName (Slot::reverb) == "ZandersPreDrop",
           "a VST3 loads into the reverb slot (" + (error.isEmpty() ? juce::String ("ok") : error).toStdString() + ")");

    const float deviation = deviationFrom (p, 1.5f, 40);
    check (deviation < 1.0e-4f, "the plug-in comes in at the calibrated reverb level: dry + 0.5 x its output (off by "
                                  + std::to_string (deviation) + ")");
    check (p.effectPluginLeaksDry (Slot::reverb), "a plug-in passing its dry signal through is flagged (set it to 100% wet)");

    // Saved with the set, plug-in settings included.
    juce::MemoryBlock state;
    p.getStateInformation (state);
    PreDropAudioProcessor reopened;
    reopened.setStateInformation (state.getData(), (int) state.getSize());
    check (reopened.getEffectPluginName (Slot::reverb) == "ZandersPreDrop" && reopened.getEffectPluginName (Slot::delay).isEmpty(),
           "a reopened set has its plug-in back");

    // On a computer without the plug-in: the built-in plays, and the plug-in's
    // settings are kept, so saving there doesn't lose them.
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
    auto* saved = xml != nullptr ? xml->getChildByName ("EFFECT_PLUGINS") : nullptr;
    check (saved != nullptr, "the plug-in is saved in the state");
    if (saved == nullptr)
        return;
    auto* slotXml = saved->getChildByName ("SLOT");
    slotXml->setAttribute ("description", slotXml->getStringAttribute ("description")
                                              .replace (vst3.getFullPathName(), "/nowhere/NotInstalled.vst3"));
    juce::MemoryBlock elsewhereState;
    juce::AudioProcessor::copyXmlToBinary (*xml, elsewhereState);
    PreDropAudioProcessor elsewhere;
    elsewhere.setStateInformation (elsewhereState.getData(), (int) elsewhereState.getSize());
    check (elsewhere.getEffectPluginName (Slot::reverb).isEmpty() && elsewhere.getMissingEffectPluginName (Slot::reverb) == "ZandersPreDrop",
           "a plug-in that isn't installed falls back to the built-in, named as missing");
    juce::MemoryBlock resaved;
    elsewhere.getStateInformation (resaved);
    auto resavedXml = juce::AudioProcessor::getXmlFromBinary (resaved.getData(), (int) resaved.getSize());
    check (resavedXml != nullptr && resavedXml->getChildByName ("EFFECT_PLUGINS") != nullptr,
           "saving there keeps the missing plug-in and its settings");

    // The delay slot works the same way, and either goes back to built-in.
    check (p.loadEffectPlugin (Slot::delay, vst3).isEmpty() && p.getEffectPluginName (Slot::delay) == "ZandersPreDrop",
           "a VST3 loads into the delay slot too");
    p.useBuiltInEffect (Slot::reverb);
    p.useBuiltInEffect (Slot::delay);
    check (p.getEffectPluginName (Slot::reverb).isEmpty() && p.getEffectPluginName (Slot::delay).isEmpty(),
           "both slots go back to the built-in effects");

    const auto text = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("predrop-not-a-plugin.txt");
    text.replaceWithText ("not a plug-in");
    check (p.loadEffectPlugin (Slot::reverb, text).isNotEmpty() && p.getEffectPluginName (Slot::reverb).isEmpty(),
           "a file that isn't a plug-in is refused, leaving the built-in");
    check (p.loadEffectPlugin (Slot::reverb, juce::File ("/no/such/Plugin.vst3")).contains ("could not be found"),
           "a missing file is refused");
    text.deleteFile();

    // The EFFECTS panel's slot pills show what is loaded.
    p.loadEffectPlugin (Slot::reverb, vst3);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
    std::vector<EffectSlotButton*> pills;
    for (auto* child : editor->getChildren())
        if (auto* pill = dynamic_cast<EffectSlotButton*> (child))
            pills.push_back (pill);
    check (pills.size() == 2, "the EFFECTS panel has REV and DLY slot pills");
    if (pills.size() == 2)
    {
        pills[0]->refresh();
        pills[1]->refresh();
        check (pills[0]->getButtonText().startsWith ("REV: ") && pills[0]->getToggleState()
                 && pills[1]->getButtonText() == "DLY: BUILT-IN",
               "they show the loaded plug-in and the built-in one (" + pills[0]->getButtonText().toStdString() + ")");
    }
    check (juce::File::isAbsolutePath (vst3.getFullPathName()) && ! PreDropAudioProcessor::findInstalledVst3Plugins().contains (vst3),
           "PreDrop itself is left out of the plug-in list");

    if (pills.size() == 2)
    {
        // Picking from the pill's menu (acted on directly; the menu is a native window).
        pills[1]->loadPlugin (vst3, false);
        check (p.getEffectPluginName (Slot::delay) == "ZandersPreDrop" && pills[1]->getToggleState(),
               "loading from the DLY pill puts the plug-in in the delay slot");
        pills[1]->choose (EffectSlotButton::builtInId);
        check (p.getEffectPluginName (Slot::delay).isEmpty() && pills[1]->getButtonText() == "DLY: BUILT-IN",
               "choosing Built-in from the menu goes back to the built-in delay");
        pills[1]->choose (EffectSlotButton::firstPluginId + 9999);
        check (p.getEffectPluginName (Slot::delay).isEmpty(), "a stale menu choice does nothing");

        // The menu: Built-in ticked while nothing is loaded, plus a window item once one is.
        const auto itemIds = [] (juce::PopupMenu menu)
        {
            std::vector<std::pair<int, bool>> ids;   // id, ticked
            for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
                ids.emplace_back (it.getItem().itemID, it.getItem().isTicked);
            return ids;
        };
        const auto has = [] (const std::vector<std::pair<int, bool>>& ids, int id, bool ticked)
        {
            return std::find (ids.begin(), ids.end(), std::pair { id, ticked }) != ids.end();
        };
        const auto builtInMenu = itemIds (pills[1]->buildMenu());
        check (has (builtInMenu, EffectSlotButton::builtInId, true) && has (builtInMenu, EffectSlotButton::browseId, false)
                 && ! has (builtInMenu, EffectSlotButton::windowId, false),
               "the menu ticks Built-in and offers a file browser");
        const auto loadedMenu = itemIds (pills[0]->buildMenu());
        check (has (loadedMenu, EffectSlotButton::builtInId, false) && has (loadedMenu, EffectSlotButton::windowId, false),
               "with a plug-in loaded it offers the plug-in's window");

        // The warnings: a plug-in passing its dry signal, and one not installed here.
        deviationFrom (p, 1.5f, 40);   // the stand-in passes everything: it leaks dry
        pills[0]->refresh();
        check (pills[0]->getButtonText().contains ("100% WET"), "a dry-leaking plug-in's pill says to set it 100% wet");

        std::unique_ptr<juce::AudioProcessorEditor> elsewhereEditor (elsewhere.createEditor());
        for (auto* child : elsewhereEditor->getChildren())
            if (auto* pill = dynamic_cast<EffectSlotButton*> (child); pill != nullptr && pill->getButtonText().startsWith ("REV"))
            {
                pill->setBounds (0, 0, 400, 18);
                pill->refresh();
                check (pill->getButtonText().contains ("MISSING"), "a missing plug-in's pill says so");
            }
    }
}

// A host playhead reporting a fixed tempo, to see it reach a hosted plug-in.
struct FixedTempoPlayHead : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setTimeInSamples (0);   // hosts always report it; VST3 requires it
        info.setBpm (128.0);
        info.setIsPlaying (true);
        return info;
    }
};

void testEffectPluginHostIntegration()
{
    std::printf ("Effect plug-ins and the host:\n");
    using Slot = PreDropAudioProcessor::EffectSlot;
    const juce::File vst3 (PREDROP_TEST_VST3);
    if (! vst3.exists())
        return;

    // Tempo-synced effects need the host's tempo and transport: the playhead
    // PreDrop gets from the host is handed to its plug-ins as it processes.
    PreDropAudioProcessor p;
    p.prepareToPlay (48000.0, 512);
    p.loadEffectPlugin (Slot::delay, vst3);
    FixedTempoPlayHead hostPlayHead;
    p.setPlayHead (&hostPlayHead);
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    juce::MidiBuffer midi;
    p.processBlock (buffer, midi);
    auto* hosted = p.getEffectPluginInstance (Slot::delay);
    check (hosted != nullptr && hosted->getPlayHead() == &hostPlayHead,
           "a hosted plug-in gets the host's playhead (tempo, position, transport)");
    check (p.getEffectPluginInstance (Slot::reverb) == nullptr, "an empty slot has no plug-in instance");

    // Hosts may restore a project on a worker thread. Plug-ins and their
    // windows belong to the message thread, so the slot is restored there.
    juce::MemoryBlock state;
    p.getStateInformation (state);
    PreDropAudioProcessor restored;
    std::thread worker ([&] { restored.setStateInformation (state.getData(), (int) state.getSize()); });
    worker.join();
    check (restored.getEffectPluginName (Slot::delay).isEmpty(),
           "restoring on a worker thread doesn't create the plug-in there");
    check (pumpUntil ([&] { return restored.getEffectPluginName (Slot::delay) == "ZandersPreDrop"; }),
           "the message thread loads it right after");
}

void testHostedEffectEdges()
{
    std::printf ("Hosted effect edge cases:\n");
    const juce::File vst3 (PREDROP_TEST_VST3);
    if (! vst3.exists())
        return;

    juce::AudioPluginFormatManager formats;
    formats.addFormat (new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> types;
    formats.getFormat (0)->findAllTypesForFile (types, vst3.getFullPathName());
    juce::String error;
    auto instance = types.isEmpty() ? nullptr : formats.createPluginInstance (*types[0], 48000.0, 256, error);
    check (instance != nullptr, "the stand-in plug-in opens");
    if (instance == nullptr)
        return;

    HostedEffect fx (std::move (instance), *types[0]);
    fx.prepare (48000.0, 256);

    // A mono send: both of the plug-in's inputs get it, and the output comes back mono.
    std::vector<float> mono (256);
    for (size_t n = 0; n < mono.size(); ++n)
        mono[n] = 0.5f * std::sin (0.05f * (float) n);
    const auto original = mono;
    float* channel = mono.data();
    fx.process (&channel, 1, 256);
    check (mono == original, "a mono send comes back through a stereo plug-in");

    // A block longer than it was prepared for is silenced rather than overrun.
    std::vector<float> longer (512, 0.25f);
    float* longChannel = longer.data();
    fx.process (&longChannel, 1, 512);
    check (std::all_of (longer.begin(), longer.end(), [] (float v) { return v == 0.0f; }),
           "an oversized block is silenced, not overrun");

    // Silence in gives nothing to judge the dry leak by, so it doesn't move.
    std::vector<float> silence (256, 0.0f);
    float* silent = silence.data();
    const float leakBefore = fx.dryLeak();
    fx.process (&silent, 1, 256);
    check (juce::exactlyEqual (fx.dryLeak(), leakBefore), "silence leaves the dry-leak measure alone");

    HostedEffect none;
    check (none.isEmpty() && none.getPlugin() == nullptr, "an empty slot is the built-in marker");
}

void testUnboundIndicators()
{
    std::printf ("Indicators without a data source:\n");

    // Chips with no Amount / depth sources fall back to idle / the default depth.
    EffectChips chips;
    chips.setSize (308, 40);
    check (hasDetail (render (chips)), "chips render with no data source");

    // A curve pointed at a missing parameter draws idle and ignores the mouse.
    PreDropAudioProcessor p;
    BuildUpCurve curve (p.getState(), "noSuchParameter");
    curve.setSize (308, 126);
    check (hasDetail (render (curve)), "curve renders without a parameter");

    curve.mouseDown (mouseAt (curve, { 200.0f, 60.0f }, false));
    curve.mouseDrag (mouseAt (curve, { 250.0f, 60.0f }, true));
    curve.mouseUp   (mouseAt (curve, { 250.0f, 60.0f }, true));
    check (approx (param (p, "amount"), 0.0f, 1.0e-6f), "a curve without a parameter changes nothing");
}

void testButtonStates()
{
    std::printf ("EFFECTS button states:\n");
    PreDropLookAndFeel lnf;
    juce::TextButton button ("EFFECTS");
    button.setSize (84, 18);

    juce::Image image (juce::Image::ARGB, 84, 18, true, juce::SoftwareImageType());
    juce::Graphics g (image);

    // Every idle / hover / pressed / engaged combination.
    for (const bool on : { false, true })
    {
        button.setToggleState (on, juce::dontSendNotification);
        for (const bool highlighted : { false, true })
            for (const bool down : { false, true })
            {
                lnf.drawButtonBackground (g, button, {}, highlighted, down);
                lnf.drawButtonText (g, button, highlighted, down);
            }
    }
    check (image.getPixelAt (42, 9).getAlpha() > 0, "button draws in every state");

    check (PreDrop::Format::hertz (800.0f) == "800 Hz", "cutoff readout in Hz");
    check (PreDrop::Format::hertz (2500.0f) == "2.50 kHz", "cutoff readout switches to kHz");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    std::printf ("PreDrop plugin tests\n====================\n");

    testProcessorBasics();
    testBusLayouts();
    testProcessing();
    testStateRoundTrip();
    testEditor();
    testBuildUpCurveDrag();
    testRiserSampleLoading();
    testRiserSampleEditor();
    testLinkHub();
    testLinkedProcessing();
    testLinkingEditors();
    testReturnRole();
    testEffectPlugins();
    testHostedEffectEdges();
    testEffectPluginHostIntegration();
    testEmptyTrackList();
    testUnboundIndicators();
    testButtonStates();

    std::printf ("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
