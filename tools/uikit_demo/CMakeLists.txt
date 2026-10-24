add_library(ObsidianDemoViews STATIC Demo.cpp Gallery.cpp)
target_link_libraries(ObsidianDemoViews PUBLIC Obsidian::EditorUI
    PRIVATE juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

foreach(app IN ITEMS ObsidianUIKitDemo ObsidianGallery)
    juce_add_gui_app(${app}
        COMPANY_NAME "SyntheticObsidian"
        PRODUCT_NAME "${app}"
        BUNDLE_ID "com.syntheticobsidian.${app}"
        VERSION "1.0.0"
        MICROPHONE_PERMISSION_ENABLED FALSE)
    target_sources(${app} PRIVATE Main.cpp)
    target_link_libraries(${app} PRIVATE ObsidianDemoViews)
endforeach()
target_compile_definitions(ObsidianGallery PRIVATE OBSIDIAN_GALLERY_APP=1)

if(BUILD_TESTING)
    juce_add_console_app(ObsidianUIKitTests PRODUCT_NAME "Obsidian UI checks")
    target_sources(ObsidianUIKitTests PRIVATE Tests.cpp GalleryTests.cpp
        ../../Source/uikit/ControlsTests.cpp ../../Source/editor_ui/SurfaceTests.cpp
        ../../Source/editor_ui/ChromeTests.cpp)
    target_link_libraries(ObsidianUIKitTests PRIVATE ObsidianDemoViews)
    add_test(NAME ObsidianUIKitTests COMMAND ObsidianUIKitTests)
endif()
