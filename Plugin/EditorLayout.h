/**
 * @file EditorLayout.h
 * @brief The layout engine: a page of controls built from the parameter tables, never placed by hand.
 *
 * A control is not written down anywhere. A page is told "these parameters of this module instance,
 * under this title, this many cells wide", and it builds a knob, a switch or a chooser from the
 * descriptor (phos::ParamDesc::curve decides which), names it from the descriptor and attaches it to
 * the host parameter of the same id. The arrangement is then measured, not typed: cells flow into
 * rows inside their group, groups flow into rows inside the page, and the page reports the height it
 * needs. Adding a parameter to a module table therefore adds a control to the editor, and moving one
 * moves it -- which is the lesson Noctuary's editor was rebuilt around.
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Frame.h"
#include "PluginProcessor.h"
#include "phos/Params.h"
#include <memory>
#include <vector>

namespace phosui {

/**
 * @brief The live ring (26.09.2026, the user's choice "Preset absolut + Live-Ring"): hands a knob or a fader the value its
 *        parameter plays at, in the control's own units. PhospheneLookAndFeel draws it where it differs from the control:
 *        the composer's rides, a section's lift, the level match's correction of a fader. Repaints only on a change.
 */
inline void showLive(juce::Slider& s, double played)
{
    // Since 01.10.2026 normalised, as the frame's look and feel reads it (Frame.h, LiveRings).
    auto& props = s.getProperties();
    const double norm = s.valueToProportionOfLength(juce::jlimit(s.getMinimum(), s.getMaximum(), played));
    const juce::var old = props["live"];
    if (!old.isVoid() && std::abs(static_cast<double>(old) - norm) <= 1.0e-4) return;
    props.set("live", norm);
    s.repaint();
}

constexpr int CellW = 76;        ///< width of one cell unit
constexpr int CellH = 84;        ///< height of a cell: knob plus its name
constexpr int LabelH = 15;       ///< the name under the knob
constexpr int GroupPad = 9;      ///< inside a group box
constexpr int GroupTitleH = 21;  ///< the group's title bar
constexpr int GroupGap = 9;      ///< between group boxes
constexpr int PagePad = 12;      ///< around the page
/**
 * @name The grid inside a group (26.09.2026)
 * The user: "wichtigere Encoder größer machen (sowas wie Cutoff im Filter ...) und die Funktionsgruppen sinnvoll und
 * platzsparend, ohne zu große Lücken, anordnen". A cell used to be one CellW by CellH box; the grid is now a half
 * cell wide and a third of one tall, so that three sizes of control fit together: a normal knob is 2 x 3 of these,
 * a large one 4 x 5 (its knob nearly twice as wide), a small one -- sends, fine settings -- and a switch or a
 * chooser 2 (or 4) x 2. Widths and heights handed in as cell units (Group::columns, addControl) mean normal cells.
 * @{ */
constexpr int ColU = CellW / 2;  ///< grid column, pixels
constexpr int RowU = CellH / 3;  ///< grid row, pixels
/** @} */

/** @brief How large a parameter's control is drawn (26.09.2026). */
enum class CellSizeE { SMALL, NORMAL, LARGE };
/** @brief A parameter and the size of its control, for ControlPage::addSizedGroup. */
struct SizedParam {
    int m_id = -1;                        ///< global parameter id
    CellSizeE m_size = CellSizeE::NORMAL;   ///< how large
};

/** @brief One control and the name under it. */
struct Cell {
    int m_param = -1;                                   ///< global parameter id, -1 for an added control
    std::vector<int> m_bound;                           ///< the parameters an added control drives (addControl)
    int m_units = 1;                                    ///< width in cell units
    int m_heightRows = 1;                               ///< height in cell rows (added controls)
    CellSizeE m_size = CellSizeE::NORMAL;                 ///< a parameter's control: small, normal or large
    int m_gridW = 2;   ///< its footprint in grid units (ColU): width
    int m_gridH = 3;   ///< ... height (RowU)
    bool m_tall = false;                                ///< occupies the whole cell height (no label line)
    std::unique_ptr<juce::Component> m_comp;            ///< the control
    std::unique_ptr<juce::Label> m_label;               ///< its name, under it
    std::unique_ptr<juce::SliderParameterAttachment> m_slider;    ///< host link for a knob
    std::unique_ptr<juce::ButtonParameterAttachment> m_button;    ///< host link for a switch
    std::unique_ptr<juce::ComboBoxParameterAttachment> m_combo;   ///< host link for a chooser
    juce::Rectangle<int> m_bounds;                      ///< filled by the layout pass
};

