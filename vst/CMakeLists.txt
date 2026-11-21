cmake_minimum_required(VERSION 3.22)

project(TheHamVST VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if HAM_JUCE_DIR is given, otherwise fetch it.
# (build.bat passes Brass Knuckles' or E-Piano's existing download when there is one.)
# ---------------------------------------------------------------------------
set(HAM_JUCE_DIR "" CACHE PATH "Optional path to a local JUCE checkout")
set(HAM_JUCE_TAG "8.0.17-security" CACHE STRING "JUCE git tag to fetch")

if(HAM_JUCE_DIR AND EXISTS "${HAM_JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${HAM_JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${HAM_JUCE_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
juce_add_plugin(TheHamVST
    COMPANY_NAME            "TheHam"
    PRODUCT_NAME            "The Ham"
    PLUGIN_MANUFACTURER_CODE Tham
    PLUGIN_CODE             Ham1
    FORMATS                 VST3 Standalone
    IS_SYNTH                TRUE
    NEEDS_MIDI_INPUT        TRUE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES         Instrument Synth
    NEEDS_WEBVIEW2          TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE)

# The UI (HTML/CSS/JS/SVG) and the factory presets are compiled into the binary.
file(GLOB_RECURSE HAM_UI_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.svg")
juce_add_binary_data(TheHamUI SOURCES ${HAM_UI_FILES} presets/factory_presets.json)

target_sources(TheHamVST PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PresetManager.cpp)

target_include_directories(TheHamVST PRIVATE Source)

target_compile_definitions(TheHamVST PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(TheHamVST
    PRIVATE
        TheHamUI
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Console tests + engine tools:  --target TheHamTests
# ---------------------------------------------------------------------------
juce_add_console_app(TheHamTests PRODUCT_NAME "TheHamTests")
target_sources(TheHamTests PRIVATE tests/PresetTests.cpp tests/OrganTests.cpp tests/VoiceTests.cpp tests/CabinetTests.cpp tests/EngineTools.cpp tests/HostCheck.cpp tests/Parity.cpp
    Source/PresetManager.cpp)
target_include_directories(TheHamTests PRIVATE Source)
# JUCE_PLUGINHOST_VST3: --host-check loads the built .vst3 like a DAW would
target_compile_definitions(TheHamTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_PLUGINHOST_VST3=1)
target_link_libraries(TheHamTests PRIVATE TheHamUI juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
