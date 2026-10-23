juce_add_gui_app(VocalAnnotationTool
    COMPANY_NAME        "SyntheticObsidian"
    COMPANY_WEBSITE     "https://github.com/synthetic-obsidian"
    COMPANY_EMAIL       ""
    BUNDLE_ID           "com.syntheticobsidian.vocalannotationtool"
    PRODUCT_NAME        "Synthetic Obsidian"
    VERSION             "${PROJECT_VERSION}"
    ICON_BIG            "${CMAKE_SOURCE_DIR}/distribution/macos-arm64/AppIcon.png"
    ICON_SMALL          "${CMAKE_SOURCE_DIR}/distribution/macos-arm64/AppIcon.png"
    NEEDS_WEB_BROWSER   TRUE
    NEEDS_WEBVIEW2      TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Synthetic Obsidian uses the selected audio input for vocal recording and monitoring."
)

juce_generate_juce_header(VocalAnnotationTool)

target_sources(VocalAnnotationTool
    PRIVATE
        Main.cpp
        AnnotationDocument.cpp
        ArrangementModel.cpp
        ArrangementPlayback.cpp
        ClipPitchAnalysis.cpp
        AnnotationJson.cpp
        AnnotationValidator.cpp
        AnnotationEditorComponent.cpp
        AlgorithmicPitchPreview.cpp
        RuntimeManager.cpp
        SashaNoteAnalysis.cpp
        SibilantModel.cpp
        ../native_note_annotator/NoteAnalysis.cpp
        ../native_note_annotator/NoteBoundaryModel.cpp
        ../native_note_annotator/PitchStructureFeatures.cpp
        ${CMAKE_SOURCE_DIR}/Source/web/SyntheticObsidianWebView.cpp
        ${CMAKE_SOURCE_DIR}/Source/web/AudioFileTransfer.cpp
)

target_include_directories(VocalAnnotationTool
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/Source
)

target_compile_definitions(VocalAnnotationTool
    PUBLIC
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=0
        JUCE_USE_WIN_WEBVIEW2=1
        $<$<NOT:$<CONFIG:Release>>:SYNTHETIC_OBSIDIAN_DEV_ROOT="${CMAKE_SOURCE_DIR}">
)