/** @brief A titled box of cells. */
struct Group {
    juce::String m_title;             ///< shown in the box's title bar
    juce::Colour m_tint;              ///< the page's colour, for the title
    int m_columns = 5;                ///< cell units per row
    std::vector<int> m_cells;         ///< indices into ControlPage::m_cells
    juce::Rectangle<int> m_bounds;    ///< filled by the layout pass
    int m_rows = 1;                   ///< filled by the layout pass
    int m_extraColumns = 0;           ///< columns the layout pass added where the page had room (26.09.2026)
    bool m_fill = false;              ///< spans the page's width, its one control with it (setFillWidth)
    bool m_hidden = false;            ///< left out of the layout and not drawn (setGroupVisible, or another section)
    bool m_off = false;               ///< hidden by setGroupVisible, in every section
};

/**
 * @brief A page of groups of controls.
 *
 * The page owns its components. layout() is a pure measurement: it fills every Group::bounds and
 * Cell::bounds from the width it is given and returns the height that arrangement needs, so the
 * editor can ask a page how tall it wants to be before deciding on the window's design size.
 */
class ControlPage : public juce::Component, public frame::Revealer {
public:
    /** @brief An empty page; the add* calls fill it. */
    ControlPage() = default;

    /**
     * @brief Adds a group holding a slice of a module instance's parameter table.
     * @param proc     the processor (for the host parameters and the store)
     * @param module   which module
     * @param instance which instance of it
     * @param title    the group's title
     * @param tint     the page's colour
     * @param columns  cell units per row
     * @param first    first index inside the module's table
     * @param count    how many entries, -1 = to the end of the table
     * @return the group's index
     */
    int addModuleGroup(PhospheneProcessor& proc, phos::ModuleE module, int instance, const juce::String& title,
                       juce::Colour tint, int columns, int first = 0, int count = -1);
    /**
     * @brief Adds a group of parameters named one by one (23.09.2026: the Set tab groups the composer's knobs by
     *        what they decide, not by where they were appended to the table).
     * @param paramIds global parameter ids, in the order they are drawn
     * @return the group's index
     * @param proc    the processor whose parameters the controls edit
     * @param title   the group's caption
     * @param tint    the group's accent colour
     * @param columns controls per row
     */
    int addParamsGroup(PhospheneProcessor& proc, const juce::String& title, juce::Colour tint, int columns,
                       const std::vector<int>& paramIds);
    /**
     * @brief Adds a group of parameters each with the size of its control (26.09.2026: the synth pages).
     * @param proc the processor; @param title the caption; @param tint the page's colour
     * @param columns the group's width in normal cells; @param params the parameters, in the order they are placed
     * @return the group's index
     */
    int addSizedGroup(PhospheneProcessor& proc, const juce::String& title, juce::Colour tint, int columns,
                      const std::vector<SizedParam>& params);
    /** @brief Adds an empty group for controls that are not parameters. */
    int addGroup(const juce::String& title, juce::Colour tint, int columns);
    /**
     * @brief Puts a control of the editor's own into a group.
     * @param groupIndex index from addGroup()
     * @param comp  the control (the page takes it over)
     * @param name  the name under it, empty for none
     * @param units width in cell units
     * @param tall  use the whole cell height instead of leaving room for the name
     * @param rows  height in cell rows; more than one puts the control in a band of its own
     * @param bound the global ids of the parameters the control drives (24.09.2026: the mixer's console, the
     *              effect preset choosers), so groupParams() -- the manual, the help, the host test's "every
     *              parameter stands on a page" -- sees them
     * @return the cell's index
     */
    int addControl(int groupIndex, std::unique_ptr<juce::Component> comp, const juce::String& name, int units = 1,
                   bool tall = false, int rows = 1, const std::vector<int>& bound = {});

    /** @brief The group @p groupIndex spans the page's whole width, and so does the control in it (the pattern rolls). */
    void setFillWidth(int groupIndex)
    {
        if (groupIndex >= 0 && groupIndex < static_cast<int>(m_groups.size())) m_groups[static_cast<size_t>(groupIndex)].m_fill = true;
    }
    /** @brief Shows or hides group @p groupIndex and its controls (01.10.2026: the headset's, while one sends). */
    void setGroupVisible(int groupIndex, bool visible);
    /**
     * @brief A page taller than its window in sections, one shown at a time (01.10.2026, the family's Frame.h,
     *        planSections): the sound and the modulation apart, each cut where the window ends; a switch at the page's
     *        top right. @p changed runs after a switch (the editor lays the page out again).
     */
    void enableSections(const frame::Skin& skin, std::function<void()> changed);
    /** @brief The height the page has in its window (the editor says so before layout()). */
    void setAvailableHeight(int h) { m_available = h; }
    /** @brief Measures and places everything for a page @p width; returns the height it needs. */
    int layout(int width);
    /** @brief The height the last layout() needed. */
    int contentHeight() const { return m_contentHeight; }
    /** @brief The narrowest width that still lets the widest group fit. */
    int minimumWidth() const;

