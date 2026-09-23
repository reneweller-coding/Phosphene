/**
 * @file EditorLayout.cpp
 * @brief Implementation of the layout engine (see EditorLayout.h).
 */
#include "EditorLayout.h"
#include "PhospheneLookAndFeel.h"
#include "phos/WaveTableFile.h"
#include <cmath>

using namespace phos;

namespace phosui {

int ControlPage::addGroup(const juce::String& title, juce::Colour tint, int columns)
{
    Group g;
    g.title = title;
    g.tint = tint;
    g.columns = juce::jmax(1, columns);
    groups_.push_back(std::move(g));
    return static_cast<int>(groups_.size()) - 1;
}

void ControlPage::addParamCell(PhospheneProcessor& proc, int groupIndex, int paramId)
{
    StoreParameter* param = proc.parameterFor(paramId);
    if (param == nullptr) return;
    const ParamDesc& d = proc.params().desc(paramId);

    Cell c;
    c.param = paramId;
    c.label = std::make_unique<juce::Label>(juce::String(), d.name);
    c.label->setJustificationType(juce::Justification::centredTop);
    c.label->setColour(juce::Label::textColourId, dim);
    c.label->setInterceptsMouseClicks(false, false);
    // "Melody Temperature" under a 76-pixel cell: squeeze it rather than cut it off.
    c.label->setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(*c.label);

    switch (d.curve) {
    case Curve::Toggle: {
        auto b = std::make_unique<juce::ToggleButton>();
        b->setTooltip(juce::String(proc.params().key(paramId)));
        addAndMakeVisible(*b);
        c.button = std::make_unique<juce::ButtonParameterAttachment>(*param, *b);
        c.comp = std::move(b);
        break;
    }
    case Curve::Choice: {
        auto cb = std::make_unique<juce::ComboBox>();
        // The wavetable choice is the one list whose entries can be absent: indices from
        // kNumBuiltinWaveTables up come out of the shipped pack, and where that file was not
        // installed waveTable() quietly hands out the built-in table the descriptor names
        // (WaveTableFile.h). Quiet is right for the audio -- a set saved elsewhere goes on playing
        // -- and wrong for the chooser, which would otherwise show twelve tables that are not there.
        //
        // They are **marked, not removed**. ComboBoxParameterAttachment maps the parameter on to the
        // *position* of an item (index / (count - 1), juce_ParameterAttachments.cpp), so dropping
        // one would renumber every table behind it: "lead.table=7" would select a different table in
        // an installation with the pack than in one without, and a state saved in the one would load
        // wrong in the other. The contract is that the index is the table.
        //
        // Phase 8 has the same shape one step further on. `compose.melody_model` and
        // `compose.bass_model` each offer a learned entry that means nothing at all when the weight
        // file was not installed: the composer then draws exactly what "Markov" and "Pattern" draw
        // and says so on stderr, which in a DAW is nowhere. Marked for the same reason as a missing
        // table, and left in place for the same reason -- the index is the contract.
        const juce::String key(proc.params().key(paramId));
        const bool isTable = key.endsWith(".table");
        const bool isMelodyModel = key == "compose.melody_model";
        const bool isBassModel = key == "compose.bass_model";
        bool learnedMissing = false;
        if (isMelodyModel || isBassModel) {
            const PhospheneProcessor::LearnedModels models = proc.learnedModels();
            learnedMissing = isMelodyModel ? !models.melody : !models.bass;
        }
        for (int i = 0; d.choices != nullptr && i <= static_cast<int>(d.maxValue); ++i) {
            juce::String text = d.choices[i];
            if (isTable && waveTableIsLibrary(i) && !waveTableLoaded(i)) text += " (missing)";
            // Index 1 of both model lists is the learned draw; index 0 is the generator that is
            // always there (Params.cpp, kMelodyModelNames and kBassModelNames).
            if (learnedMissing && i == 1) text += " (missing)";
            cb->addItem(text, i + 1);
        }
        cb->setTooltip(juce::String(proc.params().key(paramId)));
        addAndMakeVisible(*cb);
        c.combo = std::make_unique<juce::ComboBoxParameterAttachment>(*param, *cb);
        c.comp = std::move(cb);
        c.units = 2;
        break;
    }
    default: {
        auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        // A range that spans zero reads far better as an arc out of the centre than as a full ring.
        if (d.minValue < -1.0e-6f && d.maxValue > 1.0e-6f) s->getProperties().set("bipolar", true);
        if (d.unit != nullptr && d.unit[0] != 0) s->setTextValueSuffix(juce::String(" ") + d.unit);
        s->setTooltip(juce::String(proc.params().key(paramId)));
        addAndMakeVisible(*s);
        c.slider = std::make_unique<juce::SliderParameterAttachment>(*param, *s);
        c.comp = std::move(s);
        break;
    }
    }
    if (c.comp != nullptr) enableMidiLearn(proc, *c.comp, paramId);
    groups_[static_cast<size_t>(groupIndex)].cells.push_back(static_cast<int>(cells_.size()));
    cells_.push_back(std::move(c));
}

void ControlPage::enableMidiLearn(PhospheneProcessor& proc, juce::Component& comp, int target)
{
    learnProc_ = &proc;
    learnTargets_.emplace_back(&comp, target);
    comp.addMouseListener(this, true);
}

void ControlPage::mouseDown(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu() || learnProc_ == nullptr) return;
    // The control that was clicked, or the learned control it lies inside (a combo box's label, say).
    int target = -1;
    for (juce::Component* c = e.originalComponent; c != nullptr && c != this && target < 0; c = c->getParentComponent())
        for (const auto& t : learnTargets_) if (t.first == c) { target = t.second; break; }
    if (target < 0) return;
    PhospheneProcessor* proc = learnProc_;
    phos::MidiMap& map = proc->midiMap();
    int ch = 0, cc = 0;
    const bool bound = map.controllerOf(target, ch, cc);
    const bool armed = map.armed() == target;
    juce::PopupMenu m;
    m.addSectionHeader(proc->midiTargetName(target) + (bound ? juce::String("  (CC ") + juce::String(cc) + ", ch " + juce::String(ch + 1) + ")" : juce::String()));
    m.addItem(1, armed ? "Cancel MIDI Learn" : "MIDI Learn: move a controller next");
    m.addItem(2, "Forget MIDI", bound);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(e.originalComponent), [proc, target, armed](int r) {
        if (r == 1) proc->midiMap().arm(armed ? -1 : target);
        if (r == 2) proc->midiMap().unbind(target);
    });
}

