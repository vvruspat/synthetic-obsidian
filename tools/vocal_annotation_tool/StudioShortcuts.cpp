#include "StudioShortcuts.h"
#include "uikit/Controls.h"
#include "uikit/Theme.h"

namespace vocal_annotation
{
const std::vector<StudioShortcuts::Definition>& StudioShortcuts::definitions()
{
    static const auto result = []
    {
        const auto cmd = juce::ModifierKeys::commandModifier;
        const auto shiftCmd = cmd | juce::ModifierKeys::shiftModifier;
        const auto alt = juce::ModifierKeys::altModifier;
        using D = Definition;
        std::vector<D> values {
            { "app.settings", "Project", "Settings", juce::KeyPress(',', cmd, 0) },
            { "project.new", "Project", "New project", juce::KeyPress('N', cmd, 0) },
            { "project.open", "Project", "Open project", juce::KeyPress('O', cmd, 0) },
            { "project.save", "Project", "Save project", juce::KeyPress('S', cmd, 0) },
            { "project.undo", "Project", "Undo", juce::KeyPress('Z', cmd, 0) },
            { "project.redo", "Project", "Redo", juce::KeyPress('Z', shiftCmd, 0) },
            { "project.export", "Project", "Export all tracks", juce::KeyPress('E', cmd, 0) },
            { "transport.play-pause", "Transport", "Play / Pause", juce::KeyPress(juce::KeyPress::spaceKey) },
            { "transport.return-to-start", "Transport", "Return to start", juce::KeyPress(juce::KeyPress::returnKey) },
            { "transport.toggle-loop", "Transport", "Toggle cycle", juce::KeyPress('L') },
            { "editor.toggle-chord-snap", "Editor", "Toggle Chord Snap", juce::KeyPress('S') },
            { "editor.toggle-piano", "Editor", "Expand / collapse Piano Roll", juce::KeyPress('P') },
            { "view.horizontal-zoom-out", "Editor", "Horizontal zoom out", juce::KeyPress('-') },
            { "view.horizontal-zoom-in", "Editor", "Horizontal zoom in", juce::KeyPress('=') },
            { "view.vertical-zoom-out", "Editor", "Vertical zoom out", juce::KeyPress('-', alt, 0) },
            { "view.vertical-zoom-in", "Editor", "Vertical zoom in", juce::KeyPress('=', alt, 0) }
        };
        const juce::StringArray tools { "pointer", "pencil", "eraser", "scissors", "join", "flex", "vibrato", "gain", "zoom" };
        for (const auto secondary : { false, true })
            for (int index = 0; index < tools.size(); ++index)
                values.push_back({ "tool." + juce::String(secondary ? "right." : "left.") + tools[index],
                    secondary ? "Command-click tool" : "Click tool", tools[index],
                    juce::KeyPress('1' + index, secondary ? cmd : 0, 0) });
        return values;
    }();
    return result;
}
StudioShortcuts::StudioShortcuts(juce::File preferences) : file_(std::move(preferences))
{
    for (const auto& definition : definitions()) bindings_[definition.id] = definition.defaultKey;
    if (!file_.existsAsFile()) return;
    const auto saved = juce::JSON::parse(file_);
    for (const auto& definition : definitions())
        if (saved.hasProperty(definition.id))
        {
            const auto value = saved[juce::Identifier(definition.id)];
            if (!value.isString() && !value.isVoid() && !value.isUndefined()) continue;
            const auto text = value.toString();
            bindings_[definition.id] = text.isEmpty() ? juce::KeyPress() : juce::KeyPress::createFromDescription(text);
        }
}
juce::String StudioShortcuts::actionFor(const juce::KeyPress& key) const
{
    if (!key.isValid()) return {};
    for (const auto& definition : definitions()) if (getKey(definition.id).isValid() && getKey(definition.id) == key) return definition.id;
    return {};
}
juce::KeyPress StudioShortcuts::getKey(const juce::String& action) const
{
    const auto found = bindings_.find(action); return found == bindings_.end() ? juce::KeyPress() : found->second;
}
void StudioShortcuts::assign(const juce::String& action, const juce::KeyPress& key)
{
    if (!bindings_.contains(action)) return;
    if (key.isValid()) for (auto& [id, binding] : bindings_) if (id != action && binding == key) binding = {};
    bindings_[action] = key; save();
}
void StudioShortcuts::restoreDefaults()
{ for (const auto& definition : definitions()) bindings_[definition.id] = definition.defaultKey; save(); }
void StudioShortcuts::save()
{
    if (file_ == juce::File()) return;
    auto value = std::make_unique<juce::DynamicObject>();
    for (const auto& [id, key] : bindings_) value->setProperty(id, key.isValid() ? key.getTextDescription() : juce::String());
    if (file_.getParentDirectory().createDirectory().failed()
        || !file_.replaceWithText(juce::JSON::toString(juce::var(value.release())))) error_ = "Could not save key commands.";
    else error_.clear();
}

class StudioShortcutEditor::Content final : public juce::Component
{
    class Binding final : public obsidian::ui::Button
    {
    public:
        Binding(Content& owner, juce::String action) : owner_(owner), action_(std::move(action))
        {
            setComponentID("settings.shortcut." + action_); setTitle("Shortcut for " + action_);
            onClick = [this] { recording_ = true; setText("Press keys..."); };
            update();
        }
        void update() { const auto key = owner_.shortcuts_->getKey(action_); setText(key.isValid() ? key.getTextDescriptionWithIcons() : "Unassigned"); }
        bool keyPressed(const juce::KeyPress& key) override
        {
            if (!recording_) return Button::keyPressed(key);
            if (key.getKeyCode() == juce::KeyPress::escapeKey) { recording_ = false; update(); return true; }
            if (!key.isValid()) return true;
            recording_ = false;
            owner_.shortcuts_->assign(action_, key.getKeyCode() == juce::KeyPress::backspaceKey || key.getKeyCode() == juce::KeyPress::deleteKey ? juce::KeyPress() : key);
            owner_.refresh(); return true;
        }
        void focusLost(FocusChangeType cause) override { recording_ = false; update(); Button::focusLost(cause); }
    private:
        Content& owner_; juce::String action_; bool recording_ = false;
    };
public:
    explicit Content(std::shared_ptr<StudioShortcuts> shortcuts) : shortcuts_(std::move(shortcuts))
    {
        addAndMakeVisible(reset_); reset_.setComponentID("settings.shortcuts.reset");
        reset_.onClick = [this] { shortcuts_->restoreDefaults(); refresh(); };
        for (const auto& definition : StudioShortcuts::definitions())
        { auto button = std::make_unique<Binding>(*this, definition.id); addAndMakeVisible(*button); buttons_.push_back(std::move(button)); }
    }
    void refresh() { for (auto& button : buttons_) button->update(); repaint(); }
    void resized() override
    {
        reset_.setBounds(std::max(20, getWidth() - 210), 12, 185, 30);
        int y = 94; juce::String category;
        for (std::size_t index = 0; index < buttons_.size(); ++index)
        {
            const auto& definition = StudioShortcuts::definitions()[index];
            if (category != definition.category) { category = definition.category; y += 32; }
            buttons_[index]->setBounds(std::max(180, getWidth() / 2), y, std::max(140, getWidth() / 2 - 28), 30); y += 36;
        }
    }
    void paint(juce::Graphics& g) override
    {
        using namespace obsidian::ui;
        g.fillAll(colour(tokens::surface)); g.setFont(font()); g.setColour(colour(tokens::muted));
        g.drawText("Click a binding and press keys. Backspace clears it; duplicates move to the new action.", 20, 52, getWidth() - 40, 28, juce::Justification::left);
        g.drawText(shortcuts_->error(), 20, 80, getWidth() - 40, 22, juce::Justification::left);
        int y = 94; juce::String category;
        for (const auto& definition : StudioShortcuts::definitions())
        {
            if (category != definition.category)
            { category = definition.category; g.setColour(colour(tokens::text)); g.drawText(category, 20, y, getWidth() - 40, 30, juce::Justification::left); y += 32; }
            g.setColour(colour(tokens::muted)); g.drawText(definition.label, 20, y, std::max(150, getWidth() / 2 - 30), 30, juce::Justification::left); y += 36;
        }
    }
private:
    std::shared_ptr<StudioShortcuts> shortcuts_;
    obsidian::ui::Button reset_ { "Restore defaults" };
    std::vector<std::unique_ptr<Binding>> buttons_;
};
StudioShortcutEditor::StudioShortcutEditor(std::shared_ptr<StudioShortcuts> shortcuts)
    : content_(std::make_unique<Content>(std::move(shortcuts)))
{ addAndMakeVisible(viewport_); viewport_.setViewedComponent(content_.get(), false); viewport_.setScrollBarsShown(true, false); }
StudioShortcutEditor::~StudioShortcutEditor() { viewport_.setViewedComponent(nullptr, false); }
void StudioShortcutEditor::resized()
{ viewport_.setBounds(getLocalBounds()); content_->setSize(std::max(400, getWidth() - 16), 1510); }
void StudioShortcutEditor::paint(juce::Graphics& g) { g.fillAll(obsidian::ui::colour(obsidian::ui::tokens::surface)); }
}
