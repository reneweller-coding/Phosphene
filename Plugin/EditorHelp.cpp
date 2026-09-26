/**
 * @file EditorHelp.cpp
 * @brief The help page, full screen and the update notice (23.09.2026).
 *
 * The help is the manual's prose (Tools/manual/chapters.txt, compiled in as PhospheneHelpData) arranged by
 * topic, plus two topics made on the spot: the parameters of the tab the help was opened from -- name, range,
 * default and the value it has now, read from the same descriptors that built the tab -- and the update
 * check. Noctuary's help page was the model: a topic list on the left, the text beside it, F1 to open it.
 */
#include "PluginEditor.h"
#include "PhospheneHelpData.h"
#include <cmath>

using namespace phos;
using namespace phosui;

namespace {
/** @brief chapters.txt as (name, text) pairs: paragraphs joined, indented lines kept as they stand. */
std::vector<std::pair<juce::String, juce::String>> readChapters()
{
    std::vector<std::pair<juce::String, juce::String>> out;
    const juce::String all = juce::String::fromUTF8(PhospheneHelpData::chapters_txt, PhospheneHelpData::chapters_txtSize);
    juce::StringArray lines = juce::StringArray::fromLines(all);
    juce::String name, textOf, para;
    auto flushPara = [&] { if (para.isNotEmpty()) { textOf << para.trimEnd() << "\n\n"; para.clear(); } };
    auto flushChapter = [&] { flushPara(); if (name.isNotEmpty()) out.emplace_back(name, textOf.trimEnd()); textOf.clear(); };
    for (const juce::String& raw : lines) {
        const juce::String l = raw.trimEnd();
        if (l.startsWith("#")) continue;
        if (l.startsWith("== ") && l.endsWith(" ==")) { flushChapter(); name = l.substring(3, l.length() - 3).trim(); continue; }
        if (name.isEmpty()) continue;
        if (l.isEmpty()) { flushPara(); continue; }
        if (l.startsWith("    ")) { flushPara(); textOf << l << "\n"; continue; }
        if (para.isNotEmpty()) para << " ";
        para << l.trim();
    }
    flushChapter();
    return out;
}

/** @brief A parameter's range in words, for the parameter topic. */
juce::String rangeOf(const ParamDesc& d)
{
    if (d.curve == Curve::Toggle) return "on / off";
    if (d.curve == Curve::Choice && d.choices != nullptr) {
        juce::StringArray c;
        for (int i = 0; i <= static_cast<int>(std::lround(d.maxValue)); ++i) c.add(d.choices[i]);
        return c.joinIntoString(" / ");
    }
    auto num = [&](float v) { return juce::String(v, std::fabs(v) >= 100.0f || d.curve == Curve::Int ? 0 : 2); };
    return num(d.minValue) + " .. " + num(d.maxValue) + (d.unit != nullptr && d.unit[0] != 0 ? juce::String(" ") + d.unit : juce::String());
}
} // namespace

// ==================================================================== HelpView

HelpView::HelpView(PhospheneProcessor& proc) : proc_(proc)
{
    for (const auto& [chapter, prose] : readChapters()) { names_.add(chapter); texts_.add(prose); }
    paramTopic_ = names_.size();
    names_.add("Parameters on this tab");
    texts_.add({});
    updateTopic_ = names_.indexOf("Updates");
    flowTopic_ = names_.indexOf("Signal flow");
    addChildComponent(flow_);

    list_.setModel(this);
    list_.setRowHeight(26);
    list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible(list_);
    text_.setMultiLine(true, true);
    text_.setReadOnly(true);
    text_.setCaretVisible(false);
    text_.setScrollbarsShown(true);
    text_.setFont(body(15.0f));
    text_.setColour(juce::TextEditor::backgroundColourId, card.withAlpha(0.6f));
    text_.setColour(juce::TextEditor::outlineColourId, edge);
    text_.setColour(juce::TextEditor::textColourId, text);
    text_.setIndents(14, 12);
    addAndMakeVisible(text_);

    // The update check's controls, shown with the "Updates" topic.
    const juce::File state = proc_.userFolder().getChildFile("update.txt");
    autoCheck_.setToggleState(phosui::UpdateCheck::enabled(state), juce::dontSendNotification);
    autoCheck_.onClick = [this, state] { phosui::UpdateCheck::setEnabled(state, autoCheck_.getToggleState()); };
    checkNow_.onClick = [this, state] { updates_->startIfDue(state, true); updateLine_.setText("asking GitHub ...", juce::dontSendNotification); };
    openRelease_.onClick = [this] {
        const juce::String url = updates_->result().url;
        juce::URL(url.isNotEmpty() ? url : juce::String("https://github.com/reneweller-coding/Phosphene/releases")).launchInDefaultBrowser();
    };
    updateLine_.setColour(juce::Label::textColourId, dim);
    for (juce::Component* c : { static_cast<juce::Component*>(&autoCheck_), static_cast<juce::Component*>(&checkNow_),
                                static_cast<juce::Component*>(&openRelease_), static_cast<juce::Component*>(&updateLine_) })
        addChildComponent(c);
    list_.updateContent();
    list_.selectRow(0);
}