target_link_libraries(VocalAnnotationTool
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_cryptography
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        SyntheticObsidianFrontendData
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if(APPLE)
    target_sources(VocalAnnotationTool PRIVATE
        MacApplicationIcon.h
        MacApplicationIcon.mm
    )
    set(
        SYNTHETIC_OBSIDIAN_MACOS_CODESIGN_IDENTITY
        ""
        CACHE STRING
        "Optional signing identity for stable macOS privacy permissions during local builds"
    )
    set_target_properties(VocalAnnotationTool PROPERTIES
        OSX_ARCHITECTURES "arm64"
    )
    target_compile_options(VocalAnnotationTool PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
    if(SYNTHETIC_OBSIDIAN_MACOS_CODESIGN_IDENTITY)
        add_custom_command(
            TARGET VocalAnnotationTool
            POST_BUILD
            COMMAND
                /usr/bin/codesign
                --force
                --deep
                --sign "${SYNTHETIC_OBSIDIAN_MACOS_CODESIGN_IDENTITY}"
                "$<TARGET_BUNDLE_DIR:VocalAnnotationTool>"
            COMMENT "Signing VocalAnnotationTool for stable macOS privacy permissions"
            VERBATIM
        )
    endif()
elseif(WIN32)
    target_compile_options(VocalAnnotationTool PRIVATE
        /W4
        /wd4100
    )
    target_compile_definitions(VocalAnnotationTool PUBLIC
        UNICODE _UNICODE
    )
endif()

option(SYNTHETIC_OBSIDIAN_VOCAL_ANALYSIS_TESTS "Build Sasha AI Parts integration checks" OFF)
if(SYNTHETIC_OBSIDIAN_VOCAL_ANALYSIS_TESTS)
    enable_testing()
    juce_add_console_app(SashaNoteAnalysisTests PRODUCT_NAME "Sasha AI Parts checks")
    juce_generate_juce_header(SashaNoteAnalysisTests)
    target_sources(SashaNoteAnalysisTests PRIVATE
        SashaNoteAnalysisTests.cpp SashaNoteAnalysis.cpp SibilantModel.cpp RuntimeManager.cpp
        AnnotationDocument.cpp AnnotationJson.cpp ArrangementModel.cpp
        ../native_note_annotator/NoteAnalysis.cpp
        ../native_note_annotator/NoteBoundaryModel.cpp
        ../native_note_annotator/PitchStructureFeatures.cpp)
    target_include_directories(SashaNoteAnalysisTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(SashaNoteAnalysisTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(SashaNoteAnalysisTests PRIVATE
        juce::juce_audio_utils juce::juce_gui_extra juce::juce_cryptography juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME SashaNoteAnalysisTests COMMAND SashaNoteAnalysisTests)
endif()

set(SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL "" CACHE FILEPATH "Optional native Sasha sibilant model")
if(SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL)
    if(NOT EXISTS "${SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL}")
        message(FATAL_ERROR "Sibilant model does not exist: ${SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL}")
    endif()
    if(APPLE)
        target_sources(VocalAnnotationTool PRIVATE "${SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL}")
        set_source_files_properties("${SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL}" PROPERTIES
            HEADER_FILE_ONLY TRUE MACOSX_PACKAGE_LOCATION "Resources/sibilant-model")
    else()
        add_custom_command(TARGET VocalAnnotationTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VocalAnnotationTool>/models/sibilants/sasha-v1-experimental"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SYNTHETIC_OBSIDIAN_APP_SIBILANT_MODEL}"
                "$<TARGET_FILE_DIR:VocalAnnotationTool>/models/sibilants/sasha-v1-experimental/native-model.json"
            VERBATIM)
    endif()
endif()

set(SYNTHETIC_OBSIDIAN_APP_NOTE_MODEL "" CACHE FILEPATH "Optional Sasha note-boundary model to bundle in Synthetic Obsidian")
if(SYNTHETIC_OBSIDIAN_APP_NOTE_MODEL)
    if(NOT EXISTS "${SYNTHETIC_OBSIDIAN_APP_NOTE_MODEL}")
        message(FATAL_ERROR "Note boundary model does not exist: ${SYNTHETIC_OBSIDIAN_APP_NOTE_MODEL}")
    endif()
    if(APPLE)
        target_sources(VocalAnnotationTool PRIVATE "${SYNTHETIC_OBSIDIAN_APP_NOTE_MODEL}")
        set_source_files_properties("${SYNTHETIC_OBSIDIAN_APP_NOTE_MODEL}" PROPERTIES
            HEADER_FILE_ONLY TRUE MACOSX_PACKAGE_LOCATION "Resources/note-model")
    else()
        add_custom_command(TARGET VocalAnnotationTool POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:VocalAnnotationTool>/models/note-boundaries/sasha-v11-melismas"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SYNTHETIC_OBSIDIAN_APP_NOTE_MODEL}"
                "$<TARGET_FILE_DIR:VocalAnnotationTool>/models/note-boundaries/sasha-v11-melismas/model.json"
            VERBATIM)
    endif()
endif()

option(SYNTHETIC_OBSIDIAN_ARRANGEMENT_TESTS "Build arrangement model and playback tests" OFF)
if(SYNTHETIC_OBSIDIAN_ARRANGEMENT_TESTS)
    enable_testing()
    juce_add_console_app(ArrangementTests PRODUCT_NAME "Arrangement tests")
    juce_generate_juce_header(ArrangementTests)
    target_sources(ArrangementTests PRIVATE ArrangementTests.cpp ArrangementModel.cpp ArrangementPlayback.cpp
        AnnotationDocument.cpp AnnotationJson.cpp ${CMAKE_SOURCE_DIR}/Source/web/AudioFileTransfer.cpp)
    target_compile_definitions(ArrangementTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ArrangementTests PRIVATE juce::juce_audio_utils juce::juce_gui_extra
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    add_test(NAME ArrangementTests COMMAND ArrangementTests)
endif()
