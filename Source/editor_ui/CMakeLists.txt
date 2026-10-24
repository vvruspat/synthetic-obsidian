add_library(ObsidianEditorUI STATIC Timeline.cpp PitchEditor.cpp Chrome.cpp)
add_library(Obsidian::EditorUI ALIAS ObsidianEditorUI)
target_link_libraries(ObsidianEditorUI PUBLIC Obsidian::UIKit
    PRIVATE juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
