juce_add_gui_app(NativeNoteAnnotator
    COMPANY_NAME "SyntheticObsidian"
    BUNDLE_ID "com.syntheticobsidian.native-note-annotator"
    PRODUCT_NAME "Vocal Note Annotator"
    VERSION "1.0.0"
    NEEDS_WEB_BROWSER FALSE
    MICROPHONE_PERMISSION_ENABLED FALSE
)
juce_generate_juce_header(NativeNoteAnnotator)

set(SYNTHETIC_OBSIDIAN_NOTE_MODEL "" CACHE FILEPATH "Optional native TCN model.json to bundle in the annotation app")
if(SYNTHETIC_OBSIDIAN_NOTE_MODEL)
    if(NOT EXISTS "${SYNTHETIC_OBSIDIAN_NOTE_MODEL}")
        message(FATAL_ERROR "Native note model does not exist: ${SYNTHETIC_OBSIDIAN_NOTE_MODEL}")
    endif()
    if(APPLE)
        target_sources(NativeNoteAnnotator PRIVATE "${SYNTHETIC_OBSIDIAN_NOTE_MODEL}")
        set_source_files_properties("${SYNTHETIC_OBSIDIAN_NOTE_MODEL}" PROPERTIES
            HEADER_FILE_ONLY TRUE MACOSX_PACKAGE_LOCATION "Resources/note-model")
    else()
        add_custom_command(TARGET NativeNoteAnnotator POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NativeNoteAnnotator>/models/note-boundaries"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SYNTHETIC_OBSIDIAN_NOTE_MODEL}"
                "$<TARGET_FILE_DIR:NativeNoteAnnotator>/models/note-boundaries/model.json"
            VERBATIM)
    endif()
endif()

set(annotation_sources
    NoteAnalysis.cpp
    NoteBoundaryModel.cpp
    PitchStructureFeatures.cpp
    NoteEditing.cpp
    DatasetExport.cpp
    ../vocal_annotation_tool/AnnotationDocument.cpp
    ../vocal_annotation_tool/AnnotationJson.cpp
    ../vocal_annotation_tool/ArrangementModel.cpp
    ../vocal_annotation_tool/AnnotationValidator.cpp
)
target_sources(NativeNoteAnnotator PRIVATE Main.cpp WaveformEditor.cpp ${annotation_sources})
target_include_directories(NativeNoteAnnotator PRIVATE . ../vocal_annotation_tool)
target_compile_definitions(NativeNoteAnnotator PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(NativeNoteAnnotator PRIVATE
    juce::juce_audio_utils juce::juce_gui_extra juce::juce_cryptography juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
if(APPLE)
    # This tool has no MLX/Python runtime and can support both macOS architectures.
    set_target_properties(NativeNoteAnnotator PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
    add_custom_command(TARGET NativeNoteAnnotator POST_BUILD
        COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:NativeNoteAnnotator>"
        COMMENT "Ad-hoc signing the local native annotation app"
        VERBATIM)
endif()

option(SYNTHETIC_OBSIDIAN_ANNOTATOR_TESTS "Build native annotation checks" OFF)
if(SYNTHETIC_OBSIDIAN_ANNOTATOR_TESTS)
    enable_testing()
    juce_add_console_app(NativeNoteAnnotatorTests PRODUCT_NAME "Native annotation checks")
    juce_generate_juce_header(NativeNoteAnnotatorTests)
    target_sources(NativeNoteAnnotatorTests PRIVATE Tests.cpp ${annotation_sources})
    target_include_directories(NativeNoteAnnotatorTests PRIVATE . ../vocal_annotation_tool)
    target_compile_definitions(NativeNoteAnnotatorTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(NativeNoteAnnotatorTests PRIVATE
        juce::juce_audio_utils juce::juce_gui_extra juce::juce_cryptography juce::juce_dsp
        juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(APPLE)
        set_target_properties(NativeNoteAnnotatorTests PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
    endif()
    add_test(NAME NativeNoteAnnotatorTests COMMAND NativeNoteAnnotatorTests)
endif()
