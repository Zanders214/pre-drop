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

    check (p.getParameters().size() == 7, "seven parameters are exposed");
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
    check (riserButton->getButtonText() == "RISER: PREDROP-EDITOR-" + juce::String::fromUTF8 ("\u2026"),
           "the button shows the sample's name, shortened to fit");
    check (riserButton->getToggleState(), "the button lights up while a sample is in use");

    // The button opens its menu (load / back to noise); dismiss it again.
    riserButton->triggerClick();
    pumpMessages (60);
    juce::PopupMenu::dismissAllActiveMenus();
    pumpMessages (30);

    p.clearRiserSample();
    pumpMessages (60);   // the editor's timer picks up the change
    check (riserButton->getButtonText() == "RISER: NOISE" && ! riserButton->getToggleState(),
           "going back to noise resets the button");

    riserFile.deleteFile();
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
    testUnboundIndicators();
    testButtonStates();

    std::printf ("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
