# ZandersPreDrop

[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=tape-stop_pre-drop&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=tape-stop_pre-drop)

A one-knob "build-up" effect plugin for DAWs (VST3 / AU / Standalone), built with
[JUCE](https://juce.com). Turn a steady loop into the tension before a drop with a
single macro: as **Amount** rises, the plugin cuts the low end, swells reverb, washes
the signal out with delay, and lifts a filtered-noise riser underneath, all together.

```
Amount  0% ............................................. 100%
HPF     |===================================================|  cut low end
Reverb  |===================================================|  swell space
Delay   |===================================================|  washed tail
Riser   |===================================================|  noise sweep
```

Every effect starts with the first percent of Amount and reaches its full level at
100%, on the same curve, so they rise in step. Reverb, delay and riser are calibrated
to the same level relative to your signal (-6 dB at full Amount with the effect
knobs at their 50% default), and the riser follows your track's loudness, so the
balance holds on quiet and loud material alike.

The riser can be your own sample: open **EFFECTS** and click the pill under the
**RISER** knob (it says *NOISE*), or drop a WAV / AIFF / FLAC anywhere on the plugin.
It plays at the same calibrated level as the built-in noise. The file (up to 60 s)
is saved inside the plugin state, so your Ableton project keeps its riser even if
the original file moves.

### Riser timing

That pill opens the **RISER** view: your sample's waveform with its bars (numbered)
and beats drawn over it, a playhead while it plays, and its timing:

- **BPM** is the sample's own tempo, which places its bars and beats. Drag it up or
  down, scroll, or double-click to type. A first guess comes from the file name
  ("128 BPM", "8 Bars") or, failing that, its length.
- **Drag the waveform** to stretch it against the bars: the point you grab lands where
  you let go (snapping to beat lines; hold shift to place it freely), which sets the
  BPM. The loop keeps its bars, so dragging the end of the sound onto a bar line
  cuts the quiet tail after it.
- **BARS** (with MATCH PROJECT) is how many bars loop: past them is cut, short of
  them is silence. Drag the amber loop-end line on the waveform to set it there too.
  By default it's the whole bars nearest the sample's length.
- **Meter**: 2/4, 3/4, 4/4, 5/4, 6/8, 7/8 or 12/8.
- **MATCH PROJECT** (on for a new sample): the riser plays at your project's tempo.
  When a build-up starts it waits for the next bar line (or starts at once if you're
  within a sixteenth past one) and loops its BARS; anything past them is shaded
  and doesn't play.
  Off, it plays the whole sample at its own speed, looping with a short crossfade.
- **KEEP PITCH** (with MATCH PROJECT): on, the sample is time-stretched to the
  project's tempo and keeps its pitch (re-rendered in the background when the tempo
  changes, straight away when bouncing); off, it plays faster or slower, like tape.

The timing is saved with the sample. Sets saved before riser timing existed keep
playing their riser at its own speed until you turn MATCH PROJECT on.

## Shaping the built-in effects

Open **EFFECTS** and click the pill under a knob to open that effect's view (BACK
returns to EFFECTS):

- **REVERB**: **SIZE** (the room at full Amount; it grows towards it), **DAMPING**,
  **WIDTH** and **PRE-DELAY** (up to 250 ms before the tail starts).
- **DELAY**: **TIME**, a note value at the project's tempo (1/32 to 1/1, with
  triplets and dotted notes) or, with **SYNC** off, milliseconds; **FEEDBACK** (at
  full Amount; it grows towards it); **TONE**, a low-pass on the repeats (OPEN leaves
  them as they are); and **PING-PONG**, echoes bouncing left and right.
- **HIGH-PASS**: **24 dB SLOPE** (off: 12 dB per octave) and **RESONANCE**, a peak at
  the cutoff. Its pill in EFFECTS shows the slope.
- **RISER** (with the built-in noise): where its sweep **START**s and **END**s, and its
  **RESONANCE** (wide noise to a whistle).

