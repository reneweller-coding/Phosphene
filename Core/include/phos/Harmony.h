/**
 * @file Harmony.h
 * @brief Scales and key helpers.
 *
 * Psytrance is almost always minor with a modal colour: Aeolian, Phrygian (the flat second is the
 * genre's signature interval), harmonic minor, Phrygian dominant and double harmonic (the "Hijaz"
 * sound of Goa), and Dorian for progressive. Each scale is seven semitone offsets from the root.
 */
#pragma once

namespace phos {

constexpr int kNumScales = 6;   ///< entries of kScaleNames (Params.h)

/** @brief Semitone offsets of the seven degrees of each scale. */
inline constexpr int kScaleSteps[kNumScales][7] = {
    { 0, 2, 3, 5, 7, 8, 10 },   // Aeolian
    { 0, 1, 3, 5, 7, 8, 10 },   // Phrygian
    { 0, 2, 3, 5, 7, 8, 11 },   // Harmonic minor
    { 0, 1, 4, 5, 7, 8, 10 },   // Phrygian dominant
    { 0, 1, 4, 5, 7, 8, 11 },   // Double harmonic
    { 0, 2, 3, 5, 7, 9, 10 },   // Dorian
};

/**
 * @brief Semitones of a scale degree, extended over octaves.
 * @param scale  index into kScaleSteps
 * @param degree 0 = root, 7 = root an octave up, -1 = the seventh below
 */
inline int scaleDegree(int scale, int degree)
{
    const int s = scale < 0 ? 0 : (scale >= kNumScales ? kNumScales - 1 : scale);
    int oct = 0;
    while (degree < 0) { degree += 7; --oct; }
    while (degree >= 7) { degree -= 7; ++oct; }
    return kScaleSteps[s][degree] + 12 * oct;
}

/** @brief Whether a pitch class (relative to the root) belongs to the scale. */
inline bool inScale(int scale, int semitonesFromRoot)
{
    const int pc = ((semitonesFromRoot % 12) + 12) % 12;
    for (int i = 0; i < 7; ++i) if (scaleDegree(scale, i) == pc) return true;
    return false;
}

/**
 * @brief MIDI note of the bass root: the key's pitch class in the octave E1 (28) .. D#2 (39),
 *        shifted by a register offset.
 */
inline int bassRootNote(int keyRoot, int registerOffset)
{
    return 28 + (((keyRoot - 4) % 12) + 12) % 12 + registerOffset;
}

} // namespace phos
