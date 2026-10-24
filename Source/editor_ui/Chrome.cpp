#include "Chrome.h"

#include <cmath>

namespace obsidian::editor
{
namespace
{
constexpr int kHeaderPadding = 12;
constexpr int kTransportHeight = 38;
constexpr int kTrackControlHeight = 24;

void text(juce::Graphics& g, const juce::String& value, juce::Rectangle<int> bounds,
          float size = ui::tokens::fontBody, juce::Colour colour = ui::colour(ui::tokens::text),
          juce::Justification alignment = juce::Justification::centredLeft)
{
    g.setColour(colour);
    g.setFont(ui::font(size));
    g.drawText(value, bounds, alignment, true);
}
}

TransportBar::TransportBar()
{
    setTitle("Transport");
    play_.setAccent(ui::colour(ui::tokens::olive));
    play_.setIconColour(ui::colour(ui::tokens::olive));
    record_.setAccent(ui::colour(ui::tokens::pink));
    record_.setIconColour(ui::colour(ui::tokens::pink));
    project_.setComponentID("transport.project");
    rewind_.setComponentID("transport.rewind");
    play_.setComponentID("transport.play");
    stop_.setComponentID("transport.stop");
    record_.setComponentID("transport.record");
    loop_.setComponentID("transport.loop");
    master_.setComponentID("transport.master");
    for (auto* component : std::initializer_list<juce::Component*> { &project_, &rewind_, &play_, &stop_, &record_, &loop_, &master_, &meter_ })
        addAndMakeVisible(component);
    master_.setTitle("Master volume");
    master_.setRange(0.0, 1.0, 0.01);
    master_.setValue(0.76);
    master_.valueToText = [](double value) { return juce::String(juce::roundToInt(value * 100.0)) + "%"; };
    setProjectActions({ "New project", "Open project...", "Save project", "Export stems..." });
    project_.onClick = [this] { projectMenu_.showFor(project_); };
    rewind_.onClick = [this] { auto callback = onRewind; if (callback) callback(); };
    play_.onClick = [this] { auto callback = onPlay; if (callback) callback(!playing_); };
    stop_.onClick = [this] { auto callback = onStop; if (callback) callback(); };
    record_.onClick = [this] { auto callback = onRecord; if (callback) callback(!recording_); };
    loop_.onClick = [this] { auto callback = onLoop; if (callback) callback(!looping_); };
    master_.onValueChange = [this](double value) { repaint(); auto callback = onMasterVolume; if (callback) callback(value); };
}

void TransportBar::setProjectTitle(juce::String title)
{
    projectTitle_ = std::move(title);
    repaint(titleBounds_);
}

void TransportBar::setSignature(int numerator, int denominator)
{
    if (numerator < 1 || numerator > 32 || denominator < 1 || denominator > 64
        || (denominator & (denominator - 1)) != 0)
        return;
    numerator_ = numerator;
    denominator_ = denominator;
    repaint(positionBounds_.getUnion(signatureBounds_));
}

juce::String TransportBar::getPositionText() const
{
    if (!timing_.signatures.empty())
    {
        const auto p = timing_.positionAt(beat_);
        return juce::String(static_cast<juce::int64>(p.bar)).paddedLeft('0', 3) + " | " + juce::String(p.beat)
            + " | " + juce::String(p.ticks).paddedLeft('0', 3);
    }
    const double quarterNotesPerBeat = 4.0 / static_cast<double>(denominator_);
    const double quarterNotesPerBar = static_cast<double>(numerator_) * quarterNotesPerBeat;
    const auto bar = static_cast<juce::int64>(beat_ / quarterNotesPerBar) + 1;
    const double quarterNotesInBar = std::fmod(beat_, quarterNotesPerBar);
    const auto beatInBar = static_cast<int>(quarterNotesInBar / quarterNotesPerBeat) + 1;
    const auto ticks = static_cast<int>(std::fmod(quarterNotesInBar, quarterNotesPerBeat) * 960.0);
    return juce::String(bar).paddedLeft('0', 3) + " | " + juce::String(beatInBar)
        + " | " + juce::String(ticks).paddedLeft('0', 3);
}

juce::String TransportBar::getSignatureText() const
{
    return juce::String(numerator_) + " / " + juce::String(denominator_);
}

void TransportBar::setProjectActions(juce::StringArray labels)
{
    projectMenu_.dismiss();
    std::vector<ui::Menu::Item> items;
    items.reserve(static_cast<std::size_t>(labels.size()));
    juce::Component::SafePointer<TransportBar> safe(this);
    for (int index = 0; index < labels.size(); ++index)
        items.push_back({ labels[index], [safe, index]
        {
            if (safe != nullptr)
            {
                auto callback = safe->onProjectAction;
                if (callback) callback(index);
            }
        } });
    projectMenu_.setItems(std::move(items));
    project_.setEnabled(!labels.isEmpty());
}

void TransportBar::setPlaying(bool playing) { playing_ = playing; play_.setSelected(playing); }
void TransportBar::setRecording(bool recording) { recording_ = recording; record_.setSelected(recording); }
void TransportBar::setLooping(bool looping) { looping_ = looping; loop_.setSelected(looping); }
void TransportBar::setPosition(double beat, double seconds)
{
    beat_ = std::isfinite(beat) ? juce::jlimit(0.0, 3999999.0, beat) : 0.0;
    seconds_ = std::isfinite(seconds) ? juce::jlimit(0.0, 359999.99, seconds) : 0.0;
    repaint(positionBounds_.getUnion(timeBounds_));
}
void TransportBar::setTempo(double tempo) { tempo_ = std::isfinite(tempo) ? juce::jlimit(1.0, 999.0, tempo) : 94.0; repaint(tempoBounds_); }
void TransportBar::setMasterLevels(float left, float right) { meter_.setLevels(left, right); }
void TransportBar::setMasterVolume(double volume) { master_.setValue(volume, juce::dontSendNotification); repaint(); }

void TransportBar::paint(juce::Graphics& g)
{
    g.fillAll(ui::colour(ui::tokens::surface));
    g.setColour(ui::colour(ui::tokens::border));
    g.drawHorizontalLine(getHeight() - 1, 0.0f, static_cast<float>(getWidth()));
    auto brand = brandBounds_;
    ui::drawIcon(g, ui::Icon::logo, brand.removeFromLeft(30).toFloat().reduced(3.0f), ui::colour(ui::tokens::muted));
    brand.removeFromLeft(8);
    text(g, "Obsidian", brand, ui::tokens::fontTitle);
    text(g, projectTitle_, titleBounds_);
    text(g, getPositionText(), positionBounds_, 16.0f);
    const auto wholeSeconds = static_cast<int>(seconds_);
    text(g, juce::String(wholeSeconds / 60).paddedLeft('0', 2) + ":" + juce::String(wholeSeconds % 60).paddedLeft('0', 2) + "." + juce::String(static_cast<int>(seconds_ * 100.0) % 100).paddedLeft('0', 2), timeBounds_);
    text(g, juce::String(tempo_, tempo_ == std::floor(tempo_) ? 0 : 1) + " BPM", tempoBounds_);
    text(g, getSignatureText(), signatureBounds_);
    auto label = masterLabelBounds_;
    text(g, master_.getValueText(), label.removeFromRight(40), 11.0f, ui::colour(ui::tokens::muted), juce::Justification::centredRight);
    text(g, "Master Volume", label, 11.0f, ui::colour(ui::tokens::muted));
}

void TransportBar::resized()
{
    const bool compact = getWidth() < 1400;
    auto row = getLocalBounds().reduced(compact ? 10 : 24, 0);
    brandBounds_ = row.removeFromLeft(compact ? 132 : 145);
    project_.setBounds(row.removeFromLeft(84).withSizeKeepingCentre(84, 30));
    row.removeFromLeft(compact ? 10 : 20);
    titleBounds_ = row.removeFromLeft(compact ? 95 : juce::jlimit(95, 232, getWidth() - 1354));
    const int masterWidth = compact ? 195 : 340;
    auto masterArea = row.removeFromRight(masterWidth).reduced(0, 10);
    auto meterArea = masterArea.removeFromRight(compact ? 67 : 112);
    meter_.setBounds(meterArea.reduced(6, 6));
    masterArea.removeFromRight(12);
    masterLabelBounds_ = masterArea.removeFromTop(16);
    master_.setBounds(masterArea);
    for (auto* button : { &rewind_, &play_, &stop_, &record_, &loop_ })
    {
        button->setBounds(row.removeFromLeft(compact ? 35 : 44).withSizeKeepingCentre(compact ? 31 : 40, kTransportHeight));
    }
    row.removeFromLeft(compact ? 8 : 18);
    positionBounds_ = row.removeFromLeft(compact ? 113 : 125);
    timeBounds_ = row.removeFromLeft(compact ? 75 : 95);
    tempoBounds_ = row.removeFromLeft(compact ? 62 : 72);
    signatureBounds_ = row;
}

TrackHeader::TrackHeader()
{
    for (auto* component : std::initializer_list<juce::Component*> { &mute_, &solo_, &gain_, &menu_ })
        addAndMakeVisible(component);
    mute_.setComponentID("track.mute");
    solo_.setComponentID("track.solo");
    gain_.setComponentID("track.gain");
    menu_.setComponentID("track.menu");
    mute_.setTitle("Mute track");
    solo_.setTitle("Solo track");
    gain_.setTitle("Track gain");
    gain_.setRange(-60.0, 6.0, 0.1);
    gain_.setValue(0.0);
    gain_.valueToText = [](double value) { return juce::String(value, 1) + " dB"; };
    mute_.onToggle = [this](bool value) { auto callback = onMute; if (callback) callback(value); };
    solo_.onToggle = [this](bool value) { auto callback = onSolo; if (callback) callback(value); };
    gain_.onValueChange = [this](double value) { repaint(); auto callback = onGain; if (callback) callback(value); };
    menu_.setVariant(ui::Button::Variant::subtle);
    menu_.onClick = [this] { auto callback = onMenu; if (callback) callback(); };
}

void TrackHeader::setTrack(int number, juce::String name, juce::Colour accent, ui::Icon icon)
{
    number_ = number; name_ = std::move(name); accent_ = accent; icon_ = icon;
    setTitle(name_);
    gain_.setAccent(accent_);
    repaint();
}
void TrackHeader::setGain(double db) { gain_.setValue(db, juce::dontSendNotification); repaint(); }
void TrackHeader::setMuted(bool muted) { mute_.setToggleState(muted, juce::dontSendNotification); }
void TrackHeader::setSolo(bool solo) { solo_.setToggleState(solo, juce::dontSendNotification); }

void TrackHeader::paint(juce::Graphics& g)
{
    g.fillAll(ui::colour(ui::tokens::surface));
    g.setColour(accent_);
    g.fillRoundedRectangle(0.0f, 0.0f, 7.0f, static_cast<float>(getHeight()), 2.0f);
    g.setColour(ui::colour(ui::tokens::border));
    g.drawRect(getLocalBounds(), 1);
    text(g, juce::String(number_), { 8, 0, 20, getHeight() }, 11.0f, ui::colour(ui::tokens::muted), juce::Justification::centred);
    ui::drawIcon(g, icon_, juce::Rectangle<float>(37.0f, static_cast<float>(getHeight()) * 0.5f - 13.0f, 26.0f, 26.0f), accent_);
    text(g, name_, { 80, 5, juce::jmax(0, getWidth() - 112), 26 });
    text(g, gain_.getValueText(), { getWidth() - 62, getHeight() - 32, 55, 24 }, 10.0f, ui::colour(ui::tokens::muted), juce::Justification::centredRight);
}

void TrackHeader::resized()
{
    const int y = getHeight() - 34;
    mute_.setBounds(80, y, 23, kTrackControlHeight);
    solo_.setBounds(106, y, 23, kTrackControlHeight);
    gain_.setBounds(142, y, juce::jmax(0, getWidth() - 209), kTrackControlHeight);
    menu_.setBounds(getWidth() - 29, 5, 22, 24);
}

TrackGroupHeader::TrackGroupHeader()
{
    juce::Component::setTitle("Backing Vocals group");
    heading_.setComponentID("group.toggle");
    add_.setComponentID("group.add");
    addAndMakeVisible(heading_);
    addAndMakeVisible(add_);
    heading_.setVariant(ui::Button::Variant::subtle);
    heading_.setTextJustification(juce::Justification::centredLeft);
    add_.setSize(ui::Button::Size::small);
    heading_.onClick = [this] { auto callback = onExpanded; if (callback) callback(!expanded_); };
    add_.onClick = [this] { auto callback = onAdd; if (callback) callback(); };
}
void TrackGroupHeader::setExpanded(bool expanded)
{
    expanded_ = expanded;
    heading_.setIcon(expanded ? ui::Icon::chevronDown : ui::Icon::chevronRight);
    heading_.setDescription(expanded ? "Expanded" : "Collapsed");
}
void TrackGroupHeader::setTitle(juce::String title) { juce::Component::setTitle(title); heading_.setText(std::move(title)); }
void TrackGroupHeader::paint(juce::Graphics& g)
{
    g.fillAll(ui::colour(ui::tokens::surface));
    g.setColour(ui::colour(ui::tokens::border));
    g.drawRect(getLocalBounds(), 1);
}
void TrackGroupHeader::resized()
{
    auto row = getLocalBounds().reduced(kHeaderPadding, 5);
    add_.setBounds(row.removeFromRight(116));
    row.removeFromRight(8);
    heading_.setBounds(row);
}
}