juce::String HelpView::topicText(int index) const
{
    if (index < 0 || index >= texts_.size()) return {};
    juce::String s = texts_[index];
    if (index == updateTopic_) {
        s << "\n\nThis is Phosphene " << JucePlugin_VersionString << ".";
    }
    return s;
}

void HelpView::open(const juce::String& name, const juce::String& tabName, std::vector<int> paramIds)
{
    // The parameter topic: every parameter the tab shows, in the order it shows them.
    const ParamStore& p = proc_.params();
    juce::String s;
    s << "The parameters of the " << tabName << " tab, in the order the tab shows them: the name, the range, the "
      << "default and -- in brackets -- the value it has now. The key in front is the name a .phosset, phos_render "
      << "--set and MIDI learn use.\n\n";
    for (int id : paramIds) {
        if (id < 0 || id >= p.count()) continue;
        const ParamDesc& d = p.desc(id);
        auto shown = [&](float v) {
            if (d.curve == Curve::Toggle) return juce::String(v >= 0.5f ? "on" : "off");
            if (d.curve == Curve::Choice && d.choices != nullptr) return juce::String(d.choices[juce::jlimit(0, static_cast<int>(std::lround(d.maxValue)), static_cast<int>(std::lround(v)))]);
            return juce::String(v, std::fabs(v) >= 100.0f ? 0 : 2);
        };
        s << p.key(id) << "\n    " << d.name << ":  " << rangeOf(d) << ",  default " << shown(p.defaultValue(id))
          << "  (now " << shown(p.get(id)) << ")\n";
    }
    if (paramIds.empty()) s << "This tab has no parameters of its own.";
    texts_.set(paramTopic_, s);
    names_.set(paramTopic_, "Parameters: " + tabName);
    list_.updateContent();
    list_.repaintRow(paramTopic_);
    int row = names_.indexOf(name);
    if (row < 0) row = names_.indexOf("About");
    list_.selectRow(juce::jmax(0, row));
    selectedRowsChanged(list_.getSelectedRow());
}

void HelpView::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= names_.size()) return;
    if (selected) { g.setColour(accent.withAlpha(0.22f)); g.fillRoundedRectangle(2.0f, 1.0f, w - 4.0f, h - 2.0f, 5.0f); }
    g.setColour(selected ? text : dim);
    g.setFont(body(14.0f));
    g.drawText(names_[row], 12, 0, w - 16, h, juce::Justification::centredLeft, true);
}

void HelpView::selectedRowsChanged(int row)
{
    text_.setText(topicText(row), false);
    text_.moveCaretToTop(false);
    const bool upd = row == updateTopic_ && updateTopic_ >= 0;
    for (juce::Component* c : { static_cast<juce::Component*>(&autoCheck_), static_cast<juce::Component*>(&checkNow_),
                                static_cast<juce::Component*>(&openRelease_), static_cast<juce::Component*>(&updateLine_) })
        c->setVisible(upd);
    flow_.setVisible(row == flowTopic_ && flowTopic_ >= 0);
    if (upd) refreshUpdate();
    resized();
}

void HelpView::refreshUpdate()
{
    if (!updateLine_.isVisible()) return;
    const phosui::UpdateCheck::Result r = updates_->result();
    juce::String line = r.message.isNotEmpty() ? r.message : juce::String("Not asked yet.");
    if (r.asked && r.when.toMilliseconds() > 0) line << "   (asked " << r.when.formatted("%d.%m.%Y %H:%M") << ")";
    if (line != updateLine_.getText()) updateLine_.setText(line, juce::dontSendNotification);
    openRelease_.setEnabled(true);
}

