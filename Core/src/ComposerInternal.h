/**
 * @file ComposerInternal.h
 * @brief What the three sources of the Composer share (Composer.cpp, ComposerControls.cpp, ComposerLevels.cpp).
 *
 * Not part of the core's interface: the voices' palette (which a track's recipe is drawn from and which the presets
 * and the control events read), the energy's gain, and the three views of a plan the bars and the level probes both
 * need.
 */
#pragma once
#include "phos/Composer.h"
#include "phos/Melody.h"
#include "phos/WaveTableFile.h"
#include <algorithm>
#include <cstdint>

namespace phos {

/**
 * @brief What a polyphonic voice may become in a track: its oscillators, wavetables and filter responses,
 *        each with a weight (19.09.2026, round "voices"; rebuilt 22.09.2026, round "Klangfarben").
 *
 * ## Lanes, not table numbers
 *
 * Until 22.09.2026 a row named its tables by hand -- fifteen indices into the `table` parameter, with
 * a `static_assert` tying the array's size to the pack's 35 entries so that neither could move without
 * the other. That was workable while the pack held 35 tables and is the reason the pack *stayed* at 35:
 * every widening meant re-writing six rows of indices by hand. A row now names the **lanes**
 * `Tools/wt_select.py` measured the library into (WaveTableFile.h, WaveTableLane), plus whichever
 * built-ins fit the role, and `waveTableLaneTables()` turns that into the candidate list at run time.
 * Repack with a wider selection and the palettes widen with it.
 *
 * The pack holds 464 library tables (35 before 22.09.2026): 80 lead, 96 arp,
 * 128 pad, 96 counter and 64 drone, each chosen from the 2191 licence-clean tables of the Noctuary
 * library by the same farthest-point spreading rule as before (`Core/data/CREDITS-wavetables.md`).
 * They cost nothing until a track asks for one: the pack is only indexed at load and a table is
 * expanded into its mip levels when a voice's recipe actually draws it (WaveTableFile.h,
 * ensureWaveTables), which is what makes a library of this size possible at all -- expanded in full
 * it would be 900 MB.
 *
 * **The counter-lead has its own lane now.** The "voices" round gave it three built-ins, the
 * vocal/formant family, deliberately: it had to stay out of the lead's and the arp's table families so
 * that the two leads could never share a sound. That was the right call against a 35-table pack and it
 * is exactly why the counter was the one voice the user could not tell apart from track to track --
 * three tables is three colours. The lane (`counter` in wt_select.py) keeps the character the choice
 * was about, the vowel: a mid centroid that *moves* across the frames, with a fundamental still there.
 * The hard guard in the recipe draw below (`v == counterV`: never the lead's oscillator, never the
 * lead's table) is what keeps the two apart, and it does that far more comfortably against 96 tables
 * than against three.
 *
 * ## Oscillators
 *
 * Every lane can now draw every oscillator. The old rows had zeros in them -- the pad could never be
 * VA or FM, the drone never supersaw or FM, the stab never FM -- so a third of the recipe's reach was
 * dead for most voices, and the FM parameters the recipe learned to shape on 21.09.2026 were dead for
 * the pad entirely. The user's point ("die Mischung 0,25 und 0,75 zwischen FM und Wavetable ist ja
 * nicht gottgegeben"): the weights are a tilt towards the role, not a gate. The wavetable weight is
 * the largest everywhere, because that is the axis the 464 tables sit on; the rest say what the voice
 * leans towards when it is not a table -- the lead towards the supersaw, the drone and the counter
 * towards VA and FM, the pad towards a low-index FM (a classic pad, not a bell).
 */
struct VoicePalette {
    double osc[static_cast<int>(PolyOsc::Count)];       ///< weight per PolyOsc
    int    builtin[6];                                  ///< built-in candidates (-1 ends the list)
    int8_t lane[3];                                     ///< lanes drawn from (-1 ends the list)
    double filter[static_cast<int>(PolyFilter::Count)]; ///< weight per PolyFilter
    double osc2[static_cast<int>(PolyOsc2::Count)];     ///< weight per PolyOsc2, index 0 = no second oscillator
    double interval[static_cast<int>(PolyOsc2Interval::Count)];   ///< weight per PolyOsc2Interval
    float  scale[kNumVoiceMacros];                      ///< how far each direction reaches for this voice
};
constexpr int8_t kLanePad = static_cast<int8_t>(WaveTableLane::Pad);   ///< the pad's wavetable lane
constexpr int8_t kLaneLead = static_cast<int8_t>(WaveTableLane::Lead);   ///< the lead's
constexpr int8_t kLaneArp = static_cast<int8_t>(WaveTableLane::Arp);   ///< the arp's
constexpr int8_t kLaneDrone = static_cast<int8_t>(WaveTableLane::Drone);   ///< the drone's
constexpr int8_t kLaneCounter = static_cast<int8_t>(WaveTableLane::Counter);   ///< the counter's
/// The drone's second oscillator never goes *below* it, which is the one row where that matters and
/// where the first attempt at these weights was wrong. The drone is already the lowest voice, and
/// rule 20 is explicit about the band under 140 Hz: it belongs to the kick and the bass from their
/// first beat back, which is why the drone moves up an octave when they return (Melody.cpp,
/// makeDrone). An octave-down partner put it straight back there -- testVoices.droneRender measured
/// -5.9 dB in the two bars after the breakdown where it wants -18 dB or less. Unison, a fifth up or
/// an octave up give it an organ-like body instead, in its own register.
///
/// The pad never answers below its own note either, and the reason is sharper than the drone's. Its
/// high pass tracks the note, so an octave-down partner is filtered away -- *except* in a breakdown,
/// where rule 20 opens that high pass to 40 Hz on purpose so the pad can carry the floor kick and
/// bass have left. So the one place the low partner is audible at all is the one place it does harm:
/// bisected to this row, the breakdown's 40..140 Hz band went from 23.7 dB under the core to 18.1,
/// against the 20 dB the section rule asks for (testSectionRules). Unison with a few cents of detune
/// is what changes the pad's *colour* rather than its weight anyway -- with an octave-down partner on
/// every pad the rendered spread of the pad's centroid across twenty tracks fell from 915 to 321
/// cents, because a low partner pulls every track's centroid to the same place.
///
/// Nor does the pad answer at a fifth (24.09.2026). The partner doubles every tone of the chord, so a
/// fifth over a chord is a second chord a fifth higher: over F# Phrygian's C# it plays G#, which the
/// mode does not have, and over a sus2 or an m7 it stacks new seconds onto the ones the chord already
/// has. The drone plays one note -- a fifth there is an organ; under a held chord it was part of what the
/// user heard as "schraeg". The stab plays the same chords, short, and lost its fifths the same day.
///
/// Per row: the first oscillator's weights; the built-in candidate tables; the library lanes; the
/// filter responses; the second oscillator's weights (index 0 = none, and it is the largest
/// everywhere -- a second oscillator is a colour a track may draw, not a thing every track has); the
/// interval it answers at (-2 Oct, -1 Oct, -5th, Unison, +5th, +1 Oct); and the reach of the five
/// directions.
///                                     Supersaw VA   FM   WT      built-ins                  lanes                             LP    BP    HP   Notch     off   sup   va    fm    wt          -2oct -1oct -5th  uni   +5th  +1oct     bright soft thick space motion
inline const VoicePalette kVoicePalette[kPolyInstances] = {
    /* lead    */ { { 0.30, 0.12, 0.13, 0.45 }, { 4, 5, -1, -1, -1, -1 },  { kLaneLead, -1, -1 },        { 0.80, 0.10, 0.0, 0.10 }, { 0.45, 0.15, 0.15, 0.10, 0.15 }, { 0.05, 0.35, 0.10, 0.20, 0.05, 0.25 }, { 1.0f, 0.6f, 1.0f, 1.0f, 0.8f } },
    /* counter */ { { 0.08, 0.17, 0.20, 0.55 }, { 5, 1, 2, -1, -1, -1 },   { kLaneCounter, -1, -1 },     { 0.40, 0.40, 0.0, 0.20 }, { 0.45, 0.05, 0.20, 0.20, 0.10 }, { 0.05, 0.30, 0.10, 0.30, 0.05, 0.20 }, { 1.0f, 0.8f, 0.8f, 1.0f, 1.0f } },
    /* arp     */ { { 0.22, 0.20, 0.13, 0.45 }, { 2, 3, -1, -1, -1, -1 },  { kLaneArp, -1, -1 },         { 0.75, 0.25, 0.0, 0.00 }, { 0.50, 0.10, 0.15, 0.10, 0.15 }, { 0.02, 0.28, 0.05, 0.25, 0.05, 0.35 }, { 1.0f, 0.0f, 0.8f, 1.0f, 0.6f } },
    /* stab    */ { { 0.28, 0.18, 0.09, 0.45 }, { 3, 4, -1, -1, -1, -1 },  { kLaneArp, kLaneLead, -1 },  { 0.70, 0.30, 0.0, 0.00 }, { 0.45, 0.15, 0.15, 0.10, 0.15 }, { 0.05, 0.40, 0.00, 0.25, 0.00, 0.30 }, { 1.0f, 0.0f, 1.0f, 1.0f, 0.5f } },
    /* pad     */ { { 0.15, 0.08, 0.12, 0.65 }, { 1, 2, 5, -1, -1, -1 },   { kLanePad, -1, -1 },         { 0.85, 0.00, 0.0, 0.15 }, { 0.30, 0.10, 0.25, 0.15, 0.20 }, { 0.00, 0.00, 0.00, 0.60, 0.00, 0.40 }, { 0.8f, 1.0f, 1.0f, 1.0f, 1.0f } },
    /* drone   */ { { 0.06, 0.20, 0.09, 0.65 }, { 1, -1, -1, -1, -1, -1 }, { kLaneDrone, kLanePad, -1 }, { 0.90, 0.10, 0.0, 0.00 }, { 0.30, 0.05, 0.30, 0.10, 0.25 }, { 0.00, 0.00, 0.05, 0.45, 0.20, 0.30 }, { 0.7f, 0.6f, 1.0f, 0.8f, 1.0f } },
};

/**
 * @brief Real candidates in a palette's `tables[]`: entries before the first -1 (or the array's end).
 *
 * The one place this is counted -- the per-track recipe draw below calls it too, so
 * `Composer::voicePaletteTableCount()` (the test-facing wrapper, Composer.h) can never drift from what
 * a track actually draws from.
 */
inline int paletteTableCount(const VoicePalette& pal)
{
    int n = 0;
    for (int b : pal.builtin) if (b >= 0) ++n;
    for (int8_t l : pal.lane) if (l >= 0) n += static_cast<int>(waveTableLaneTables(static_cast<WaveTableLane>(l)).size());
    return n;
}

/** @brief Candidate @p k of a palette: the built-ins first, then each lane in turn. */
inline int paletteTableAt(const VoicePalette& pal, int k)
{
    for (int b : pal.builtin) {
        if (b < 0) break;
        if (k-- == 0) return b;
    }
    for (int8_t l : pal.lane) {
        if (l < 0) break;
        const std::vector<int>& lane = waveTableLaneTables(static_cast<WaveTableLane>(l));
        if (k < static_cast<int>(lane.size())) return lane[static_cast<size_t>(k)];
        k -= static_cast<int>(lane.size());
    }
    return -1;
}

/**
 * @brief The loudness side of Farbood's tension model: what a section's energy does to the track gain.
 *
 * At most +-2 dB around the level the track was matched to, so that the energy is audible without
 * undoing the level match between tracks.
 */
inline float energyGainDb(float energy)
{
    return std::clamp((energy - 0.7f) * 5.0f, -2.0f, 2.0f);
}

/** @brief Which melodic parts a track plays, how much, and its lead's range: what a transition bar needs of it. */
PartAvailability availabilityOf(const TrackPlan& plan);
/** @brief Whether the drone plays bar @p inTrack in its low octave (the high pass is opened for it). */
bool droneLowAt(const TrackPlan& plan, int inTrack);
/** @brief The melody context of bar @p bp, bar @p inTrack of @p plan (see its definition in Composer.cpp). */
MelodyContext melodyContext(const TrackPlan& plan, const BarPlan& bp, int inTrack);

} // namespace phos
