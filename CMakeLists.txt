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
    PRODUCT_NAME                "Pre Drop"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Filter
    AU_MAIN_TYPE                kAudioUnitType_Effect)

juce_generate_juce_header(PreDrop)

target_sources(PreDrop PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

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
    if(PREDROP_ENABLE_COVERAGE)
        target_compile_options(PreDropTests PRIVATE --coverage -O0 -g)
        target_link_options(PreDropTests PRIVATE --coverage)
    endif()

    add_test(NAME PreDropEngine COMMAND PreDropTests)
endif()
