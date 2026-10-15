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
 * **The DJ overlap** (Form.h, DjOverlap; 19.09.2026): track N+1 starts sixteen bars before track N
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
 * @author Rene Weller
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
class ArrangeDisplay final : public juce::Component
{
public:
    /** @brief One section of a track as the editor copied it out of the plan. */
    struct Sec
    {
        int                m_index = 0;   ///< section number within the track (the lockable unit)
        int                m_bar = 0;     ///< first bar, from the start of the set
        int                m_bars = 0;    ///< length
        phos::SectionTypeE m_type = phos::SectionTypeE::GROOVE;   ///< what kind it is
        float              m_energy = 0.5f;                       ///< energy at its start
        float              m_energyTo = 0.5f;                     ///< energy at its end (a buildup rises)
        bool               m_locked = false;                      ///< the curation lock
        uint32_t           m_variation = 0;                       ///< how often it has been rerolled
    };
    /** @brief One track of the set. */
    struct Trk
    {
        int              m_index = 0;        ///< track number
        int              m_firstBar = 0;     ///< where it starts in the set
        int              m_bars = 0;         ///< how long it is
        int              m_key = 6;          ///< pitch class
        int              m_scale = 1;        ///< index into ScaleNames
        double           m_bpm = 145.0;      ///< tempo
        float            m_gainDb = 0.0f;    ///< level match
        bool             m_locked = false;   ///< the curation lock
        uint32_t         m_variation = 0;    ///< how often it has been rerolled
        std::vector<Sec> m_sections;         ///< its form
    };
    /** @brief What the editor read out of the published plans. */
    struct Snapshot
    {
        std::vector<Trk> m_tracks;        ///< the tracks that have been planned so far
        int              m_bars = 0;      ///< bars they cover
        int              m_minutes = 0;   ///< the set length the knobs ask for, for the caption
    };

    /** @brief An empty timeline; update() fills it. */
    ArrangeDisplay();
    /**
     * @brief The set strip alone (01.10.2026, the frame's overview under the header, as every generator has one): no
     *        track rows, no legend; a click jumps.
     * @param[in] on  true for the strip alone
     */
    void setStripOnly( bool on ) { m_stripOnly = on; }

    /**
     * @brief Replaces what is drawn (message thread); the picture is redrawn, not the play head.
     * @param[in] s  the snapshot
     * @pre called on the message thread
     */
    void setSnapshot( Snapshot s );
    /**
     * @brief Moves the play head. Returns true when it moved far enough to be worth a repaint.
     * @param[in] musicalBeat  the beat
     * @return true when worth a repaint
     */
    bool setPosition( double musicalBeat );
    /**
     * @brief The snapshot that is on screen, so the editor can compare before replacing it.
     * @return the snapshot
     */
    const Snapshot &snapshot() const { return m_snap; }
    /**
     * @brief Where track @p i sits in the set strip (empty before the first measure); for the host test.
     * @param[in] i  the track
     * @return its rectangle
     */
    juce::Rectangle<int> trackBlock( size_t i ) const
    {
        return i < m_blocks.size() ? m_blocks[i] : juce::Rectangle<int>();
    }
    /**
     * @brief Bars track @p i shares with the one before (first) and the one after it (second); 0 = none.
     * @param[in] i  the track
     * @return (bars before, bars after)
     */
    std::pair<int, int> overlapOf( size_t i ) const;

    /**
     * @brief The set strip, the tracks and their sections, the play head.
     * @param[in,out] g  the graphics context
     */
    void paint( juce::Graphics & ) override;
    /** @brief Measures the blocks for the size. */
    void resized() override;
    /**
     * @brief A click jumps to the bar clicked.
     * @param[in] e  the mouse event
     */
    void mouseDown( const juce::MouseEvent & ) override;

    std::function<void( int )>                        m_onSeek;     ///< called with a bar to jump to
    std::function<void( phos::LockUnitE, int, bool )> m_onLock;     ///< unit, index, the new state
    std::function<void( phos::LockUnitE, int )>       m_onReroll;   ///< unit, index

private:
    /** @brief A rectangle that can be clicked, and what clicking it means. */
    struct Hit
    {
        juce::Rectangle<int> m_area;                            ///< where it is
        int                  m_bar = -1;                        ///< seek target, -1 = none
        phos::LockUnitE      m_unit = phos::LockUnitE::TRACK;   ///< which unit the two buttons act on
        int                  m_index = 0;                       ///< its index
        int                  m_action = 0;                      ///< 0 seek, 1 toggle the lock, 2 reroll
        bool                 m_locked = false;                  ///< state of the lock, for the toggle
    };

    /** @brief Measures the strip, the rows and every clickable rectangle from the snapshot. */
    void measure();
    /** @brief Draws everything that does not move into #m_cache. */
    void render();
    /**
     * @brief x of a bar inside the set strip.
     * @param[in] bar  the bar
     * @return its x in pixels
     */
    float stripX( double bar ) const;

    Snapshot                          m_snap;                ///< what is drawn
    std::vector<Hit>                  m_hits;                ///< filled by measure(), scanned by mouseDown()
    juce::Rectangle<int>              m_strip;               ///< the set-wide bar at the top
    std::vector<juce::Rectangle<int>> m_blocks;              ///< each track's block in the strip, in its lane
    std::vector<juce::Rectangle<int>> m_rowArea;             ///< the part of each row the sections live in
    juce::Image                       m_cache;               ///< the static picture at the current size
    double                            m_beat = 0.0;          ///< the play head, in beats from the start of the set
    int                               m_headPixel = -1;      ///< where it was last drawn, so a repaint is worth it
    int                               m_totalBars = 1;       ///< bars the strip spans
    bool                              m_stripOnly = false;   ///< setStripOnly
};
