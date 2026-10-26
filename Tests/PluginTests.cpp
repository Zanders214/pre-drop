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

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
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

juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, bool dragged)
{
    const auto now = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), pos, {},
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &c, &c, now, pos, now, 1, dragged };
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

    check (p.getParameters().size() == 12, "twelve parameters are exposed");
    check (! p.getState().getParameter ("role")->isAutomatable() && ! p.getState().getParameter ("scope")->isAutomatable(),
           "the MAIN / TRACK role and the applies-to choice stay out of automation");
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
    check (label.startsWith ("RISER: PRED") && label.endsWith (juce::String::fromUTF8 ("\u2026")),
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

    hub.mainState().selectedTracksOnly.store (true);
    check (hub.tracksActive(), "tracks are active while the main applies to selected tracks");
    hub.releaseMain (&b);
    check (hub.isMain (&a), "only the main itself can release the slot");
    hub.releaseMain (&a);
    check (! hub.hasMain() && ! hub.tracksActive(), "releasing the main deactivates the tracks");
    check (hub.claimMain (&b), "the slot is free for the next main");
}

void testLinkedProcessing()
{
    std::printf ("Linked processing:\n");
    using Role = PreDropAudioProcessor::Role;
    using Effect = LinkHub::Effect;

    PreDropAudioProcessor master;
    check (master.getRole() == Role::main && master.isLinkMain(), "a new PreDrop is a MAIN and takes the link");

    PreDropAudioProcessor guitar, drums;
    guitar.setRole (Role::track);
    drums.setRole (Role::track);
    check (master.getLinkHub().getTracks().size() == 2, "tracks set to TRACK appear in the main's list");
    check (guitar.getTrackName() == "Track 1" && drums.getTrackName() == "Track 2", "unnamed tracks are numbered");

    drums.updateTrackProperties (hostTrack ("Drums"));
    guitar.updateTrackProperties (hostTrack ("Audio 3"));
    guitar.setTrackName ("Guitar");
    check (drums.getTrackName() == "Drums", "the host's track name is used");
    check (guitar.getTrackName() == "Guitar", "a name typed into PreDrop wins over the host's");

    for (auto e : { Effect::hpf, Effect::reverb, Effect::delay })
        drums.setLinkEffectOn (e, false);

    for (auto* p : { &master, &guitar, &drums })
        p->prepareToPlay (48000.0, 512);
    setParam (master, "amount", 1.0f);
    setParam (master, "riserDepth", 0.0f);   // keeps the main's own output comparable

    // ALL TRACKS: the main processes the mix; tracks stay out of it.
    check (! master.appliesToSelectedTracks(), "the main applies to all tracks by default");
    check (settledChange (guitar, 512, 20) == 0.0f, "on ALL TRACKS a linked track passes audio through untouched");
    check (settledChange (master, 512, 40) > 0.05f, "on ALL TRACKS the main processes the whole mix");

    // SELECTED TRACKS: tracks apply their effects; the main adds only the riser.
    master.setAppliesToSelectedTracks (true);
    check (master.getLinkHub().tracksActive(), "SELECTED TRACKS activates the linked tracks");
    check (settledChange (master, 512, 40) == 0.0f, "on SELECTED TRACKS the main leaves high-pass, reverb and delay to the tracks");
    check (settledChange (guitar, 512, 40) > 0.05f, "a track with its effects on follows the main's Amount");
    check (settledChange (drums, 512, 40) == 0.0f, "a track with every effect off is untouched");

    drums.setLinkEffectOn (Effect::reverb, true);
    check (settledChange (drums, 512, 40) > 0.01f, "switching one effect on for a track applies it");

    setParam (master, "amount", 0.0f);
    settledChange (guitar, 512, 10);                         // let the ramps settle
    check (settledChange (guitar, 512, 20) == 0.0f, "Amount at 0 on the main returns the tracks to dry");

    // Roles survive a save / reload, and so do a track's name and switches.
    juce::MemoryBlock state;
    drums.getStateInformation (state);
    PreDropAudioProcessor reloaded;
    reloaded.setStateInformation (state.getData(), (int) state.getSize());
    check (reloaded.getRole() == Role::track && reloaded.isLinkEffectOn (Effect::reverb)
             && ! reloaded.isLinkEffectOn (Effect::delay) && master.getLinkHub().getTracks().size() == 3,
           "a reloaded TRACK relinks with its switches");

    // A second MAIN works on its own until the first one steps down.
    PreDropAudioProcessor second;
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
    const auto allTracksView = render (*matrix);
    check (hasDetail (render (*mainEditor)), "on ALL TRACKS the panel explains it covers the whole mix");

    matrix->mouseDown (mouseAt (*matrix, matrix->scopeButton (true).getCentre(), false));
    check (master.appliesToSelectedTracks(), "clicking SELECTED TRACKS switches the main to selected tracks");
    matrix->refresh();
    check (pixelsChanged (allTracksView, render (*matrix)) > 500, "the panel then lists the linked tracks");

    matrix->mouseDown (mouseAt (*matrix, matrix->cell (1, Effect::reverb).getCentre(), false));
    check (! guitar.isLinkEffectOn (Effect::reverb) && keys.isLinkEffectOn (Effect::reverb),
           "clicking a track's REV switch turns reverb off for that track only");

    matrix->mouseDown (mouseAt (*matrix, matrix->cell (0, Effect::delay).getCentre(), false));
    check (! guitar.isLinkEffectOn (Effect::delay) && ! keys.isLinkEffectOn (Effect::delay), "ALL switches a column off for every track");
    matrix->mouseDown (mouseAt (*matrix, matrix->cell (0, Effect::delay).getCentre(), false));
    check (guitar.isLinkEffectOn (Effect::delay) && keys.isLinkEffectOn (Effect::delay), "and back on again");
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
    std::unique_ptr<juce::AudioProcessorEditor> otherEditor (otherMain.createEditor());
    if (auto* otherMatrix = findChild<TrackMatrix> (*otherEditor))
    {
        otherMatrix->mouseDown (mouseAt (*otherMatrix, otherMatrix->scopeButton (true).getCentre(), false));
        check (! otherMain.appliesToSelectedTracks() && hasDetail (render (*otherMatrix)),
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

    for (auto* b : findChildren<juce::TextButton> (*panel))
        if (b->getButtonText() == "HIGH-PASS")
            b->triggerClick();
    check (pumpUntil ([&] { return ! guitar.isLinkEffectOn (Effect::hpf); }), "a TRACK's own HIGH-PASS switch turns it off");
    check (hasDetail (render (*trackEditor)), "a TRACK renders its face");

    // Switching the main's role from its pill swaps its face over.
    roleButton->triggerClick();
    pumpMessages (60);
    check (master.getRole() == Role::track && roleButton->getButtonText() == "TRACK"
             && findChild<TrackPanel> (*mainEditor)->isVisible() && ! tracksButton->isVisible(),
           "the MAIN / TRACK pill switches the role and the face");
    roleButton->triggerClick();
    pumpMessages (60);
    check (master.getRole() == Role::main && tracksButton->isVisible(), "and back to MAIN");
}

void testEmptyTrackList()
{
    std::printf ("Empty track list:\n");
    PreDropAudioProcessor master;
    master.setAppliesToSelectedTracks (true);
    TrackMatrix matrix (master);
    matrix.setBounds (0, 0, 308, 114);
    matrix.refresh();
    check (hasDetail (render (matrix)), "with no linked tracks the panel says how to add one");
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
    testEmptyTrackList();
    testUnboundIndicators();
    testButtonStates();

    std::printf ("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
