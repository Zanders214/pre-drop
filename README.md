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

The riser can be your own sample: it loops (with a short crossfade so the loop point
doesn't click), restarts from the top at the start of each build-up, and plays at
the same calibrated level as the built-in noise. The file (up to 60 s) is saved
inside the plugin state, so your Ableton project keeps its riser even if the
original file moves.

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

## Controls

| Knob        | What it does                                                        |
|-------------|---------------------------------------------------------------------|
| **Amount**  | The macro. Drives the whole build-up from 0% (clean) to 100% (peak). |
| **Mix**     | Global dry/wet so the build-up can sit under the source.             |
| **Output**  | Output trim (dB) to tame the energy that piles up near the top.      |
| **MAIN / TRACK / RETURN** | The header pill: shows this PreDrop's role and opens a menu to change it (see above). |
| **Tracks**  | On the main: what is linked, and each linked track's HPF / REV / DLY level. |
| **Riser**   | The riser sound. Built-in noise by default; click **RISER** (or drop a WAV / AIFF / FLAC anywhere on the plugin) to use your own sample instead, and pick *Use built-in noise* to go back. |
| **Effects** | Per-effect depth for Reverb, Delay, Riser and High-Pass. 50% (default) is the balanced level, 0% switches that effect off, 100% doubles it (+6 dB; the high-pass goes from 800 Hz to 3.2 kHz). |

## Building

JUCE is pulled automatically via CMake `FetchContent` (pinned to `8.0.4`):

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
  PluginEditor.{h,cpp}       "Neon Clean" editor: knob + chips + curve + sliders
  PreDropLookAndFeel.{h,cpp} custom knob (gradient ring) and Mix/Output sliders
  EffectChips.h              HPF/REV/DLY/RIS status chips (read-only indicators)
  BuildUpCurve.h             44-bar energy histogram; second handle on Amount
  PreDropTheme.h             shared palette, fonts, colour + value formatting
  PreDropVisualModel.h       pure macro -> indicator math (mirrors mapMacro)
  PreDropEngine.h            host-agnostic DSP core (the macro mapping + effect chain)
  RiserSample.h              a user riser sample, resampled / loop-crossfaded / level-matched
  LinkHub.h                  process-wide link between the MAIN and its TRACK / RETURN instances
  TrackMatrix.h              the main's TRACKS panel (applies-to switch, per-track levels)
  TrackPanel.h               what a TRACK / RETURN shows: link status, name, its effect levels
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
and the EFFECTS panel of depth knobs. Indicator values are derived from Amount and
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
