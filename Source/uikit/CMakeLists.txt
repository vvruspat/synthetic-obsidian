add_library(ObsidianUIKit STATIC Theme.cpp Controls.cpp Menu.cpp Layout.cpp)
add_library(Obsidian::UIKit ALIAS ObsidianUIKit)
target_include_directories(ObsidianUIKit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(ObsidianUIKit PUBLIC cxx_std_20)
target_compile_definitions(ObsidianUIKit PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(ObsidianUIKit PUBLIC juce::juce_gui_basics
    PRIVATE juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
