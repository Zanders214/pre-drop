# Pre Drop

[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=tape-stop_pre-drop&metric=alert_status)](https://sonarcloud.io/summary/new_code?id=tape-stop_pre-drop)

A one-knob "build-up" effect plugin for DAWs (VST3 / AU / Standalone), built with
[JUCE](https://juce.com). Turn a steady loop into the tension before a drop with a
single macro: as **Amount** rises, the plugin progressively cuts the low end, swells
reverb, washes the signal out with delay, and lifts a filtered-noise riser underneath.

```
Amount  0% ............................................. 100%
HPF     |===================================================|  cut low end
Reverb         |=========================================|     swell space
Delay                  |=================================|     washed tail
Riser                          |=========================|     noise sweep
```

Each effect has its own activation window inside the macro range, so they stack in one
at a time instead of all arriving at once.

## Controls

| Knob        | What it does                                                        |
|-------------|---------------------------------------------------------------------|
| **Amount**  | The macro. Drives the whole build-up from 0% (clean) to 100% (peak). |
| **Mix**     | Global dry/wet so the build-up can sit under the source.             |
| **Output**  | Output trim (dB) to tame the energy that piles up near the top.      |

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

The DSP core (`PreDropEngine`) has a headless, framework-free test harness:

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
Tests/
  PreDropEngineTests.cpp        framework-free DSP tests + UI/DSP coupling check
  PreDropVisualModelTests.cpp   framework-free tests for the editor's numeric model
```

## UI

The editor recreates the **"Direction D — Neon Clean"** design natively in JUCE
(no WebView): a multicolour progress-ring **Amount** knob with a centred readout,
four effect status chips that light up as each effect engages (HPF 0% → Reverb
20% → Delay 40% → Riser 60%), a live build-up energy histogram you can also drag
to set Amount, and Mix / Output sliders. Indicator values are derived from the
macro through `PreDropVisualModel`, whose cutoff/threshold math is unit-tested
against `PreDropEngine::mapMacro` so the UI never drifts from the DSP. The face is
authored at 360 px and scales proportionally (resizable, fixed aspect ratio).

Typography targets Space Grotesk (UI) + JetBrains Mono (numbers), used when the
host has them and falling back to the platform sans / monospaced face otherwise —
bundling the TTFs as `BinaryData` is a localized swap in `PreDropTheme.h`.

## CI

GitHub Actions builds on Linux/macOS/Windows, runs the unit tests, and validates the
VST3 with [`pluginval`](https://github.com/Tracktion/pluginval) at strictness level 5.