void HelpView::paint(juce::Graphics& g)
{
    g.setColour(group);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
    g.setColour(text);
    g.setFont(title(13.0f));
    g.drawText("HELP", 22, 10, 120, 20, juce::Justification::centredLeft, false);
    g.setColour(dim);
    g.setFont(body(11.0f));
    g.drawText("F1, Esc or Help closes it.  The same text is the manual's (Phosphene-Manual.pdf).", 90, 10,
               getWidth() - 110, 20, juce::Justification::centredLeft, false);
}

void HelpView::resized()
{
    juce::Rectangle<int> r = getLocalBounds().reduced(14).withTrimmedTop(24);
    list_.setBounds(r.removeFromLeft(230));
    r.removeFromLeft(12);
    if (updateLine_.isVisible()) {
        juce::Rectangle<int> row = r.removeFromBottom(34);
        autoCheck_.setBounds(row.removeFromLeft(260));
        checkNow_.setBounds(row.removeFromLeft(120).reduced(4, 2));
        openRelease_.setBounds(row.removeFromLeft(190).reduced(4, 2));
        updateLine_.setBounds(r.removeFromBottom(26));
        r.removeFromBottom(6);
    }
    if (flow_.isVisible()) {
        // The picture on top at its own proportions, the chapter's text under it.
        const int h = juce::jmin(juce::roundToInt(static_cast<float>(r.getHeight()) * 0.68f),
                                 juce::roundToInt(static_cast<float>(r.getWidth()) * SignalFlow::kCanvasH / SignalFlow::kCanvasW));
        flow_.setBounds(r.removeFromTop(h));
        r.removeFromTop(8);
    }
    text_.setBounds(r.withWidth(juce::jmin(r.getWidth(), 900)));
}

// ==================================================================== the editor's side

void PhospheneEditor::showHelp(bool show)
{
    if (help_ == nullptr) return;
    if (show) {
        // The topic of the tab that is open, and that tab's parameters.
        std::vector<int> ids;
        if (auto* page = activePage())
            for (int g = 0; g < page->groupCount(); ++g)
                for (int id : page->groupParams(g)) ids.push_back(id);
        const juce::String tabName = tabNames()[tab_];
        help_->open(tabName, tabName, std::move(ids));
    }
    help_->setVisible(show);
    viewport_.setVisible(!show);
    for (auto* b : laneButtons_) b->setVisible(!show && tab_ == TabPerc);
    helpButton_.setToggleState(show, juce::dontSendNotification);
    layoutContent();
    content_.repaint();
    if (show) help_->grabKeyboardFocus();
}

void PhospheneEditor::toggleFullScreen()
{
    // Only the standalone has a window of its own; in a host the host owns it, and the button is hidden.
    auto* window = findParentComponentOfClass<juce::DocumentWindow>();
    if (window == nullptr) return;
    auto& desktop = juce::Desktop::getInstance();
    const bool on = desktop.getKioskModeComponent() != window;
    desktop.setKioskModeComponent(on ? window : nullptr, false);
    fullButton_.setToggleState(on, juce::dontSendNotification);
    grabKeyboardFocus();   // so F11 and Esc keep working in the new window state
}

void PhospheneEditor::parentHierarchyChanged()
{
    // As Noctuary: a maximise button beside the other two. Only the standalone has a DocumentWindow of its own;
    // in a host this finds nothing, which is right.
    //
    // Not here and now, but on the next turn of the message loop: this is called while JUCE's standalone window
    // is still putting its content in and is a few pixels big, and changing the title bar re-lays the window out
    // at that size -- which pushed the editor down to its minimum size (measured: 430 x 373 instead of the
    // design size, once the fixed aspect ratio no longer hid it).
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PhospheneEditor>(this)] {
        if (safe == nullptr) return;
        auto* window = safe->findParentComponentOfClass<juce::DocumentWindow>();
        if (window != nullptr)
            window->setTitleBarButtonsRequired(juce::DocumentWindow::minimiseButton | juce::DocumentWindow::maximiseButton
                                                   | juce::DocumentWindow::closeButton, false);
        safe->fullButton_.setVisible(window != nullptr);
    });
}
