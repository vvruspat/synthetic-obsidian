cmake_minimum_required(VERSION 3.22)

project(SyntheticObsidian VERSION 0.1.0)

# ── C++ standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build type default ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ── JUCE ─────────────────────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

option(SYNTHETIC_OBSIDIAN_BUILD_TOOLS "Build offline Synthetic Obsidian tools" ON)

# ── Embedded React frontend ──────────────────────────────────────────────────
# The checked-in dist bundle keeps C++ builds independent from Node.js. Rebuild
# it explicitly with `npm ci && npm run build` from frontend/ after UI changes.
set(SYNTHETIC_OBSIDIAN_FRONTEND_DIST "${CMAKE_SOURCE_DIR}/frontend/dist")
set(SYNTHETIC_OBSIDIAN_FRONTEND_ASSETS
    "${SYNTHETIC_OBSIDIAN_FRONTEND_DIST}/index.html"
    "${SYNTHETIC_OBSIDIAN_FRONTEND_DIST}/assets/app.js"
    "${SYNTHETIC_OBSIDIAN_FRONTEND_DIST}/assets/app.css"
    "${SYNTHETIC_OBSIDIAN_FRONTEND_DIST}/favicon.svg"
)

foreach(frontend_asset IN LISTS SYNTHETIC_OBSIDIAN_FRONTEND_ASSETS)
    if(NOT EXISTS "${frontend_asset}")
        message(FATAL_ERROR
            "Missing embedded frontend asset: ${frontend_asset}. "
            "Run `npm ci && npm run build` in ${CMAKE_SOURCE_DIR}/frontend.")
    endif()
endforeach()

juce_add_binary_data(SyntheticObsidianFrontendData
    HEADER_NAME SyntheticObsidianFrontendData.h
    NAMESPACE SyntheticObsidianFrontendData
    SOURCES ${SYNTHETIC_OBSIDIAN_FRONTEND_ASSETS}
)
if(APPLE)
    set_target_properties(SyntheticObsidianFrontendData PROPERTIES
        OSX_ARCHITECTURES "arm64;x86_64"
    )
endif()

# ── Plugin target ─────────────────────────────────────────────────────────────
juce_add_plugin(SyntheticObsidian
    COMPANY_NAME                "SyntheticObsidian"
    COMPANY_WEBSITE             "https://github.com/synthetic-obsidian"
    COMPANY_EMAIL               ""
    BUNDLE_ID                   "com.syntheticobsidian.plugin"

    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE

    EDITOR_WANTS_KEYBOARD_FOCUS FALSE

    # Plugin codes — 4 chars each, must be unique
    PLUGIN_MANUFACTURER_CODE    Sobd
    PLUGIN_CODE                 Syob

    FORMATS                     VST3 Standalone

    PRODUCT_NAME                "Synthetic Obsidian"
    VERSION                     "0.1.0"

    VST3_CATEGORIES             "Fx" "Analyzer"

    # Copy to system plugin folder after build (set to FALSE for CI)
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

# ── Generate JuceHeader.h ─────────────────────────────────────────────────────
juce_generate_juce_header(SyntheticObsidian)

# ── Source files ──────────────────────────────────────────────────────────────
target_sources(SyntheticObsidian
    PRIVATE
        # Core
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp

        # DSP
        Source/dsp/AudioEngine.cpp
        Source/dsp/PitchCorrectionModule.cpp
        Source/dsp/WORLDResynthesizer.cpp

        # UI
        Source/ui/SyntheticObsidianLookAndFeel.cpp
        Source/ui/TrackManagerPanel.cpp
        Source/ui/PianoRollEditor.cpp
        Source/ui/BottomParameterPanel.cpp
        Source/ui/TransportBar.cpp

        # AI / RVC bridge
        Source/ai/RVCPythonBridge.cpp
        Source/ai/SeedVCBridge.cpp

        # Analysis
        Source/analysis/AudioAnalysisEngine.cpp

        # State
        Source/state/ProjectState.cpp
        Source/state/TrackModel.cpp
)

# ── Include directories ───────────────────────────────────────────────────────
target_include_directories(SyntheticObsidian
    PRIVATE
        Source
)

# ── Compile definitions ───────────────────────────────────────────────────────
target_compile_definitions(SyntheticObsidian
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        # Silence deprecation warnings from JUCE internals on newer compilers
        JUCE_MODAL_LOOPS_PERMITTED=0
)

# ── Linked libraries ──────────────────────────────────────────────────────────
target_link_libraries(SyntheticObsidian
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ── Platform-specific settings ────────────────────────────────────────────────
if(APPLE)
    # Universal Binary (Apple Silicon + Intel)
    set_target_properties(SyntheticObsidian PROPERTIES
        OSX_ARCHITECTURES "arm64;x86_64"
    )
    target_compile_options(SyntheticObsidian PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter   # JUCE callbacks often have unused params
    )
elseif(WIN32)
    target_compile_options(SyntheticObsidian PRIVATE
        /W4
        /wd4100  # unreferenced formal parameter (same as above)
    )
    # Ensure Unicode
    target_compile_definitions(SyntheticObsidian PUBLIC
        UNICODE _UNICODE
    )
endif()

# ── Optional: tests ───────────────────────────────────────────────────────────
# Uncomment when adding unit tests
# add_subdirectory(Tests)

if(SYNTHETIC_OBSIDIAN_BUILD_TOOLS)
    add_subdirectory(tools/vocal_annotation_tool ${CMAKE_BINARY_DIR}/vocal_annotation_tool_build)
endif()
