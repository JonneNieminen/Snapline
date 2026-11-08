cmake_minimum_required(VERSION 3.22)
project(Snapline VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_DIR "${CMAKE_CURRENT_LIST_DIR}/third_party/JUCE" CACHE PATH "Path to a JUCE checkout (JUCE 9.0.5 or newer)")
if (NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE was not found at ${JUCE_DIR}. Clone JUCE 9.0.5 into third_party/JUCE or set -DJUCE_DIR=/path/to/JUCE")
endif()
add_subdirectory("${JUCE_DIR}" JUCE)

juce_add_plugin(Snapline
    COMPANY_NAME "Snapline Audio"
    BUNDLE_ID "audio.snapline.pitchcorrection"
    PLUGIN_MANUFACTURER_CODE SNPL
    PLUGIN_CODE SNL1
    FORMATS AU VST3
    PRODUCT_NAME "Snapline"
    DESCRIPTION "Real-time vocal pitch correction"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicEffect
    VST3_CATEGORIES Fx "Pitch Shift"
)

juce_generate_juce_header(Snapline)

set(SNAPLINE_DSP_SOURCES
    Source/PitchDetector.cpp
    Source/PitchDetector.h
    Source/SnaplineDsp.cpp
    Source/SnaplineDsp.h
    Source/Scales.h
)

target_sources(Snapline PRIVATE
    ${SNAPLINE_DSP_SOURCES}
    Source/Presets.h
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PitchGraph.cpp
    Source/PitchGraph.h
    Source/DesignTokens.h
)

target_compile_definitions(Snapline PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(Snapline PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Offline checks: renders a synthetic vocal through the engine at several sample rates and
# block sizes, measures the corrected pitch, transparency, latency and stability.
# Configure with -DSNAPLINE_BUILD_TOOLS=ON.
option(SNAPLINE_BUILD_TOOLS "Build developer tools (snapline-render)" OFF)
if (SNAPLINE_BUILD_TOOLS)
    juce_add_console_app(SnaplineRender PRODUCT_NAME "snapline-render")
    target_sources(SnaplineRender PRIVATE tools/render/Main.cpp ${SNAPLINE_DSP_SOURCES})
    target_include_directories(SnaplineRender PRIVATE Source)
    target_compile_definitions(SnaplineRender PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(SnaplineRender PRIVATE
        juce::juce_dsp juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Renders the real editor (dark and light) to PNG for checks and README screenshots.
    juce_add_console_app(SnaplineSnapshot PRODUCT_NAME "snapline-snapshot")
    juce_generate_juce_header(SnaplineSnapshot)
    target_sources(SnaplineSnapshot PRIVATE tools/snapshot/Main.cpp ${SNAPLINE_DSP_SOURCES}
        Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/PitchGraph.cpp)
    target_include_directories(SnaplineSnapshot PRIVATE Source)
    target_compile_definitions(SnaplineSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="Snapline" JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(SnaplineSnapshot PRIVATE
        juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
