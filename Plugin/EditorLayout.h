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
#include "PluginProcessor.h"
#include "phos/Params.h"
#include <memory>
#include <vector>

namespace phosui {

constexpr int kCellW = 76;        ///< width of one cell unit
constexpr int kCellH = 84;        ///< height of a cell: knob plus its name
constexpr int kLabelH = 15;       ///< the name under the knob
constexpr int kGroupPad = 9;      ///< inside a group box
constexpr int kGroupTitleH = 21;  ///< the group's title bar
constexpr int kGroupGap = 9;      ///< between group boxes
constexpr int kPagePad = 12;      ///< around the page

/** @brief One control and the name under it. */
struct Cell {
    int param = -1;                                   ///< global parameter id, -1 for an added control
    int units = 1;                                    ///< width in cell units
    int heightRows = 1;                               ///< height in cell rows; > 1 takes a band of its own
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
     * @return the cell's index
     */
    int addControl(int groupIndex, std::unique_ptr<juce::Component> comp, const juce::String& name, int units = 1,
                   bool tall = false, int rows = 1);

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
    /** @brief The global parameter ids of a group, in the order they are drawn (-1 cells left out). */
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
    /** @brief The right click of enableMidiLearn (the page listens to its controls' mouse). */
    void mouseDown(const juce::MouseEvent& e) override;

private:
    /** @brief Builds the control a descriptor asks for -- knob, switch or chooser -- and attaches it. */
    void addParamCell(PhospheneProcessor& proc, int groupIndex, int paramId);
    PhospheneProcessor* learnProc_ = nullptr;                    ///< set by the first enableMidiLearn
    std::vector<std::pair<juce::Component*, int>> learnTargets_; ///< control -> target
    std::vector<Cell> cells_;    ///< every control on the page, in the order it was added
    std::vector<Group> groups_;  ///< the boxes, each naming the cells that belong to it
    int contentHeight_ = 0;      ///< what the last layout() needed
};

} // namespace phosui
