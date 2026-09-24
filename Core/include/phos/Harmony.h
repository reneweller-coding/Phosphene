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
 * @brief MIDI note of the bass root: the key's pitch class in the octave E1 (28) .. D#2 (39), that
 *        window moved by @p registerOffset semitones.
 *
 * The offset moves the *window*, never the pitch class. Until 24.09.2026 it was added to the note:
 * compose.bass_register at 4 put an F# set's bass on A# -- a major third over the key, in harmonic minor
 * a note the mode does not have, under a pad playing its A. The user: "Das Pad an fuer sich geht ja
 * jetzt, aber wenn der Bass einsetzt wird es absolut schief." At 0 and at +-12 the note is the one it
 * always was; in between the bass stays on the root and only changes octave where the window says so.
 */
inline int bassRootNote(int keyRoot, int registerOffset)
{
    const int low = 28 + registerOffset;   // the window's bottom note
    return low + ((((keyRoot - low) % 12) + 12) % 12);
}

/**
 * @name Chord types (22.09.2026, round "Harmonik")
 *
 * The user's brief on psytrance pads: they "meiden reine, einfache Dreiklaenge" and live in four
 * families instead -- the Phrygian chords with the flat second (m(b9), sus(b2)), the floating sus and
 * add chords that are neither major nor minor (sus2, sus4, m7, m9), the Phrygian-dominant "Hijaz"
 * chord with both a major third and a flat second, and the clusters of the dark styles (m(b5)). Until
 * this round every pad chord was the scale's plain triad. A type is a set of intervals over the chord
 * root; whether it *fits* is whether every one of them is in the section's mode (chordTypeFits), so
 * the same table gives Phrygian its m(b9) and Aeolian its m9 without a per-mode list.
 * @{ */
enum class ChordType : int {
    Triad = 0,   ///< root, third, fifth -- the scale's own
    Sus2,        ///< 1 - 2 - 5: open, weightless
    Sus4,        ///< 1 - 4 - 5
    Min7,        ///< 1 - b3 - 5 - b7: depth and room (progressive, morning)
    Min9,        ///< 1 - b3 - 5 - b7 - 9
    Maj7,        ///< 1 - 3 - 5 - 7: the bII of the Phrygian pendulum as Gmaj7 over F#
    MinFlat9,    ///< 1 - b3 - 5 - b9: the Phrygian signature, the semitone rub over the root
    SusFlat2,    ///< 1 - b2 - 5: no third at all, cold and unresolved
    PhrygDom,    ///< 1 - b2 - 3 - 5: Hijaz, the Goa temple sound
    MinFlat5,    ///< 1 - b3 - b5: the tritone cluster of darkpsy and forest
    Quartal,     ///< stacked fourths from the fifth: modern, cool, blurred
    Count
};
constexpr int kNumChordTypes = static_cast<int>(ChordType::Count);
inline constexpr const char* kChordTypeNames[kNumChordTypes] = {
    "triad", "sus2", "sus4", "m7", "m9", "maj7", "m(b9)", "sus(b2)", "phryg.dom", "m(b5)", "quartal"
};

/**
 * @brief The intervals of a chord type over its root, in semitones, in the order they are voiced.
 *
 * Index 0 is the fifth (the tritone for m(b5)), which sits directly over the root in every voicing;
 * the rest are the upper voices and are given at or above the octave, so that the pad's voicings are
 * *open* -- root, fifth, and the colour an octave up -- rather than the close triads in the low mids
 * the brief calls mud. The third, where a type has one, is the scale's own (3 or 4 semitones), so
 * "Triad" on the bII of Phrygian is the major chord the pendulum wants.
 * @return how many intervals were written (2 .. 4)
 */
inline int chordIntervals(ChordType type, int scale, int degree, int out[4])
{
    const int third = scaleDegree(scale, degree + 2) - scaleDegree(scale, degree);   // 3 or 4
    int n = 0;
    auto put = [&](int iv) { if (n < 4) out[n++] = iv; };
    switch (type) {
    case ChordType::Sus2:     put(7);  put(14); break;
    case ChordType::Sus4:     put(7);  put(17); break;
    case ChordType::Min7:     put(7);  put(third + 12); put(10); break;
    case ChordType::Min9:     put(7);  put(third + 12); put(10); put(14); break;
    case ChordType::Maj7:     put(7);  put(third + 12); put(11); break;
    case ChordType::MinFlat9: put(7);  put(third + 12); put(13); break;
    case ChordType::SusFlat2: put(7);  put(13); break;
    case ChordType::PhrygDom: put(7);  put(third + 12); put(13); break;
    case ChordType::MinFlat5: put(6);  put(third + 12); break;
    case ChordType::Quartal:  put(7);  put(12); put(17); put(22); break;
    case ChordType::Triad:
    default:                  put(7);  put(third + 12); break;
    }
    return n;
}

/**
 * @brief Whether every tone of the chord lies in the mode -- relative to the key, with the chord on
 *        scale degree @p degree.
 *
 * This is the whole harmony rule of the round: the style says which types it likes, the mode says
 * which of them exist. m(b9) needs the flat second, so it exists in the Phrygian family and not in
 * Aeolian; the Hijaz chord needs a major third and a flat second, so only Phrygian dominant and
 * double harmonic give it; m9 needs the major ninth, so it lives in Aeolian and Dorian. m(b5) fits
 * no root in any of the six modes and is admitted chromatically by the dark styles alone (Melody.cpp).
 */
inline bool chordTypeFits(int scale, int degree, ChordType type)
{
    int iv[4];
    const int n = chordIntervals(type, scale, degree, iv);
    const int root = scaleDegree(scale, degree);
    for (int i = 0; i < n; ++i) if (!inScale(scale, root + iv[i])) return false;
    return true;
}
/** @} */

} // namespace phos
