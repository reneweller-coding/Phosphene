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

constexpr int kCellW = 76;        ///< width of one cell unit
constexpr int kCellH = 84;        ///< height of a cell: knob plus its name
constexpr int kLabelH = 15;       ///< the name under the knob
constexpr int kGroupPad = 9;      ///< inside a group box
constexpr int kGroupTitleH = 21;  ///< the group's title bar
constexpr int kGroupGap = 9;      ///< between group boxes
constexpr int kPagePad = 12;      ///< around the page
/**
 * @name The grid inside a group (26.09.2026)
 * The user: "wichtigere Encoder größer machen (sowas wie Cutoff im Filter ...) und die Funktionsgruppen sinnvoll und
 * platzsparend, ohne zu große Lücken, anordnen". A cell used to be one kCellW by kCellH box; the grid is now a half
 * cell wide and a third of one tall, so that three sizes of control fit together: a normal knob is 2 x 3 of these,
 * a large one 4 x 5 (its knob nearly twice as wide), a small one -- sends, fine settings -- and a switch or a
 * chooser 2 (or 4) x 2. Widths and heights handed in as cell units (Group::columns, addControl) mean normal cells.
 * @{ */
constexpr int kColU = kCellW / 2;  ///< grid column, pixels
constexpr int kRowU = kCellH / 3;  ///< grid row, pixels
/** @} */

/** @brief How large a parameter's control is drawn (26.09.2026). */
enum class CellSize { Small, Normal, Large };
/** @brief A parameter and the size of its control, for ControlPage::addSizedGroup. */
struct SizedParam {
    int id = -1;                        ///< global parameter id
    CellSize size = CellSize::Normal;   ///< how large
};

/** @brief One control and the name under it. */
struct Cell {
    int param = -1;                                   ///< global parameter id, -1 for an added control
    std::vector<int> bound;                           ///< the parameters an added control drives (addControl)
    int units = 1;                                    ///< width in cell units
    int heightRows = 1;                               ///< height in cell rows (added controls)
    CellSize size = CellSize::Normal;                 ///< a parameter's control: small, normal or large
    int gridW = 2, gridH = 3;                         ///< its footprint in grid units (kColU x kRowU), set when added
    bool tall = false;                                ///< occupies the whole cell height (no label line)
    std::unique_ptr<juce::Component> comp;            ///< the control
    std::unique_ptr<juce::Label> label;               ///< its name, under it
    std::unique_ptr<juce::SliderParameterAttachment> slider;    ///< host link for a knob
    std::unique_ptr<juce::ButtonParameterAttachment> button;    ///< host link for a switch
    std::unique_ptr<juce::ComboBoxParameterAttachment> combo;   ///< host link for a chooser
    juce::Rectangle<int> bounds;                      ///< filled by the layout pass
};

/** @brief A titled box of cells. */
struct Group {
    juce::String title;             ///< shown in the box's title bar
    juce::Colour tint;              ///< the page's colour, for the title
    int columns = 5;                ///< cell units per row
    std::vector<int> cells;         ///< indices into ControlPage::cells_
    juce::Rectangle<int> bounds;    ///< filled by the layout pass
    int rows = 1;                   ///< filled by the layout pass
    int extraColumns = 0;           ///< columns the layout pass added where the page had room (26.09.2026)
    bool fill = false;              ///< spans the page's width, its one control with it (setFillWidth)
    bool hidden = false;            ///< left out of the layout and not drawn (setGroupVisible, or another section)
    bool off = false;               ///< hidden by setGroupVisible, in every section
};

/**
 * @brief A page of groups of controls.
 *
 * The page owns its components. layout() is a pure measurement: it fills every Group::bounds and
 * Cell::bounds from the width it is given and returns the height that arrangement needs, so the
 * editor can ask a page how tall it wants to be before deciding on the window's design size.
 */
class ControlPage : public juce::Component {
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
    int addModuleGroup(PhospheneProcessor& proc, phos::Module module, int instance, const juce::String& title,
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
        if (groupIndex >= 0 && groupIndex < static_cast<int>(groups_.size())) groups_[static_cast<size_t>(groupIndex)].fill = true;
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
    void setAvailableHeight(int h) { available_ = h; }
    /** @brief Measures and places everything for a page @p width; returns the height it needs. */
    int layout(int width);
    /** @brief The height the last layout() needed. */
    int contentHeight() const { return contentHeight_; }
    /** @brief The narrowest width that still lets the widest group fit. */
    int minimumWidth() const;

    /** @name What the page ended up holding
     *  The manual generator reads this back: a page is asked which groups it built and which
     *  parameters went into them, so the manual is the layout rather than a description of it, and
     *  a parameter that no group claimed shows up as a hole.
     *  @{ */
    int groupCount() const { return static_cast<int>(groups_.size()); }   ///< number of groups
    const juce::String& groupTitle(int index) const { return groups_[static_cast<size_t>(index)].title; }
    /** @brief The global parameter ids of a group, in the order they are drawn (an added control's bound ids with it). */
    std::vector<int> groupParams(int index) const;
    /** @} */

    void resized() override;
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

private:
    /** @brief Packs every group's cells and places the groups on the page (one pass of layout()); returns the bottom. */
    int place(int usable);
    /** @brief The whole arrangement for @p usable: the groups grown into the room beside them, then placed; returns the height. */
    int arrange(int usable);
    /** @brief Packs @p g's cells into @p gridCols grid columns; returns the rows used; @p apply writes the cells' bounds. */
    int pack(Group& g, int gridCols, bool apply);
    /** @brief Builds the control a descriptor asks for -- knob, switch or chooser -- and attaches it. */
    void addParamCell(PhospheneProcessor& proc, int groupIndex, int paramId, CellSize size = CellSize::Normal);
    PhospheneProcessor* learnProc_ = nullptr;                    ///< set by the first enableMidiLearn
    std::vector<std::pair<juce::Component*, int>> learnTargets_; ///< control -> target
    std::vector<Cell> cells_;    ///< every control on the page, in the order it was added
    std::vector<Group> groups_;  ///< the boxes, each naming the cells that belong to it
    int contentHeight_ = 0;      ///< what the last layout() needed
    const frame::Skin* skin_ = nullptr;                ///< set by enableSections: the page may have sections
    std::unique_ptr<frame::SectionSwitch> sections_;   ///< the switch, while the page has more than one section
    std::vector<int> sectionOf_;                       ///< per group its section
    std::function<void()> sectionChanged_;
    int available_ = 0, plannedWidth_ = -1, plannedAvailable_ = -1;
    bool planning_ = false;                            ///< measuring: the switch's row counted in
    void plan(int usable);                             ///< the sections for this width and available_
    void applySections();                              ///< the groups of the section shown, the others hidden
    bool split() const { return sections_ != nullptr && sections_->isVisible(); }
    int topBar() const { return planning_ || split() ? 32 : 0; }   ///< room for the switch above the groups
};

} // namespace phosui
