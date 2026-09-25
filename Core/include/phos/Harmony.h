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
 * @brief Whether a pitch class is a colour tone *for the acid and the lead*: isColourTone, except the b2 of the
 *        modes that have it as a degree (Phrygian, Phrygian dominant, double harmonic).
 *
 * The user's decision of 25.09.2026, from the literature they brought: in Phrygian the b2 is a structural degree
 * that also stands on stressed beats and without an immediate resolution -- the E - F - E riff of the acid and the
 * lead ("the defining interval for maximum psychedelic tension"). For those two lines it is drawn like any scale
 * tone (with a weight of its own); the other colour tones, and the b2 of every other voice, stay neighbours at
 * the colour slots.
 */
inline bool isLineColourTone(int scale, int pcFromRoot)
{
    const int pc = ((pcFromRoot % 12) + 12) % 12;
    return isColourTone(scale, pc) && !(pc == 1 && inScale(scale, 1));
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
 * The user's brief on psytrance pads: they "meiden reine, einfache Dreiklaenge" and live in the
 * floating sus and add chords that are neither major nor minor (sus2, sus4, m7, m9), open sevenths and
 * fourths. With the scale's plain triads alone every pad chord sounds alike. A type is a set of intervals over the chord root; whether it *fits* is whether
 * every one of them is in the section's mode (chordTypeFits), so the same table gives Aeolian its m9
 * and leaves it out of Phrygian without a per-mode list.
 *
 * **No b9 in a pad** (the user's rule, 25.09.2026: "Er sollte auf keinen Fall in den Pads verwendet
 * werden"). Held for bars under a bass and a drone on the tonic, a semitone over the root is heard as
 * wrong and tires the ear; the Phrygian colour belongs to the short accents of the lead and the arp
 * (their colour slots, Melody.h rule 1). The three types that were built on it -- m(b9), sus(b2) and
 * the Hijaz chord 1-b2-3-5 -- are therefore gone, no type is drawn where one of its tones is the key's
 * b2 (padAvoidsRubs), and the bII of the Phrygian pendulum, whose root *is* that b2, is held by the
 * pad as the tonic with the minor sixth (padChordIntervals).
 *
 * **Nor a leading tone or a tritone** (25.09.2026, from the literature the user brought: no source knows
 * a held m(b5) or a leading tone against the tonic pedal in a psytrance pad; darkpsy's dissonance is made
 * with the sound -- clusters, noise, drones -- not with voicings). The m(b5) cluster of the dark styles is
 * gone, and no type is drawn whose tones include the key's leading tone, which held under the drone's
 * octave is the same semitone rub a b9 is. A major seventh stays where its seventh is not the leading
 * tone (bVI and bIII as maj7), rarely.
 * @{ */
enum class ChordType : int {
    Triad = 0,   ///< root, third, fifth -- the scale's own
    Sus2,        ///< 1 - 2 - 5: open, weightless
    Sus4,        ///< 1 - 4 - 5
    Min7,        ///< 1 - b3 - 5 - b7: depth and room (progressive, morning)
    Min9,        ///< 1 - b3 - 5 - b7 - 9
    Maj7,        ///< 1 - 3 - 5 - 7: bVI or bIII as a floating major seventh
    Quartal,     ///< stacked fourths from the fifth: modern, cool, blurred
    Count
};
constexpr int kNumChordTypes = static_cast<int>(ChordType::Count);
inline constexpr const char* kChordTypeNames[kNumChordTypes] = {
    "triad", "sus2", "sus4", "m7", "m9", "maj7", "quartal"
};

/**
 * @brief The intervals of a chord type over its root, in semitones, in the order they are voiced.
 *
 * Index 0 is the fifth, which sits directly over the root in every voicing;
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
 * The style says which types it likes, the mode says which of them exist: m9 needs the major ninth,
 * so it lives in Aeolian and Dorian and not in Phrygian.
 */
inline bool chordTypeFits(int scale, int degree, ChordType type)
{
    int iv[4];
    const int n = chordIntervals(type, scale, degree, iv);
    const int root = scaleDegree(scale, degree);
    for (int i = 0; i < n; ++i) if (!inScale(scale, root + iv[i])) return false;
    return true;
}

/** @brief Whether a pitch class (semitones over the key) rubs against the tonic of bass and drone: its b2, or its leading tone. */
inline bool rubsTonic(int pcFromKey) { const int pc = ((pcFromKey % 12) + 12) % 12; return pc == 1 || pc == 11; }

/**
 * @brief Whether a pad may hold the chord: neither its root nor any of its tones rubs against the tonic
 *        (rubsTonic: the b2, a minor ninth over it, or the leading tone under its octave), and no tone is a
 *        minor ninth or a tritone over the chord's own root.
 */
inline bool padAvoidsRubs(int scale, int degree, ChordType type)
{
    int iv[4];
    const int n = chordIntervals(type, scale, degree, iv);
    const int root = scaleDegree(scale, degree) % 12;
    if (rubsTonic(root)) return false;
    for (int i = 0; i < n; ++i)
        if (rubsTonic(root + iv[i]) || iv[i] % 12 == 1 || iv[i] % 12 == 6) return false;
    return true;
}

/**
 * @brief What the pad plays for a chord of the progression: its root and its intervals, without a b9.
 *
 * Mostly the chord itself (chordIntervals). On the bII -- the Phrygian pendulum's other chord, whose root is
 * the minor ninth over the tonic -- the pad stays on the tonic and turns its colour instead: root, fifth,
 * octave and the minor sixth an octave up (E - B - E - C over E), the Phrygian pad the literature gives as the
 * safe alternative to the b9; the b2 itself sounds only as the lines' colour tone. The same holds for a root on
 * the leading tone, and for a chord whose fifth would be one (a bIII of Aeolian borrowed into double harmonic
 * stands on the major third, and its fifth is the leading tone). Any other tone that would rub against the tonic
 * or the root (only a type chosen when nothing else fitted can have one) is left out.
 * @param type   the chord's type
 * @param scale  index into kScaleSteps
 * @param degree the chord's scale degree
 * @param root   receives the pad's root in semitones over the key, 0..11
 * @param out    receives the intervals over that root, the fifth first
 * @return how many intervals were written (2 .. 4)
 */
inline int padChordIntervals(ChordType type, int scale, int degree, int& root, int out[4])
{
    int iv[4];
    const int m = chordIntervals(type, scale, degree, iv);
    root = scaleDegree(scale, degree) % 12;
    if (rubsTonic(root) || rubsTonic(root + iv[0]) || iv[0] % 12 == 6) {
        root = 0;
        out[0] = 7;
        out[1] = 12;
        out[2] = inScale(scale, 8) ? 20 : scaleDegree(scale, 2) + 12;
        return 3;
    }
    int n = 0;
    for (int i = 0; i < m; ++i) {
        const bool rub = rubsTonic(root + iv[i]) || iv[i] % 12 == 1 || iv[i] % 12 == 6;
        if (!rub) out[n++] = iv[i];
    }
    return n;
}
/** @} */

} // namespace phos
