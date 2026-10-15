/**
 * @file Patterns.h
 * @brief Bass pattern definitions shared by the composer and the engine.
 *
 * The composer places notes by these slots; the engine needs the first slot of the current pattern
 * for two computations that must agree with the composer exactly: how long the kick may ring before
 * the first bass note (the tail constraint), and the kick's phase at that moment (the phase lock).
 * @author Rene Weller
 */
#pragma once

namespace phos {

constexpr int NumBassPatterns = 5;   ///< entries of BassPatternNames (Params.h)

/** @brief Slots of a bass pattern within one beat, in beats after the kick. */
struct BassPatternDef {
    int m_count;        ///< notes per beat
    double m_pos[3];    ///< positions in beats, ascending
};

/** @brief K-B-B-B, K-.-B-B, K-B-.-B, K-.-B-., K-B-B (triplets). */
inline constexpr BassPatternDef BassPatterns[NumBassPatterns] = {
    { 3, { 0.25, 0.5, 0.75 } },               // Rolling
    { 2, { 0.5, 0.75, 0.0 } },                // Gallop
    { 2, { 0.25, 0.75, 0.0 } },               // Skip
    { 1, { 0.5, 0.0, 0.0 } },                 // Offbeat
    { 2, { 1.0 / 3.0, 2.0 / 3.0, 0.0 } },     // Triplet
};

/**
 * @brief Beats from the kick to the first bass note of a pattern.
 * @param[in] pattern  the pattern
 * @return the beats
 */
inline double firstBassSlot(int pattern)
{
    const int p = pattern < 0 ? 0 : (pattern >= NumBassPatterns ? NumBassPatterns - 1 : pattern);
    return BassPatterns[p].m_pos[0];
}

/**
 * @brief Shortest slot of a pattern in beats: the distance from a note to the next note, or from the
 *        last note of the beat to the next kick. A note's length is Gate times its slot.
 * @param[in] pattern  the pattern
 * @return the slot in beats
 */
inline double shortestBassSlot(int pattern)
{
    const int p = pattern < 0 ? 0 : (pattern >= NumBassPatterns ? NumBassPatterns - 1 : pattern);
    const BassPatternDef& d = BassPatterns[p];
    double best = 1.0 - d.m_pos[d.m_count - 1];
    for (int i = 0; i + 1 < d.m_count; ++i) best = d.m_pos[i + 1] - d.m_pos[i] < best ? d.m_pos[i + 1] - d.m_pos[i] : best;
    return best;
}

} // namespace phos