int ControlPage::addModuleGroup(PhospheneProcessor& proc, Module module, int instance, const juce::String& title,
                                juce::Colour tint, int columns, int first, int count)
{
    const int base = proc.params().base(module, instance);
    const int total = ParamStore::moduleCount(module);
    if (base < 0 || total <= 0) return -1;
    const int from = juce::jlimit(0, total, first);
    const int to = count < 0 ? total : juce::jlimit(from, total, from + count);
    const int g = addGroup(title, tint, columns);
    for (int i = from; i < to; ++i) addParamCell(proc, g, base + i);
    return g;
}

int ControlPage::addControl(int groupIndex, std::unique_ptr<juce::Component> comp, const juce::String& name, int units,
                            bool tall, int rows)
{
    if (groupIndex < 0 || groupIndex >= static_cast<int>(groups_.size()) || comp == nullptr) return -1;
    Cell c;
    c.units = juce::jmax(1, units);
    c.heightRows = juce::jmax(1, rows);
    c.tall = tall;
    if (name.isNotEmpty()) {
        c.label = std::make_unique<juce::Label>(juce::String(), name);
        c.label->setJustificationType(juce::Justification::centredTop);
        c.label->setColour(juce::Label::textColourId, dim);
        c.label->setInterceptsMouseClicks(false, false);
        c.label->setMinimumHorizontalScale(0.5f);
        addAndMakeVisible(*c.label);
    }
    addAndMakeVisible(*comp);
    c.comp = std::move(comp);
    groups_[static_cast<size_t>(groupIndex)].cells.push_back(static_cast<int>(cells_.size()));
    cells_.push_back(std::move(c));
    return static_cast<int>(cells_.size()) - 1;
}

std::vector<int> ControlPage::groupParams(int index) const
{
    std::vector<int> out;
    if (index < 0 || index >= static_cast<int>(groups_.size())) return out;
    for (int ci : groups_[static_cast<size_t>(index)].cells)
        if (cells_[static_cast<size_t>(ci)].param >= 0) out.push_back(cells_[static_cast<size_t>(ci)].param);
    return out;
}

