/**
 * @file EditorLayout.cpp
 * @brief Implementation of the layout engine (see EditorLayout.h).
 * @author Rene Weller
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
    g.m_title = title;
    g.m_tint = groupColour(title, tint);   // the frame's family by what the group does, else the page's colour
    g.m_columns = juce::jmax(1, columns);
    m_groups.push_back(std::move(g));
    return static_cast<int>(m_groups.size()) - 1;
}

void ControlPage::addParamCell(PhospheneProcessor& proc, int groupIndex, int paramId, CellSizeE size)
{
    StoreParameter* param = proc.parameterFor(paramId);
    if (param == nullptr) return;
    const ParamDesc& d = proc.params().desc(paramId);
    const juce::Colour tint = m_groups[static_cast<size_t>(groupIndex)].m_tint;

    Cell c;
    c.m_param = paramId;
    c.m_size = size;
    c.m_label = std::make_unique<juce::Label>(juce::String(), d.m_name);
    c.m_label->setJustificationType(juce::Justification::centredTop);
    // A large knob is what the page is about, and its name reads so.
    c.m_label->setColour(juce::Label::textColourId, size == CellSizeE::LARGE ? Text : Dim);
    c.m_label->setInterceptsMouseClicks(false, false);
    // "Melody Temperature" under a 76-pixel cell: squeeze it rather than cut it off.
    c.m_label->setMinimumHorizontalScale(0.5f);
    addAndMakeVisible(*c.m_label);

    switch (d.m_curve) {
    case CurveE::TOGGLE: {
        auto b = std::make_unique<juce::ToggleButton>();
        b->setColour(juce::ToggleButton::tickColourId, tint);
        b->setTooltip(juce::String(proc.params().key(paramId)));
        c.m_gridW = 2;
        c.m_gridH = 2;
        addAndMakeVisible(*b);
        c.m_button = std::make_unique<juce::ButtonParameterAttachment>(*param, *b);
        c.m_comp = std::move(b);
        break;
    }
    case CurveE::CHOICE: {
        auto cb = std::make_unique<juce::ComboBox>();
        // The wavetable choice is the one list whose entries can be absent: indices from
        // NumBuiltinWaveTables up come out of the shipped pack, and where that file was not
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
            learnedMissing = isMelodyModel ? !models.m_melody : !models.m_bass;
        }
        for (int i = 0; d.m_choices != nullptr && i <= static_cast<int>(d.m_maxValue); ++i) {
            juce::String item = d.m_choices[i];
            if (isTable && waveTableIsLibrary(i) && !waveTableLoaded(i)) item += " (missing)";
            // Index 1 of both model lists is the learned draw; index 0 is the generator that is
            // always there (Params.cpp, MelodyModelNames and BassModelNames).
            if (learnedMissing && i == 1) item += " (missing)";
            cb->addItem(item, i + 1);
        }
        cb->setTooltip(juce::String(proc.params().key(paramId)));
        addAndMakeVisible(*cb);
        c.m_combo = std::make_unique<juce::ComboBoxParameterAttachment>(*param, *cb);
        c.m_comp = std::move(cb);
        c.m_units = 2;
        c.m_gridW = 4;
        c.m_gridH = 2;   // a chooser is a line of text and its name: two thirds of a knob's height
        break;
    }
    default: {
        auto s = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        // A range that spans zero reads far better as an arc out of the centre than as a full ring.
        if (d.m_minValue < -1.0e-6f && d.m_maxValue > 1.0e-6f) s->getProperties().set("bipolar", true);
        if (d.m_unit != nullptr && d.m_unit[0] != 0) s->setTextValueSuffix(juce::String(" ") + d.m_unit);
        s->setColour(juce::Slider::rotarySliderFillColourId, tint);   // the arc in the page's colour
        c.m_gridW = size == CellSizeE::LARGE ? 4 : 2;
        c.m_gridH = size == CellSizeE::LARGE ? 5 : (size == CellSizeE::SMALL ? 2 : 3);
        s->setTooltip(juce::String(proc.params().key(paramId)));
        addAndMakeVisible(*s);
        c.m_slider = std::make_unique<juce::SliderParameterAttachment>(*param, *s);
        c.m_comp = std::move(s);
        break;
    }
    }
    if (c.m_comp != nullptr) {
        enableMidiLearn(proc, *c.m_comp, paramId);
        c.m_comp->getProperties().set("frameId", paramId);   // the family's search and info line find it (Frame.h, findControl)
    }
    m_groups[static_cast<size_t>(groupIndex)].m_cells.push_back(static_cast<int>(m_cells.size()));
    m_cells.push_back(std::move(c));
}

void ControlPage::showValues(const phos::ParamStore& store, int first, int count, const std::vector<std::pair<int, float>>* values)
{
    for (Cell& c : m_cells) {
        if (c.m_param < first || c.m_param >= first + count || c.m_comp == nullptr) continue;
        float v = store.get(c.m_param);
        if (values != nullptr)
            for (const auto& kv : *values) if (first + kv.first == c.m_param) { v = kv.second; break; }
        if (auto* s = dynamic_cast<juce::Slider*>(c.m_comp.get())) {
            if (s->getValue() != static_cast<double>(v) && !s->isMouseButtonDown()) s->setValue(v, juce::dontSendNotification);
        } else if (auto* cb = dynamic_cast<juce::ComboBox*>(c.m_comp.get())) {
            const int id = static_cast<int>(std::lround(v)) + 1;
            if (cb->getSelectedId() != id) cb->setSelectedId(id, juce::dontSendNotification);
        } else if (auto* t = dynamic_cast<juce::ToggleButton*>(c.m_comp.get())) {
            if (t->getToggleState() != (v >= 0.5f)) t->setToggleState(v >= 0.5f, juce::dontSendNotification);
        }
    }
}

void ControlPage::showPlayed(const PhospheneProcessor& proc)
{
    const bool arcs = m_modulation != nullptr && m_modulation->access().m_arcs;
    for (Cell& c : m_cells)
        if (c.m_param >= 0 && c.m_comp != nullptr)
            if (auto* s = dynamic_cast<juce::Slider*>(c.m_comp.get())) {
                showLive(*s, proc.playedValue(c.m_param));
                if (arcs) frame::showModArcs(*s, m_modulation->access().m_arcs(c.m_param));   // the voice's matrix rows on it
            }
}

void ControlPage::reveal(juce::Component& part)
{
    if (!split()) return;
    for (size_t g = 0; g < m_groups.size(); ++g)
        for (int i : m_groups[g].m_cells) {
            juce::Component* comp = m_cells[static_cast<size_t>(i)].m_comp.get();
            if (comp == nullptr || (comp != &part && !comp->isParentOf(&part))) continue;
            if (g < m_sectionOf.size() && m_sectionOf[g] != m_sections->current()) {
                m_sections->setCurrent(m_sectionOf[g]);
                applySections();
                if (m_sectionChanged) m_sectionChanged();
            }
            return;
        }
}

void ControlPage::setCellName(int cell, const juce::String& name)
{
    if (cell < 0 || cell >= static_cast<int>(m_cells.size())) return;
    if (auto& label = m_cells[static_cast<size_t>(cell)].m_label; label != nullptr && label->getText() != name) label->setText(name, juce::dontSendNotification);
}

void ControlPage::enableMidiLearn(PhospheneProcessor& proc, juce::Component& comp, int target)
{
    m_learnProc = &proc;
    m_learnTargets.emplace_back(&comp, target);
    comp.addMouseListener(this, true);
}

void ControlPage::mouseDown(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu() || m_learnProc == nullptr) return;
    // The control that was clicked, or the learned control it lies inside (a combo box's label, say).
    int target = -1;
    for (juce::Component* c = e.originalComponent; c != nullptr && c != this && target < 0; c = c->getParentComponent())
        for (const auto& t : m_learnTargets) if (t.first == c) { target = t.second; break; }
    if (target < 0) return;
    PhospheneProcessor* proc = m_learnProc;
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
    if (param && m_modulation != nullptr) m_modulation->addMenuItems(m, target);   // the modulation at the knob (15.10.2026)
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(e.originalComponent), [proc, target, armed](int r) {
        if (r == 1) proc->midiMap().arm(armed ? -1 : target);
        if (r == 2) proc->midiMap().unbind(target);
        if (r == 3) proc->resetToDefault(target);
    });
}

int ControlPage::addModuleGroup(PhospheneProcessor& proc, ModuleE module, int instance, const juce::String& title,
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
        if (sp.m_id >= 0 && sp.m_id < proc.params().count()) addParamCell(proc, g, sp.m_id, sp.m_size);
    return g;
}

int ControlPage::addControl(int groupIndex, std::unique_ptr<juce::Component> comp, const juce::String& name, int units,
                            bool tall, int rows, const std::vector<int>& bound)
{
    if (groupIndex < 0 || groupIndex >= static_cast<int>(m_groups.size()) || comp == nullptr) return -1;
    Cell c;
    c.m_bound = bound;
    c.m_units = juce::jmax(1, units);
    c.m_heightRows = juce::jmax(1, rows);
    c.m_gridW = 2 * c.m_units;
    c.m_gridH = 3 * c.m_heightRows;
    c.m_tall = tall;
    if (name.isNotEmpty()) {
        c.m_label = std::make_unique<juce::Label>(juce::String(), name);
        c.m_label->setJustificationType(juce::Justification::centredTop);
        c.m_label->setColour(juce::Label::textColourId, Dim);
        c.m_label->setInterceptsMouseClicks(false, false);
        c.m_label->setMinimumHorizontalScale(0.5f);
        addAndMakeVisible(*c.m_label);
    }
    addAndMakeVisible(*comp);
    c.m_comp = std::move(comp);
    m_groups[static_cast<size_t>(groupIndex)].m_cells.push_back(static_cast<int>(m_cells.size()));
    m_cells.push_back(std::move(c));
    return static_cast<int>(m_cells.size()) - 1;
}

std::vector<int> ControlPage::groupParams(int index) const
{
    std::vector<int> out;
    if (index < 0 || index >= static_cast<int>(m_groups.size())) return out;
    for (int ci : m_groups[static_cast<size_t>(index)].m_cells) {
        const Cell& c = m_cells[static_cast<size_t>(ci)];
        if (c.m_param >= 0) out.push_back(c.m_param);
        out.insert(out.end(), c.m_bound.begin(), c.m_bound.end());
    }
    return out;
}

int ControlPage::minimumWidth() const
{
    int widest = 0;
    for (const Group& g : m_groups) widest = juce::jmax(widest, g.m_columns * CellW + 2 * GroupPad);
    return widest + 2 * PagePad;
}

int ControlPage::layout(int width)
{
    // Two packings, both measured: the cells into the grid of their group, then the groups onto the page.
    // Nothing here knows where anything is; it only knows how large a thing is and where there is room.
    const int usable = juce::jmax(CellW, width - 2 * PagePad);
    plan(usable);   // the sections for this width first (01.10.2026)
    m_contentHeight = arrange(usable);
    setSize(width, m_contentHeight);
    resized();
    return m_contentHeight;
}

int ControlPage::arrange(int usable)
{
    for (Group& g : m_groups) g.m_extraColumns = 0;
    // Then a group that has room to its right takes it, in whole cells, as far as its cells would still fill a
    // row -- never wider than all of them side by side, so no group grows empty space inside itself -- and the
    // page is placed again, since a group that grew wider grew shorter (26.09.2026, "ohne zu große Lücken").
    for (int pass = 0; pass < 3; ++pass) {
        place(usable);
        bool grew = false;
        for (Group& g : m_groups) {
            if (g.m_fill) continue;
            int limit = PagePad + usable;
            for (const Group& o : m_groups)
                if (&o != &g && o.m_bounds.getX() >= g.m_bounds.getRight() && o.m_bounds.getY() < g.m_bounds.getBottom()
                    && g.m_bounds.getY() < o.m_bounds.getBottom())
                    limit = juce::jmin(limit, o.m_bounds.getX() - GroupGap);
            const int room = (limit - g.m_bounds.getRight()) / CellW;
            // The narrowest of the widths the room allows that reaches the lowest of their heights, and only if
            // that is lower than now: beside a large knob the small ones then stack instead of lining up in a row
            // under which the group stands empty.
            const int now = g.m_columns + g.m_extraColumns;
            int bestCols = now, bestH = pack(g, 2 * now, false);
            for (int add = 1; add <= room; ++add) {
                const int h = pack(g, 2 * (now + add), false);
                if (h < bestH) { bestH = h; bestCols = now + add; }
            }
            if (bestCols > now) { g.m_extraColumns += bestCols - now; grew = true; }
        }
        if (!grew) break;
    }
    return place(usable) + PagePad;
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
    for (int ci : g.m_cells) {
        Cell& c = m_cells[static_cast<size_t>(ci)];
        const int w = juce::jmin(gridCols, c.m_gridW), h = c.m_gridH;
        int row = 0, col = 0;
        for (bool placed = false; !placed; ++row) {
            for (col = 0; col + w <= gridCols; ++col)
                if (freeAt(row, col, w, h)) { placed = true; break; }
            if (placed) break;
        }
        while (static_cast<int>(used.size()) < row + h) used.emplace_back(static_cast<size_t>(gridCols), static_cast<char>(0));
        for (int y = row; y < row + h; ++y)
            for (int x = col; x < col + w; ++x) used[static_cast<size_t>(y)][static_cast<size_t>(x)] = 1;
        if (apply) c.m_bounds = juce::Rectangle<int>(col * ColU, row * RowU, w * ColU, h * RowU);
        bottom = juce::jmax(bottom, row + h);
    }
    return bottom;
}

void ControlPage::setGroupVisible(int groupIndex, bool visible)
{
    if (groupIndex < 0 || groupIndex >= static_cast<int>(m_groups.size())) return;
    Group& g = m_groups[static_cast<size_t>(groupIndex)];
    if (g.m_off == !visible) return;
    g.m_off = !visible;
    applySections();
    layout(getWidth());
    repaint();
}

int ControlPage::place(int usable)
{
    for (Group& g : m_groups) {
        if (g.m_hidden) { g.m_bounds = {}; continue; }
        if (g.m_fill) {
            // As wide as the page, in whole grid columns; its control (a pattern roll) takes all of them.
            g.m_extraColumns = juce::jmax(0, (usable - 2 * GroupPad) / CellW - g.m_columns);
            for (int ci : g.m_cells) m_cells[static_cast<size_t>(ci)].m_gridW = 2 * (g.m_columns + g.m_extraColumns);
        }
        const int bottom = pack(g, 2 * (g.m_columns + g.m_extraColumns), true);
        g.m_rows = juce::jmax(1, (bottom + 2) / 3);
        const int gpw = (g.m_columns + g.m_extraColumns) * CellW + 2 * GroupPad;
        const int gph = GroupTitleH + juce::jmax(2, bottom) * RowU + GroupPad;
        g.m_bounds = juce::Rectangle<int>(0, 0, gpw, gph);
    }

    // On the page, a skyline (26.09.2026): each group, in order, goes where the page is lowest across its width --
    // the leftmost of the lowest -- instead of into rows as tall as their tallest group, so a short group no
    // longer leaves a gap under itself as wide as it is.
    std::vector<juce::Rectangle<int>> placed;
    int contentBottom = PagePad + topBar();
    for (Group& g : m_groups) {
        if (g.m_hidden) continue;
        const int gw = juce::jmin(g.m_bounds.getWidth(), usable);
        std::vector<int> xs { PagePad };
        for (const auto& r : placed)
            if (r.getRight() + GroupGap + gw <= PagePad + usable) xs.push_back(r.getRight() + GroupGap);
        int bestX = PagePad, bestY = std::numeric_limits<int>::max();
        for (int x : xs) {
            int y = PagePad + topBar();
            for (const auto& r : placed)
                if (r.getX() < x + gw && x < r.getRight()) y = juce::jmax(y, r.getBottom() + GroupGap);
            if (y < bestY || (y == bestY && x < bestX)) { bestY = y; bestX = x; }
        }
        g.m_bounds.setPosition(bestX, bestY);
        placed.push_back(g.m_bounds);
        contentBottom = juce::jmax(contentBottom, g.m_bounds.getBottom());
        for (int ci : g.m_cells)
            m_cells[static_cast<size_t>(ci)].m_bounds.translate(g.m_bounds.getX() + GroupPad, g.m_bounds.getY() + GroupTitleH);
    }
    return contentBottom;
}

void ControlPage::enableSections(const frame::Skin& skin, std::function<void()> changed)
{
    m_skin = &skin;
    m_sectionChanged = std::move(changed);
    m_plannedWidth = -1;
}

void ControlPage::plan(int usable)
{
    if (m_skin == nullptr || (usable == m_plannedWidth && m_available == m_plannedAvailable)) return;
    m_plannedWidth = usable;
    m_plannedAvailable = m_available;
    juce::StringArray titles;
    for (const Group& g : m_groups) titles.add(g.m_title);
    // A section's height as the page places it: its groups alone (those the editor hides stay hidden), the switch's
    // row, the margin.
    auto height = [this, usable](const std::vector<int>& set) {
        m_planning = true;
        for (size_t i = 0; i < m_groups.size(); ++i)
            m_groups[i].m_hidden = m_groups[i].m_off || std::find(set.begin(), set.end(), static_cast<int>(i)) == set.end();
        const int h = arrange(usable);   // measured as the page lays it out, the groups that grow included
        m_planning = false;
        return h;
    };
    const frame::SectionPlan p = frame::planSections(titles, m_available, height);
    m_sectionOf.assign(m_groups.size(), 0);
    for (size_t s = 0; s < p.m_groups.size(); ++s)
        for (int gi : p.m_groups[s]) m_sectionOf[static_cast<size_t>(gi)] = static_cast<int>(s);
    if (p.m_groups.size() > 1) {
        if (m_sections == nullptr) {
            m_sections = std::make_unique<frame::SectionSwitch>(*m_skin);
            m_sections->m_onChange = [this](int) {
                applySections();
                if (m_sectionChanged) m_sectionChanged();
            };
            addChildComponent(*m_sections);
        }
        m_sections->setNames(p.m_names);
        m_sections->setVisible(true);
    } else if (m_sections != nullptr) {
        m_sections->setVisible(false);
    }
    applySections();
}

void ControlPage::applySections()
{
    const int shownSection = split() ? m_sections->current() : -1;
    for (size_t i = 0; i < m_groups.size(); ++i) {
        Group& g = m_groups[i];
        const bool inSection = shownSection < 0 || (i < m_sectionOf.size() && m_sectionOf[i] == shownSection);
        g.m_hidden = g.m_off || !inSection;
        for (int ci : g.m_cells) {
            Cell& c = m_cells[static_cast<size_t>(ci)];
            if (c.m_comp != nullptr) c.m_comp->setVisible(!g.m_hidden);
            if (c.m_label != nullptr) c.m_label->setVisible(!g.m_hidden);
        }
    }
    repaint();
}

void ControlPage::resized()
{
    if (m_sections != nullptr) {
        const int w = m_sections->bestWidth();
        m_sections->setBounds(getWidth() - PagePad - w, PagePad - 4, w, 26);
    }
    for (Cell& c : m_cells) {
        if (c.m_comp == nullptr) continue;
        juce::Rectangle<int> r = c.m_bounds.reduced(3, 3);
        if (c.m_tall || c.m_label == nullptr) {
            c.m_comp->setBounds(r);
            if (c.m_label != nullptr) c.m_label->setBounds(juce::Rectangle<int>());
            continue;
        }
        const int labelH = c.m_size == CellSizeE::LARGE ? LabelH + 3 : (c.m_size == CellSizeE::SMALL ? LabelH - 2 : LabelH);
        juce::Rectangle<int> labelArea = r.removeFromBottom(labelH);
        if (dynamic_cast<juce::Slider*>(c.m_comp.get()) != nullptr) {
            // A knob is round: keep it square and centred in what is left.
            const int side = juce::jmin(r.getWidth(), r.getHeight());
            c.m_comp->setBounds(juce::Rectangle<int>(side, side).withCentre(r.getCentre()));
        } else {
            const int h = juce::jmin(r.getHeight(), 26);
            c.m_comp->setBounds(r.withSizeKeepingCentre(r.getWidth(), h));
        }
        c.m_label->setBounds(labelArea);
    }
}

void ControlPage::paint(juce::Graphics& g)
{
    // The group boxes as every generator draws them (01.10.2026, the frame): the box, its title in its family's colour
    // and a hairline under it.
    for (const Group& grp : m_groups) {
        if (grp.m_hidden) continue;
        const juce::Rectangle<float> r = grp.m_bounds.toFloat();
        g.setColour(phosui::GroupColour);
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(Edge);
        g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);
        g.setColour(grp.m_tint);
        g.setFont(title(11.0f));
        g.drawText(grp.m_title.toUpperCase(), grp.m_bounds.withHeight(GroupTitleH).reduced(GroupPad + 1, 2),
                   juce::Justification::centredLeft, false);
        g.setColour(grp.m_tint.withAlpha(0.45f));
        g.fillRect(r.getX() + GroupPad, r.getY() + static_cast<float>(GroupTitleH) - 2.0f, r.getWidth() - 2.0f * GroupPad, 1.0f);
    }
}

} // namespace phosui