    /** @name What the page ended up holding
     *  The manual generator reads this back: a page is asked which groups it built and which
     *  parameters went into them, so the manual is the layout rather than a description of it, and
     *  a parameter that no group claimed shows up as a hole.
     *  @{ */
    int groupCount() const { return static_cast<int>(m_groups.size()); }   ///< number of groups
    const juce::String& groupTitle(int index) const { return m_groups[static_cast<size_t>(index)].m_title; }
    /** @brief The global parameter ids of a group, in the order they are drawn (an added control's bound ids with it). */
    std::vector<int> groupParams(int index) const;
    /** @} */

    /** @brief Lays the groups out (in sections where the window is too low). */
    void resized() override;
    /** @brief The groups' boxes and titles. */
    void paint(juce::Graphics&) override;

    /**
     * @brief MIDI learn on a control (23.09.2026, phos/MidiMap.h): a right click on @p comp offers "MIDI Learn"
     *        and "Forget MIDI" for @p target (PhospheneProcessor's target numbering). Every parameter cell gets
     *        it by itself; the editor calls it for its own controls that drive a target (the macros).
     */
    void enableMidiLearn(PhospheneProcessor& proc, juce::Component& comp, int target);
    /**
     * @brief Shows @p values on the controls of the parameters [@p first, @p first + @p count) without writing them
     *        (26.09.2026: the composer's preset of a synth; the attachment writes a value only when the user moves the
     *        control). A parameter without a value -- or all of them, @p values null -- shows its own value again.
     * @param store the parameters (the value of a control @p values does not name)
     * @param first the first parameter of the synth
     * @param count how many parameters the synth has
     * @param values (index from @p first, value) pairs, or null
     */
    void showValues(const phos::ParamStore& store, int first, int count, const std::vector<std::pair<int, float>>* values);
    /** @brief Gives every knob of the page the value its parameter plays at (the live ring; showLive). */
    void showPlayed(const PhospheneProcessor& proc);
    /** @brief The right click of enableMidiLearn (the page listens to its controls' mouse). */
    void mouseDown(const juce::MouseEvent& e) override;
    /** @brief Shows the section holding @p part (the search found it there; 15.10.2026). */
    void reveal(juce::Component& part) override;
    /** @brief The name under cell @p cell anew (the preset chooser's star). */
    void setCellName(int cell, const juce::String& name);
    /** @brief The modulation at the knob (15.10.2026; the editor's): the right-click menu's rows and the arcs. */
    frame::ModulationAtKnob* m_modulation = nullptr;

private:
    /** @brief Packs every group's cells and places the groups on the page (one pass of layout()); returns the bottom. */
    int place(int usable);
    /** @brief The whole arrangement for @p usable: the groups grown into the room beside them, then placed; returns the height. */
    int arrange(int usable);
    /** @brief Packs @p g's cells into @p gridCols grid columns; returns the rows used; @p apply writes the cells' bounds. */
    int pack(Group& g, int gridCols, bool apply);
    /** @brief Builds the control a descriptor asks for -- knob, switch or chooser -- and attaches it. */
    void addParamCell(PhospheneProcessor& proc, int groupIndex, int paramId, CellSizeE size = CellSizeE::NORMAL);
    PhospheneProcessor* m_learnProc = nullptr;                    ///< set by the first enableMidiLearn
    std::vector<std::pair<juce::Component*, int>> m_learnTargets; ///< control -> target
    std::vector<Cell> m_cells;    ///< every control on the page, in the order it was added
    std::vector<Group> m_groups;  ///< the boxes, each naming the cells that belong to it
    int m_contentHeight = 0;      ///< what the last layout() needed
    const frame::Skin* m_skin = nullptr;                ///< set by enableSections: the page may have sections
    std::unique_ptr<frame::SectionSwitch> m_sections;   ///< the switch, while the page has more than one section
    std::vector<int> m_sectionOf;                       ///< per group its section
    std::function<void()> m_sectionChanged;   ///< called when another section is chosen
    int m_available = 0;   ///< the height of the window (setAvailableHeight)
    int m_plannedWidth = -1;   ///< the width the sections were planned for, -1 never
    int m_plannedAvailable = -1;   ///< the height the sections were planned for, -1 never
    bool m_planning = false;                            ///< measuring: the switch's row counted in
    void plan(int usable);                             ///< the sections for this width and m_available
    void applySections();                              ///< the groups of the section shown, the others hidden
    /** @brief Whether the page shows its groups in sections. */
    bool split() const { return m_sections != nullptr && m_sections->isVisible(); }
    int topBar() const { return m_planning || split() ? 32 : 0; }   ///< room for the switch above the groups
};

} // namespace phosui
