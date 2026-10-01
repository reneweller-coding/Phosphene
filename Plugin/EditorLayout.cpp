/**
 * @file EditorLayout.cpp
 * @brief Implementation of the layout engine (see EditorLayout.h).
 */
#include "EditorLayout.h"
#include "PhospheneLookAndFeel.h"
#include "phos/WaveTableFile.h"
#include <cmath>
#include <limits>

using namespace phos;

namespace phosui {

int ControlPage::addGroup(const juce::String& title, juce::Colour tint, int columns)
{
    Group g;
    g.title = title;
    g.tint = groupColour(title, tint);   // the frame's family by what the group does, else the page's colour
    g.columns = juce::jmax(1, columns);
    groups_.push_back(std::move(g));
    return static_cast<int>(groups_.size()) - 1;
}

void ControlPage::addParamCell(PhospheneProcessor& proc, int groupIndex, int paramId, CellSize size)
{
    StoreParameter* param = proc.parameterFor(paramId);
    if (param == nullptr) return;
    const ParamDesc& d = proc.params().desc(paramId);
    const juce::Colour tint = groups_[static_cast<size_t>(groupIndex)].tint;

    Cell c;
    c.param = paramId;
    c.size = size;
    c.label = std::make_unique<juce::Label>(juce::String(), d.name);
    c.label->setJustificationType(juce::Justification::centredTop);
    // A large knob is what the page is about, and its name reads so.
    c.label->setColour(juce::Label::textColourId, size == CellSize::Large ? text : dim);
    c.label->setInterceptsMouseClicks(false, false);
    // "Melody Temperature" under a 76-pixel cell: squeeze it rather than cut it off.
    c.label->setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(*c.label);

    switch (d.curve) {
    case Curve::Toggle: {
        auto b = std::make_unique<juce::ToggleButton>();
        b->setColour(juce::ToggleButton::tickColourId, tint);
        b->setTooltip(juce::String(proc.params().key(paramId)));
        c.gridW = 2;
        c.gridH = 2;
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
            juce::String item = d.choices[i];
            if (isTable && waveTableIsLibrary(i) && !waveTableLoaded(i)) item += " (missing)";
            // Index 1 of both model lists is the learned draw; index 0 is the generator that is
            // always there (Params.cpp, kMelodyModelNames and kBassModelNames).
            if (learnedMissing && i == 1) item += " (missing)";
            cb->addItem(item, i + 1);
        }
        cb->setTooltip(juce::String(proc.params().key(paramId)));
        addAndMakeVisible(*cb);
        c.combo = std::make_unique<juce::ComboBoxParameterAttachment>(*param, *cb);
        c.comp = std::move(cb);
        c.units = 2;
        c.gridW = 4;
        c.gridH = 2;   // a chooser is a line of text and its name: two thirds of a knob's height
        break;
    }
    default: {
        auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        // A range that spans zero reads far better as an arc out of the centre than as a full ring.
        if (d.minValue < -1.0e-6f && d.maxValue > 1.0e-6f) s->getProperties().set("bipolar", true);
        if (d.unit != nullptr && d.unit[0] != 0) s->setTextValueSuffix(juce::String(" ") + d.unit);
        s->setColour(juce::Slider::rotarySliderFillColourId, tint);   // the arc in the page's colour
        c.gridW = size == CellSize::Large ? 4 : 2;
        c.gridH = size == CellSize::Large ? 5 : (size == CellSize::Small ? 2 : 3);
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

void ControlPage::showValues(const phos::ParamStore& store, int first, int count, const std::vector<std::pair<int, float>>* values)
{
    for (Cell& c : cells_) {
        if (c.param < first || c.param >= first + count || c.comp == nullptr) continue;
        float v = store.get(c.param);
        if (values != nullptr)
            for (const auto& kv : *values) if (first + kv.first == c.param) { v = kv.second; break; }
        if (auto* s = dynamic_cast<juce::Slider*>(c.comp.get())) {
            if (s->getValue() != static_cast<double>(v) && !s->isMouseButtonDown()) s->setValue(v, juce::dontSendNotification);
        } else if (auto* cb = dynamic_cast<juce::ComboBox*>(c.comp.get())) {
            const int id = static_cast<int>(std::lround(v)) + 1;
            if (cb->getSelectedId() != id) cb->setSelectedId(id, juce::dontSendNotification);
        } else if (auto* t = dynamic_cast<juce::ToggleButton*>(c.comp.get())) {
            if (t->getToggleState() != (v >= 0.5f)) t->setToggleState(v >= 0.5f, juce::dontSendNotification);
        }
    }
}

void ControlPage::showPlayed(const PhospheneProcessor& proc)
{
    for (Cell& c : cells_)
        if (c.param >= 0 && c.comp != nullptr)
            if (auto* s = dynamic_cast<juce::Slider*>(c.comp.get())) showLive(*s, proc.playedValue(c.param));
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
    // The frame's third item (01.10.2026, as in every generator): back to the default, an undo step.
    const bool param = target >= 0 && target < proc->params().count();
    m.addItem(3, "Default value", param);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(e.originalComponent), [proc, target, armed](int r) {
        if (r == 1) proc->midiMap().arm(armed ? -1 : target);
        if (r == 2) proc->midiMap().unbind(target);
        if (r == 3) proc->resetToDefault(target);
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

int ControlPage::addParamsGroup(PhospheneProcessor& proc, const juce::String& title, juce::Colour tint, int columns,
                                const std::vector<int>& paramIds)
{
    const int g = addGroup(title, tint, columns);
    for (int id : paramIds) if (id >= 0 && id < proc.params().count()) addParamCell(proc, g, id);
    return g;
}

int ControlPage::addSizedGroup(PhospheneProcessor& proc, const juce::String& title, juce::Colour tint, int columns,
                               const std::vector<SizedParam>& params)
{
    const int g = addGroup(title, tint, columns);
    for (const SizedParam& sp : params)
        if (sp.id >= 0 && sp.id < proc.params().count()) addParamCell(proc, g, sp.id, sp.size);
    return g;
}

int ControlPage::addControl(int groupIndex, std::unique_ptr<juce::Component> comp, const juce::String& name, int units,
                            bool tall, int rows, const std::vector<int>& bound)
{
    if (groupIndex < 0 || groupIndex >= static_cast<int>(groups_.size()) || comp == nullptr) return -1;
    Cell c;
    c.bound = bound;
    c.units = juce::jmax(1, units);
    c.heightRows = juce::jmax(1, rows);
    c.gridW = 2 * c.units;
    c.gridH = 3 * c.heightRows;
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
    for (int ci : groups_[static_cast<size_t>(index)].cells) {
        const Cell& c = cells_[static_cast<size_t>(ci)];
        if (c.param >= 0) out.push_back(c.param);
        out.insert(out.end(), c.bound.begin(), c.bound.end());
    }
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
    // Two packings, both measured: the cells into the grid of their group, then the groups onto the page.
    // Nothing here knows where anything is; it only knows how large a thing is and where there is room.
    const int usable = juce::jmax(kCellW, width - 2 * kPagePad);
    for (Group& g : groups_) g.extraColumns = 0;
    // Then a group that has room to its right takes it, in whole cells, as far as its cells would still fill a
    // row -- never wider than all of them side by side, so no group grows empty space inside itself -- and the
    // page is placed again, since a group that grew wider grew shorter (26.09.2026, "ohne zu große Lücken").
    for (int pass = 0; pass < 3; ++pass) {
        place(usable);
        bool grew = false;
        for (Group& g : groups_) {
            if (g.fill) continue;
            int limit = kPagePad + usable;
            for (const Group& o : groups_)
                if (&o != &g && o.bounds.getX() >= g.bounds.getRight() && o.bounds.getY() < g.bounds.getBottom()
                    && g.bounds.getY() < o.bounds.getBottom())
                    limit = juce::jmin(limit, o.bounds.getX() - kGroupGap);
            const int room = (limit - g.bounds.getRight()) / kCellW;
            // The narrowest of the widths the room allows that reaches the lowest of their heights, and only if
            // that is lower than now: beside a large knob the small ones then stack instead of lining up in a row
            // under which the group stands empty.
            const int now = g.columns + g.extraColumns;
            int bestCols = now, bestH = pack(g, 2 * now, false);
            for (int add = 1; add <= room; ++add) {
                const int h = pack(g, 2 * (now + add), false);
                if (h < bestH) { bestH = h; bestCols = now + add; }
            }
            if (bestCols > now) { g.extraColumns += bestCols - now; grew = true; }
        }
        if (!grew) break;
    }
    contentHeight_ = place(usable) + kPagePad;
    setSize(width, contentHeight_);
    resized();
    return contentHeight_;
}

int ControlPage::pack(Group& g, int gridCols, bool apply)
{
    // Every cell goes to the first place it fits, row by row (26.09.2026): a large knob takes 4 x 5 grid units and
    // the small and normal cells after it fill the rows beside it, where a flow of equal cells would have left a
    // hole under every shorter one.
    std::vector<std::vector<char>> used;
    auto freeAt = [&](int r, int col, int w, int h) {
        for (int y = r; y < r + h && y < static_cast<int>(used.size()); ++y)
            for (int x = col; x < col + w; ++x)
                if (used[static_cast<size_t>(y)][static_cast<size_t>(x)] != 0) return false;
        return true;
    };
    int bottom = 0;
    for (int ci : g.cells) {
        Cell& c = cells_[static_cast<size_t>(ci)];
        const int w = juce::jmin(gridCols, c.gridW), h = c.gridH;
        int row = 0, col = 0;
        for (bool placed = false; !placed; ++row) {
            for (col = 0; col + w <= gridCols; ++col)
                if (freeAt(row, col, w, h)) { placed = true; break; }
            if (placed) break;
        }
        while (static_cast<int>(used.size()) < row + h) used.emplace_back(static_cast<size_t>(gridCols), static_cast<char>(0));
        for (int y = row; y < row + h; ++y)
            for (int x = col; x < col + w; ++x) used[static_cast<size_t>(y)][static_cast<size_t>(x)] = 1;
        if (apply) c.bounds = juce::Rectangle<int>(col * kColU, row * kRowU, w * kColU, h * kRowU);
        bottom = juce::jmax(bottom, row + h);
    }
    return bottom;
}

void ControlPage::setGroupVisible(int groupIndex, bool visible)
{
    if (groupIndex < 0 || groupIndex >= static_cast<int>(groups_.size())) return;
    Group& g = groups_[static_cast<size_t>(groupIndex)];
    if (g.hidden == !visible) return;
    g.hidden = !visible;
    for (int ci : g.cells) {
        Cell& c = cells_[static_cast<size_t>(ci)];
        if (c.comp != nullptr) c.comp->setVisible(visible);
        if (c.label != nullptr) c.label->setVisible(visible);
    }
    layout(getWidth());
    repaint();
}

int ControlPage::place(int usable)
{
    for (Group& g : groups_) {
        if (g.hidden) { g.bounds = {}; continue; }
        if (g.fill) {
            // As wide as the page, in whole grid columns; its control (a pattern roll) takes all of them.
            g.extraColumns = juce::jmax(0, (usable - 2 * kGroupPad) / kCellW - g.columns);
            for (int ci : g.cells) cells_[static_cast<size_t>(ci)].gridW = 2 * (g.columns + g.extraColumns);
        }
        const int bottom = pack(g, 2 * (g.columns + g.extraColumns), true);
        g.rows = juce::jmax(1, (bottom + 2) / 3);
        const int gpw = (g.columns + g.extraColumns) * kCellW + 2 * kGroupPad;
        const int gph = kGroupTitleH + juce::jmax(2, bottom) * kRowU + kGroupPad;
        g.bounds = juce::Rectangle<int>(0, 0, gpw, gph);
    }

    // On the page, a skyline (26.09.2026): each group, in order, goes where the page is lowest across its width --
    // the leftmost of the lowest -- instead of into rows as tall as their tallest group, so a short group no
    // longer leaves a gap under itself as wide as it is.
    std::vector<juce::Rectangle<int>> placed;
    int contentBottom = kPagePad;
    for (Group& g : groups_) {
        if (g.hidden) continue;
        const int gw = juce::jmin(g.bounds.getWidth(), usable);
        std::vector<int> xs { kPagePad };
        for (const auto& r : placed)
            if (r.getRight() + kGroupGap + gw <= kPagePad + usable) xs.push_back(r.getRight() + kGroupGap);
        int bestX = kPagePad, bestY = std::numeric_limits<int>::max();
        for (int x : xs) {
            int y = kPagePad;
            for (const auto& r : placed)
                if (r.getX() < x + gw && x < r.getRight()) y = juce::jmax(y, r.getBottom() + kGroupGap);
            if (y < bestY || (y == bestY && x < bestX)) { bestY = y; bestX = x; }
        }
        g.bounds.setPosition(bestX, bestY);
        placed.push_back(g.bounds);
        contentBottom = juce::jmax(contentBottom, g.bounds.getBottom());
        for (int ci : g.cells)
            cells_[static_cast<size_t>(ci)].bounds.translate(g.bounds.getX() + kGroupPad, g.bounds.getY() + kGroupTitleH);
    }
    return contentBottom;
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
        const int labelH = c.size == CellSize::Large ? kLabelH + 3 : (c.size == CellSize::Small ? kLabelH - 2 : kLabelH);
        juce::Rectangle<int> labelArea = r.removeFromBottom(labelH);
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
    // The group boxes as every generator draws them (01.10.2026, the frame): the box, its title in its family's colour
    // and a hairline under it.
    for (const Group& grp : groups_) {
        if (grp.hidden) continue;
        const juce::Rectangle<float> r = grp.bounds.toFloat();
        g.setColour(phosui::group);
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(edge);
        g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);
        g.setColour(grp.tint);
        g.setFont(title(11.0f));
        g.drawText(grp.title.toUpperCase(), grp.bounds.withHeight(kGroupTitleH).reduced(kGroupPad + 1, 2),
                   juce::Justification::centredLeft, false);
        g.setColour(grp.tint.withAlpha(0.45f));
        g.fillRect(r.getX() + kGroupPad, r.getY() + static_cast<float>(kGroupTitleH) - 2.0f, r.getWidth() - 2.0f * kGroupPad, 1.0f);
    }
}

} // namespace phosui
