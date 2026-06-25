cmake_minimum_required(VERSION 3.22)

project(PreDrop VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE acquisition
#
# Default: pull a pinned JUCE release via FetchContent (single source of truth,
# no submodules to keep in sync).
#
# Override: pass -DJUCE_PATH=/path/to/JUCE to use a local/vendored checkout
# instead. Useful in locked-down CI / containers where cloning from GitHub is
# blocked by network policy.
# ---------------------------------------------------------------------------
if(DEFINED JUCE_PATH)
    message(STATUS "PreDrop: using vendored JUCE at ${JUCE_PATH}")
    add_subdirectory(${JUCE_PATH} juce_build)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(PreDrop
    COMPANY_NAME                "ZandersAudio"
    BUNDLE_ID                   com.zandersaudio.predrop
    PLUGIN_MANUFACTURER_CODE    Zand
    PLUGIN_CODE                 Pdrp
    FORMATS                     VST3 AU Standalone
    PRODUCT_NAME                "ZandersPreDrop"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Filter
    AU_MAIN_TYPE                kAudioUnitType_Effect)

juce_generate_juce_header(PreDrop)

# Plugin translation units, shared with the headless console drivers (rt_check /
# perf_bench) so they exercise the exact same code the host runs. The editor
# sources are included because PluginProcessor::createEditor() references them at
# link time.
set(PREDROP_APP_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PreDropLookAndFeel.cpp)

target_sources(PreDrop PRIVATE ${PREDROP_APP_SOURCES})

target_compile_definitions(PreDrop
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(PreDrop
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Unit tests for the DSP core (PreDropEngine). Headless and framework-free,
# so they run on every platform without a display. Run with: ctest --test-dir build
# ---------------------------------------------------------------------------
option(PREDROP_BUILD_TESTS "Build PreDropEngine unit tests" ON)
option(PREDROP_ENABLE_COVERAGE "Instrument the unit tests for gcov/llvm coverage" OFF)

# Opt-in performance / real-time-safety targets. OFF by default so normal and
# release builds stay lean; CI flips them on for the dedicated jobs.
option(PREDROP_RT_SANITIZE "Build rt_check with clang RealtimeSanitizer (needs clang>=20)" OFF)
option(PREDROP_BUILD_BENCH "Build the perf_bench micro-benchmark (fetches nanobench)"        OFF)

if(PREDROP_BUILD_TESTS)
    enable_testing()

    add_executable(PreDropTests Tests/PreDropEngineTests.cpp)
    target_include_directories(PreDropTests PRIVATE Source)
    target_compile_definitions(PreDropTests PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(PreDropTests
        PRIVATE
            juce::juce_audio_basics
            juce::juce_dsp
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    # Coverage instrumentation for the DSP core, fed to SonarQube from CI. Only
    # the test target is instrumented, and -O0 keeps gcov's line mapping honest.
    if(PREDROP_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(PreDropTests PRIVATE --coverage -O0 -g)
        target_link_options(PreDropTests PRIVATE --coverage)
    endif()

    add_test(NAME PreDropEngine COMMAND PreDropTests)

    # The editor's numeric model is pure and framework-free, so it gets its own
    # tiny test target with no JUCE dependency at all.
    add_executable(PreDropVisualModelTests Tests/PreDropVisualModelTests.cpp)
    target_include_directories(PreDropVisualModelTests PRIVATE Source)

    if(PREDROP_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(PreDropVisualModelTests PRIVATE --coverage -O0 -g)
        target_link_options(PreDropVisualModelTests PRIVATE --coverage)
    endif()

    add_test(NAME PreDropVisualModel COMMAND PreDropVisualModelTests)
endif()

# ---------------------------------------------------------------------------
# Real-time-safety check (clang RealtimeSanitizer). A headless console app that
# runs the real audio callback under -fsanitize=realtime so any malloc / lock /
# syscall on the audio thread fails the build. Needs clang >= 20; CI runs it on
# Linux only. Defining PREDROP_RT_NONBLOCKING as the clang attribute here is what
# arms the [[clang::nonblocking]] annotation on PreDropEngine::process.
# ---------------------------------------------------------------------------
if(PREDROP_RT_SANITIZE)
    juce_add_console_app(rt_check PRODUCT_NAME "rt_check")
    juce_generate_juce_header(rt_check)
    set_target_properties(rt_check PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(rt_check PRIVATE Tests/rt_check.cpp ${PREDROP_APP_SOURCES})
    target_include_directories(rt_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_compile_definitions(rt_check PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="ZandersPreDrop"
        "PREDROP_RT_NONBLOCKING=[[clang::nonblocking]]")   # CMake passes brackets fine inside quotes
    target_compile_options(rt_check PRIVATE -fsanitize=realtime)
    target_link_options   (rt_check PRIVATE -fsanitize=realtime)
    target_link_libraries(rt_check PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()

# ---------------------------------------------------------------------------
# Performance micro-benchmark (nanobench). A headless console app that measures
# the per-block cost of the full effect chain and writes a github-action-benchmark
# JSON file for the perf workflow's trend dashboard.
# ---------------------------------------------------------------------------
if(PREDROP_BUILD_BENCH)
    include(FetchContent)
    FetchContent_Declare(nanobench
        GIT_REPOSITORY https://github.com/martinus/nanobench.git
        GIT_TAG        v4.3.11
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(nanobench)

    juce_add_console_app(perf_bench PRODUCT_NAME "perf_bench")
    juce_generate_juce_header(perf_bench)
    set_target_properties(perf_bench PROPERTIES EXCLUDE_FROM_ALL TRUE)
    target_sources(perf_bench PRIVATE Tests/perf_bench.cpp ${PREDROP_APP_SOURCES})
    target_include_directories(perf_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${nanobench_SOURCE_DIR}/src/include)   # we define ANKERL_NANOBENCH_IMPLEMENT ourselves
    target_compile_definitions(perf_bench PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="ZandersPreDrop")
    target_link_libraries(perf_bench PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()
