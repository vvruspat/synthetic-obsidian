juce_add_gui_app(VocalAnnotationTool
    COMPANY_NAME        "SyntheticObsidian"
    COMPANY_WEBSITE     "https://github.com/synthetic-obsidian"
    COMPANY_EMAIL       ""
    BUNDLE_ID           "com.syntheticobsidian.vocalannotationtool"
    PRODUCT_NAME        "Vocal Annotation Tool"
    VERSION             "0.1.0"
    NEEDS_WEB_BROWSER   TRUE
    NEEDS_WEBVIEW2      TRUE
)

juce_generate_juce_header(VocalAnnotationTool)

target_sources(VocalAnnotationTool
    PRIVATE
        Main.cpp
        AnnotationDocument.cpp
        AnnotationJson.cpp
        AnnotationValidator.cpp
        AnnotationEditorComponent.cpp
        ${CMAKE_SOURCE_DIR}/Source/web/SyntheticObsidianWebView.cpp
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
        SYNTHETIC_OBSIDIAN_ROOT="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(VocalAnnotationTool
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
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
    set_target_properties(VocalAnnotationTool PROPERTIES
        OSX_ARCHITECTURES "arm64;x86_64"
    )
    target_compile_options(VocalAnnotationTool PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
elseif(WIN32)
    target_compile_options(VocalAnnotationTool PRIVATE
        /W4
        /wd4100
    )
    target_compile_definitions(VocalAnnotationTool PUBLIC
        UNICODE _UNICODE
    )
endif()