The defaults are PreDrop's original sound. Every setting is a parameter, so it's
saved with your set and can be automated, and each one keeps its effect at the
calibrated level (changing the tone or the resonance doesn't make it jump out). Linked
TRACKs and RETURNs use the main's settings. While one of your VST3 plug-ins plays the
reverb or delay its view dims these (the plug-in's own window has its settings); its
source pill sits at the top of the view.

## Presets

The pill in the header (between the name and MAIN) holds the presets: pick one from
its menu to load it, or choose **Save preset...**, **Export...** or **Import...**. A
preset is the sound: every effect's settings and depth, Mix, Output, the riser sample
with its timing, and any VST3 reverb or delay with its own settings. It leaves Amount,
the role and the track levels as they are. Presets are `.predrop` files in
`~/Library/Audio/Presets/Zanders/PreDrop` on a Mac (the app-data folder's
`Zanders/PreDrop/Presets` elsewhere; *Show presets folder* opens it). Export writes one
anywhere, to share it; importing one loads it and keeps a copy in the folder. The
name of the preset in use is saved with your set.

## Choosing which tracks get which effects

Put PreDrop on your **master**: it's the **MAIN**, and on its own it works on the whole
mix. To give only some tracks the effects, **add PreDrop to those tracks**: with a
main already in the set, a new PreDrop joins as a **TRACK** by itself. From then on
the main keeps the Amount knob, the effect depths and the riser, and the linked tracks
apply the high-pass, reverb and delay, so the drums (with no PreDrop) stay dry.

In the main's **TRACKS** panel each linked track has an **HPF / REV / DLY** level (plus
an **ALL** row per column): its share of the main's depth for that effect, 100% by
default and "off" at 0%. Drag a level up or down, click to toggle off / 100%,
double-click to reset. So the guitar can get all the reverb, the piano 40% of it, and
everything follows one knob (or one automation lane). A track's levels are its own
parameters, so they save with the set and can be automated per track (they're also on
the TRACK's own face, along with its name: the host's, or double-click to type one).

**Without a PreDrop on every track: use a return.** Put PreDrop on a **return track**
and choose **RETURN** from its header pill. It adds reverb and delay, wet only, to
whatever the tracks send it, following the main at the same calibrated level; each
track's **send knob** in Ableton is its amount. (The high-pass can't work this way,
since it has to sit on the track itself, and the amounts live on the send knobs
rather than in PreDrop's list.) TRACKs and RETURNs can be mixed freely.

The header pill shows each PreDrop's role and opens a menu to change it: **MAIN**
(on your master), **TRACK** (on a track), **RETURN** (on a return track). A TRACK with
no main passes its audio through untouched, and a RETURN with no main is silent.
(Linking works because hosts like Ableton Live run every plugin instance in one
process.)

## Your own reverb and delay

Open **EFFECTS**: the pills under the **REVERB** and **DELAY** knobs choose what
PreDrop uses as its reverb and its delay: **Built-in** (default), any **VST3 effect**
from your plug-in folders, or one you browse to. Pick one and its window opens so you
can shape its sound (reopen it from the pill's menu). Set it to **100% wet**: PreDrop
feeds it the high-passed signal and brings its output in at the same calibrated level
as the built-in effect, so Amount, the depth knobs and per-track levels all keep
working (the pill turns amber and says *100% WET?* if it hears the dry signal
coming through). The plug-in and its settings are saved with your set; on a computer
where it isn't installed, the built-in effect plays and the pill says *MISSING*, and
its settings are kept for when it is. TRACKs and RETURNs have the same pills, under
their REVERB and DELAY levels.

With the built-in effects, Amount also grows the room and the delay feedback (see
Shaping the built-in effects above); with your own plug-in it sets the level, while
the plug-in's own knobs set its sound. A
plug-in that reports latency adds that much pre-delay to its effect (reverbs and
delays rarely have any).

## Controls

| Knob        | What it does                                                        |
|-------------|---------------------------------------------------------------------|
| **Amount**  | The macro. Drives the whole build-up from 0% (clean) to 100% (peak). |
| **Mix**     | Global dry/wet so the build-up can sit under the source.             |
| **Output**  | Output trim (dB) to tame the energy that piles up near the top.      |
| **Presets** | The header pill beside the name: the preset in use, and a menu to load, save, export and import presets (see Presets above). |
| **MAIN / TRACK / RETURN** | The header pill: shows this PreDrop's role and opens a menu to change it (see above). |
| **Tracks**  | On the main: what is linked, and each linked track's HPF / REV / DLY level. |
| **Sources** | The pills under the knobs in EFFECTS: what each effect uses (reverb and delay: *BUILT-IN* or one of your VST3 effects; riser: *NOISE* or your own sample, which you can also drop anywhere on the plugin; high-pass: its slope). Each opens that effect's view. On TRACK / RETURN the reverb and delay pills choose their source directly. |
| **Effect views** | REVERB, DELAY, HIGH-PASS: the source and the built-in effect's settings (see Shaping the built-in effects above). BACK returns to EFFECTS. |
| **Riser view** | The sample with its bars and beats (drag it to stretch it, drag its loop end to set the bars), its source (*Load riser sample...* / *Use built-in noise*), BPM, BARS, meter, MATCH PROJECT and KEEP PITCH (see Riser timing above); or, with the noise, its START, END and RESONANCE. BACK returns to EFFECTS. |
| **Effects** | Per-effect depth for Reverb, Delay, Riser and High-Pass. 50% (default) is the balanced level, 0% switches that effect off, 100% doubles it (+6 dB; the high-pass goes from 800 Hz to 3.2 kHz). |

## Building

JUCE is pulled automatically via CMake `FetchContent` (pinned to `8.0.4`), as is
[Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)
(`1.4.0`, MIT licence), which time-stretches the riser sample to the project's tempo:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

In locked-down / offline environments, point at a local JUCE checkout instead:

```sh
cmake -B build -DJUCE_PATH=/path/to/JUCE
```

Built plugins land under `build/PreDrop_artefacts/`.

## Tests

The DSP core (`PreDropEngine`) has a headless, framework-free test harness, and
the processor and editor are tested headlessly too: the real UI is rendered
offscreen in both panel states and at 2x (on Linux this runs under `xvfb-run`):

```sh
ctest --test-dir build --output-on-failure
```

## Layout

```
Source/
  PluginProcessor.{h,cpp}    thin host wrapper + parameters (APVTS)
  ParamIDs.h                 the parameter IDs
  RiserSource.{h,cpp}        the riser sample: loading, saving with the set, rendering it for the engine
  RiserTiming.h              a riser sample's tempo, meter and how it follows the project
  TrackLink.{h,cpp}          this instance's role (MAIN / TRACK / RETURN) and its place in the link
  EffectPlugins.{h,cpp}      the VST3 reverb / delay slots: loading, saving, windows, audio hand-over
  EffectSettings.h           the built-in effects' settings (reverb size, delay time, slopes...) and their parameters
  PresetManager.{h,cpp}      presets: the sound as a .predrop file, saved, loaded, exported and imported
  PluginEditor.{h,cpp}       "Neon Clean" editor: knob + chips + curve + sliders
  PreDropLookAndFeel.{h,cpp} custom knob (gradient ring) and Mix/Output sliders
  EffectChips.h              HPF/REV/DLY/RIS status chips (read-only indicators)
  BuildUpCurve.h             44-bar energy histogram; second handle on Amount
  PreDropTheme.h             shared palette, fonts, colour + value formatting
  PreDropVisualModel.h       pure macro -> indicator math (mirrors mapMacro)
  PreDropEngine.h            host-agnostic DSP core (the macro mapping + effect chain)
  RiserSample.h              a user riser sample, resampled / loop-crossfaded / level-matched
  RtHandoff.h                lock-free hand-over of objects to the audio thread
  HostedEffect.h             a VST3 effect standing in for the reverb / delay, and its window
  EffectSlotButton.h         the reverb / delay source pill: built-in or a VST3, its menu and warnings
  LinkHub.h                  process-wide link between the MAIN and its TRACK / RETURN instances
  TrackMatrix.h              the main's TRACKS panel (applies-to switch, per-track levels)
  TrackPanel.h               what a TRACK / RETURN shows: link status, name, its effect levels
  RiserView.h                the RISER view: waveform with bars and beats, the riser's timing controls
  EffectView.h               the REVERB / DELAY / HIGH-PASS views: source and settings
  SettingKnob.h              a small knob for one effect setting, with its name and value
  PresetButton.h             the header's preset pill and its menu (save, export, import)
Tests/
  PreDropEngineTests.cpp        framework-free DSP tests + UI/DSP coupling check
  PreDropVisualModelTests.cpp   framework-free tests for the editor's numeric model
  PluginTests.cpp               processor + editor: params, state, audio, offscreen renders
```

## UI

The editor recreates the **"Direction D — Neon Clean"** design natively in JUCE
(no WebView): a multicolour progress-ring **Amount** knob with a centred readout,
four effect status chips (the HPF cutoff, and each other effect's Amount × depth), a
live build-up energy histogram you can also drag to set Amount, Mix / Output sliders,
the EFFECTS panel of depth knobs, each with its source pill beneath, a view per effect
for its settings (the RISER view also shows the sample's waveform and timing), and a
preset pill in the header. Indicator values are derived from Amount and
the depths through `PreDropVisualModel`, whose cutoff/engagement math is unit-tested
against `PreDropEngine::mapMacro` so the UI never drifts from the DSP. The face is
authored at 360 px and scales proportionally (resizable, fixed aspect ratio).

Typography targets Space Grotesk (UI) + JetBrains Mono (numbers), used when the
host has them and falling back to the platform sans / monospaced face otherwise —
bundling the TTFs as `BinaryData` is a localized swap in `PreDropTheme.h`.

## CI

GitHub Actions builds on Linux/macOS/Windows, runs the unit tests, and validates the
VST3 with [`pluginval`](https://github.com/Tracktion/pluginval) at strictness level 5.

When a pull request from `dev` is merged into `main`, the release workflow builds and
tests all three platforms and publishes a GitHub Release containing:

- a Windows x64 ZIP (VST3 and standalone app),
- a Linux x64 tarball (VST3 and standalone app), and
- a universal macOS installer package (Apple Silicon and Intel).

The macOS installer puts the VST3 in `/Library/Audio/Plug-Ins/VST3`, the AU in
`/Library/Audio/Plug-Ins/Components`, and the standalone app in `/Applications`.
After installation, restart Ableton Live so it scans the newly installed plug-in.
