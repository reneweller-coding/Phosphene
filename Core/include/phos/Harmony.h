/**
 * @file Harmony.h
 * @brief Scales, key helpers, the genre's colour tones and Lerdahl's stability hierarchy.
 *
 * Psytrance is almost always minor with a modal colour: Aeolian, Phrygian (the flat second is the
 * genre's signature interval), harmonic minor, Phrygian dominant and double harmonic (the "Hijaz"
 * sound of Goa), and Dorian for progressive. Each scale is seven semitone offsets from the root.
 *
 * **Stability.** Lerdahl's basic space (*Tonal Pitch Space*, Oxford University Press 2001) is a set
 * of nested levels over the twelve pitch classes of a key: the octave level holds the tonic, the
 * fifth level adds the fifth, the triadic level the third, the diatonic level the remaining scale
 * degrees, and the chromatic level all twelve. A pitch class's depth is how many levels contain it,
 * and its *instability* is five minus that depth -- 0 for the tonic, 1 for the fifth, 2 for the
 * minor third, 3 for the other diatonic degrees, 4 for a chromatic tone (the flat second, the
 * tritone and the raised second among them). That ordering is what Tools/corpus/measure_tension.py
 * counts against position in the phrase, and what the lead's and the acid's per-position weights
 * tilt towards (Melody.cpp, the measured tension curve of 16.09.2026).
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
 * @brief Whether a pitch class is one of the genre's colour tones of @p scale.
 *
 * The flat second (one semitone above the root) and the upper note of an augmented second -- a step
 * of three semitones between two neighbouring degrees, the Hijaz interval of Goa. Easwaran
 * ("Psytrance and the Spirituality of Electronics", 2004) names both as what makes a psytrance
 * melody sound like one; Farbood's tension model ("A parametric, temporal model of musical tension",
 * Music Perception 2012) counts them as dissonance, which is one of the four quantities the energy
 * arc moves.
 */
inline bool isColourTone(int scale, int pcFromRoot)
{
    const int pc = ((pcFromRoot % 12) + 12) % 12;
    if (pc == 1) return true;
    for (int d = 1; d < 7; ++d)
        if (scaleDegree(scale, d) - scaleDegree(scale, d - 1) == 3 && scaleDegree(scale, d) % 12 == pc) return true;
    return false;
}

/**
 * @brief How many of the genre's colour tones a mode holds: how far it reaches for the Hijaz sound.
 *
 * Aeolian and Dorian have none, Phrygian and harmonic minor one, Phrygian dominant two, double
 * harmonic three. That is the ranking the literature gives (Easwaran 2004), computed rather than
 * tabulated, and it is what decides which mode a high-energy section may borrow (Form.cpp).
 */
inline int scaleColourTones(int scale)
{
    int n = 0;
    for (int d = 0; d < 7; ++d) if (isColourTone(scale, scaleDegree(scale, d))) ++n;
    return n;
}

/**
 * @brief Lerdahl instability of a pitch class above the tonic: 0 tonic, 1 fifth, 2 minor third,
 *        3 the other diatonic degrees, 4 a chromatic tone.
 *
 * Five minus the depth of the pitch class in the basic space of a minor tonic (Lerdahl 2001); see
 * the file comment. Mirrored exactly by ``instability()`` in Tools/corpus/measure_tension.py, so the
 * curve the composer tilts towards is the curve that was measured.
 */
inline int lerdahlInstability(int pcFromRoot)
{
    const int pc = ((pcFromRoot % 12) + 12) % 12;
    int depth = 1;                                                // chromatic level: every pitch class
    if (pc == 0 || pc == 2 || pc == 3 || pc == 5 || pc == 7 || pc == 8 || pc == 10) ++depth;   // diatonic
    if (pc == 0 || pc == 3 || pc == 7) ++depth;                   // triadic
    if (pc == 0 || pc == 7) ++depth;                              // fifth
    if (pc == 0) ++depth;                                         // octave
    return 5 - depth;
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
