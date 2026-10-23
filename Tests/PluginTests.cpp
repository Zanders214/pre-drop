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

// Runs blocks through the processor's real audio callback and returns the RMS
// of the final block, by which time the 40 ms parameter ramps have settled.
float settledRms (PreDropAudioProcessor& p, int block, int blocks, bool silentInput)
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, block);

    for (int b = 0; b < blocks; ++b)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int n = 0; n < block; ++n)
                buffer.setSample (ch, n, silentInput ? 0.0f : 0.5f * std::sin (0.05f * (float) (b * block + n)));

        p.processBlock (buffer, midi);
    }

    double sum = 0.0;
    for (int n = 0; n < block; ++n)
        sum += (double) buffer.getSample (0, n) * buffer.getSample (0, n);
    return (float) std::sqrt (sum / block);
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
        check (approx (param (p, id), 1.0f, 1.0e-6f), std::string (id) + " defaults to full depth");

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

    // Prove the callback really runs the effect chain: with silent input, only
    // the engine's noise riser can make sound, and pulling Mix to 0 must return
    // the (silent) dry signal once the smoothers settle.
    const float riserRms = settledRms (p, block, 20, true);
    check (riserRms > 0.01f, "silent input at full Amount produces the riser through the processor");

    setParam (p, "mix", 0.0f);
    const float dryRms = settledRms (p, block, 20, true);
    check (dryRms < 1.0e-6f, "Mix at 0 passes the silent dry signal through untouched");
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

void testUnboundIndicators()
{
    std::printf ("Indicators without a data source:\n");

    // Chips with no Amount / HPF-depth source fall back to idle / full depth.
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
    testUnboundIndicators();
    testButtonStates();

    std::printf ("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