int ControlPage::minimumWidth() const
{
    int widest = 0;
    for (const Group& g : groups_) widest = juce::jmax(widest, g.columns * kCellW + 2 * kGroupPad);
    return widest + 2 * kPagePad;
}

int ControlPage::layout(int width)
{
    // Two nested flows, both measured: cells into rows of their group, groups into rows of the page.
    // Nothing here knows where anything is; it only knows how wide a thing is and how much is left.
    const int usable = juce::jmax(kCellW, width - 2 * kPagePad);
    int x = kPagePad, y = kPagePad, rowHeight = 0;

    for (Group& g : groups_) {
        // Flow the cells into rows of the group first, in the group's own coordinates; how tall the
        // group is falls out of that, and only then is it placed on the page.
        int cx = 0, cy = 0, used = 0;
        for (int ci : g.cells) {
            Cell& c = cells_[static_cast<size_t>(ci)];
            const int u = juce::jmin(g.columns, c.units);
            if (c.heightRows > 1) {
                if (used > 0) { ++cy; used = 0; cx = 0; }
                c.bounds = juce::Rectangle<int>(cx * kCellW, cy * kCellH, u * kCellW, c.heightRows * kCellH);
                cy += c.heightRows;
                continue;
            }
            if (used + u > g.columns) { used = 0; cx = 0; ++cy; }
            c.bounds = juce::Rectangle<int>(cx * kCellW, cy * kCellH, u * kCellW, kCellH);
            used += u;
            cx += u;
        }
        g.rows = juce::jmax(1, cy + (used > 0 ? 1 : 0));
        const int gw = g.columns * kCellW + 2 * kGroupPad;
        const int gh = kGroupTitleH + g.rows * kCellH + kGroupPad;
        if (x > kPagePad && x + gw > kPagePad + usable) { x = kPagePad; y += rowHeight + kGroupGap; rowHeight = 0; }
        g.bounds = juce::Rectangle<int>(x, y, gw, gh);
        rowHeight = juce::jmax(rowHeight, gh);
        x += gw + kGroupGap;
        for (int ci : g.cells)
            cells_[static_cast<size_t>(ci)].bounds.translate(g.bounds.getX() + kGroupPad, g.bounds.getY() + kGroupTitleH);
    }
    contentHeight_ = y + rowHeight + kPagePad;
    setSize(width, contentHeight_);
    resized();
    return contentHeight_;
}

void ControlPage::resized()
{
    for (Cell& c : cells_) {
        if (c.comp == nullptr) continue;
        juce::Rectangle<int> r = c.bounds.reduced(3, 3);
        if (c.tall || c.label == nullptr) {
            c.comp->setBounds(r);
            if (c.label != nullptr) c.label->setBounds(juce::Rectangle<int>());
            continue;
        }
        juce::Rectangle<int> labelArea = r.removeFromBottom(kLabelH);
        if (dynamic_cast<juce::Slider*>(c.comp.get()) != nullptr) {
            // A knob is round: keep it square and centred in what is left.
            const int side = juce::jmin(r.getWidth(), r.getHeight());
            c.comp->setBounds(juce::Rectangle<int>(side, side).withCentre(r.getCentre()));
        } else {
            const int h = juce::jmin(r.getHeight(), 26);
            c.comp->setBounds(r.withSizeKeepingCentre(r.getWidth(), h));
        }
        c.label->setBounds(labelArea);
    }
}

void ControlPage::paint(juce::Graphics& g)
{
    for (const Group& grp : groups_) {
        const juce::Rectangle<float> r = grp.bounds.toFloat();
        g.setColour(phosui::group);
        g.fillRoundedRectangle(r, 7.0f);
        g.setColour(edge);
        g.drawRoundedRectangle(r.reduced(0.5f), 7.0f, 1.0f);
        g.setColour(grp.tint.withAlpha(0.14f));
        g.fillRoundedRectangle(r.withHeight(static_cast<float>(kGroupTitleH)), 7.0f);
        g.setColour(grp.tint);
        g.setFont(title(11.5f));
        g.drawText(grp.title.toUpperCase(), grp.bounds.withHeight(kGroupTitleH).reduced(kGroupPad, 2),
                   juce::Justification::centredLeft, false);
    }
}

} // namespace phosui
