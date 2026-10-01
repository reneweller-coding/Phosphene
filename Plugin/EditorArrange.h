/**
 * @file EditorArrange.h
 * @brief The arrange timeline: the whole planned set on one page, and the curation loop on top of it.
 *
 * PLAN 8.1 asks the Arrange tab for "the set's timeline: tracks, sections as blocks, locks, rerolls
 * per block, jump to section, markers". The hard part is the scale. A sixty-minute set is about 2300
 * bars, nine tracks and a hundred sections; drawn as one ruler a sixteen-bar section is eight pixels
 * wide and nothing can be read or hit. So the view has two levels, and both are measured rather than
 * placed:
 *
 *  - the **set strip** across the top, where one pixel is many bars: the tracks as blocks in the
 *    order they play, with the energy arc of the whole night drawn over them and the play head in it.
 *    This is the map -- where am I in the night, what is left.
 *  - the **track rows** underneath, one per track, each row giving its own track the full width. A
 *    track of eight sections therefore gets about 140 pixels per section whatever the set's length,
 *    which is enough for the section's name, its lock and its reroll.
 *
 * **The DJ overlap** (Form.h, kDjOverlap; 19.09.2026): track N+1 starts sixteen bars before track N
 * ends, so two tracks sound at once and their blocks share bars. Drawn in one lane the later block
 * covered the end of the earlier one and the strip read as back-to-back tracks with a wrong length. The
 * strip therefore has **two lanes**, odd tracks above and even ones below, so every block keeps its full
 * length and the overlap is where the two lanes run side by side (marked with a crossfade). In the
 * track rows, where each track fills the width on its own, the bars it shares with its neighbours are
 * hatched and say which track they mix with.
 *
 * Nothing here talks to phos::Composer. The editor copies the published plans (which the composer
 * thread publishes as it goes) into a Snapshot on the message thread, and the component draws that.
 * The static picture is drawn once into an image and kept; a repaint moves the play head over it. A
 * timeline that redrew a hundred blocks and three hundred glyphs at every tick would be a bug.
 */
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "phos/Composer.h"
#include "phos/Form.h"
#include "phos/Score.h"
#include <functional>
#include <utility>
#include <vector>

/** @brief The planned set, drawn. */
class ArrangeDisplay final : public juce::Component {
public:
    /** @brief One section of a track as the editor copied it out of the plan. */
    struct Sec {
        int   index = 0;      ///< section number within the track (the lockable unit)
        int   bar = 0;        ///< first bar, from the start of the set
        int   bars = 0;       ///< length
        phos::SectionType type = phos::SectionType::Groove;   ///< what kind it is
        float energy = 0.5f;  ///< energy at its start
        float energyTo = 0.5f;   ///< energy at its end (a buildup rises)
        bool  locked = false; ///< the curation lock
        uint32_t variation = 0;   ///< how often it has been rerolled
    };
    /** @brief One track of the set. */
    struct Trk {
        int    index = 0;        ///< track number
        int    firstBar = 0;     ///< where it starts in the set
        int    bars = 0;         ///< how long it is
        int    key = 6;          ///< pitch class
        int    scale = 1;        ///< index into kScaleNames
        double bpm = 145.0;      ///< tempo
        float  gainDb = 0.0f;    ///< level match
        bool   locked = false;   ///< the curation lock
        uint32_t variation = 0;  ///< how often it has been rerolled
        std::vector<Sec> sections;   ///< its form
    };
    /** @brief What the editor read out of the published plans. */
    struct Snapshot {
        std::vector<Trk> tracks;   ///< the tracks that have been planned so far
        int bars = 0;              ///< bars they cover
        int minutes = 0;           ///< the set length the knobs ask for, for the caption
    };

    /** @brief An empty timeline; update() fills it. */
    ArrangeDisplay();
    /**
     * @brief The set strip alone (01.10.2026, the frame's overview under the header, as every generator has one): no
     *        track rows, no legend; a click jumps.
     */
    void setStripOnly(bool on) { stripOnly_ = on; }

    /** @brief Replaces what is drawn (message thread); the picture is redrawn, not the play head. */
    void setSnapshot(Snapshot s);
    /** @brief Moves the play head. Returns true when it moved far enough to be worth a repaint. */
    bool setPosition(double musicalBeat);
    /** @brief The snapshot that is on screen, so the editor can compare before replacing it. */
    const Snapshot& snapshot() const { return snap_; }
    /** @brief Where track @p i sits in the set strip (empty before the first measure); for the host test. */
    juce::Rectangle<int> trackBlock(size_t i) const { return i < blocks_.size() ? blocks_[i] : juce::Rectangle<int>(); }
    /** @brief Bars track @p i shares with the one before (first) and the one after it (second); 0 = none. */
    std::pair<int, int> overlapOf(size_t i) const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    std::function<void(int)> onSeek;                                  ///< called with a bar to jump to
    std::function<void(phos::LockUnit, int, bool)> onLock;            ///< unit, index, the new state
    std::function<void(phos::LockUnit, int)> onReroll;                ///< unit, index

private:
    /** @brief A rectangle that can be clicked, and what clicking it means. */
    struct Hit {
        juce::Rectangle<int> area;   ///< where it is
        int  bar = -1;               ///< seek target, -1 = none
        phos::LockUnit unit = phos::LockUnit::Track;   ///< which unit the two buttons act on
        int  index = 0;              ///< its index
        int  action = 0;             ///< 0 seek, 1 toggle the lock, 2 reroll
        bool locked = false;         ///< state of the lock, for the toggle
    };

    /** @brief Measures the strip, the rows and every clickable rectangle from the snapshot. */
    void measure();
    /** @brief Draws everything that does not move into #cache_. */
    void render();
    /** @brief x of a bar inside the set strip. */
    float stripX(double bar) const;

    Snapshot snap_;                  ///< what is drawn
    std::vector<Hit> hits_;          ///< filled by measure(), scanned by mouseDown()
    juce::Rectangle<int> strip_;     ///< the set-wide bar at the top
    std::vector<juce::Rectangle<int>> blocks_;    ///< each track's block in the strip, in its lane
    std::vector<juce::Rectangle<int>> rowArea_;   ///< the part of each row the sections live in
    juce::Image cache_;              ///< the static picture at the current size
    double beat_ = 0.0;              ///< the play head, in beats from the start of the set
    int headPixel_ = -1;             ///< where it was last drawn, so a repaint is worth it
    int totalBars_ = 1;              ///< bars the strip spans
    bool stripOnly_ = false;         ///< setStripOnly
};
