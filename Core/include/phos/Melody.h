/**
 * @file Melody.h
 * @brief The melodic layer of a track: chords, acid line, lead phrases, arpeggios and when each plays.
 *
 * Everything is decided once per track from its seed (MelodyPlan) and then played bar by bar, so a
 * track has an identity -- its acid riff, its lead motif, its arp -- that returns and varies instead of
 * being redrawn every bar.
 *
 * **Harmony.** Four chords of two or four bars each, on the degrees of the track's scale. Chord changes
 * follow the chord-root successions measured bar by bar in the arp loops of the corpus (Corpus.h):
 * at a change the chord stays with probability 0.35, otherwise the next root is drawn from the corpus
 * successions with the chord kept removed, among the roots the scale offers. (The corpus keeps its
 * chord in 88 % of bars; used as it is, nearly every progression would be the tonic alone.)
 *
 * **Pitches** are drawn from the corpus pitch model of the role (Witten-Bell order 2) under constraints,
 * by exact constrained sampling (Pachet and Roy 2011): scale tones inside the register, chord tones on
 * strong sixteenths (every eighth step) for lead and arp, the root at the start of an acid line and a
 * chord tone at the end of a lead phrase. **Rhythm** comes from the corpus onset and length counts per
 * sixteenth step of the same role.
 *
 * **Acid.** A one- or two-bar pattern A and its variation B, which redraws two or three of A's notes
 * from the model given their neighbours (the same sampler with every other note fixed) and moves one
 * accent. A A A B per phrase. The corpus carries no usable accents or slides (a MIDI loop rarely
 * records them), so these are design probabilities: accents likelier on the offbeat eighths, slides
 * only into a note that follows directly.
 *
 * **Lead.** An eight-bar phrase A A' B A'': A is a two-bar motif; A' keeps its rhythm and every note
 * that still fits the new chords, redrawing only strong notes that do not; B has its own rhythm and
 * continues from A'; A'' is A again with its last notes redrawn towards a chord tone at the end. Two
 * phrases per track, alternating by eight bars.
 *
 * **Arp.** One bar per chord, on the chord tones over one or two octaves: up, down, up-down, or drawn
 * from the corpus arp model with every note constrained to the chord.
 *
 * **Layers.** Which parts play in which bar is no longer decided here. Since Phase 5 the
 * instrumentation matrix of the form grammar (Form.h) answers that per bar from the section type and
 * the energy arc, including the masking rule between lead and arp; this file only builds the material
 * and plays the bar it is handed.
 *
 * **Pads.** Four-note voicings of the chords between G3 and G5, each chosen from every combination of
 * chord tones that contains all three pitch classes by the smallest total movement of the voices from
 * the voicing before -- the voice-leading rule of the plan (5.7), exact rather than greedy. Pads hold
 * each chord and carry the sections without lead or acid.
 *
 * **Colour (dissonance).** Farbood's tension model counts dissonance among the four quantities an
 * energy arc should move. The lead's constraint sets therefore carry weights rather than plain flags:
 * the flat second and the upper note of an augmented second -- the two intervals Easwaran names as the
 * genre's colour -- get more weight the higher the track's place on the arc and the more the style
 * profile asks for.
 *
 * **Colour (tension curve, measured 16.09.2026).** On top of the arc's colour the per-position
 * weights carry the tension curve the corpus was measured to have. A review proposed a schedule over
 * an eight-bar phrase (stable to bar 4, rising, peak in bar 7, resolving in bar 8) from Lerdahl's
 * stability hierarchy (*Tonal Pitch Space*, Oxford 2001). Tools/corpus/measure_tension.py counted
 * the hierarchy against position in 655 deduplicated corpus lines and found something else: within
 * the bar instability rises from beat 1 to beat 4 (lead +0.55 Lerdahl levels, 95 % interval [0.32,
 * 0.79], paired per line; acid +0.45 [0.16, 0.76]; arp +0.29 [0.21, 0.36]) and between bars it
 * *alternates* with a period of two -- the second bar of a two-bar pair is the less stable one (lead
 * +0.20 [0.07, 0.32], acid +0.14 [0.04, 0.25], arp +0.12 [0.09, 0.14]). There is no eight-bar arch,
 * and the last note of a bar is *less* often a tonic or a fifth as a four-bar group goes on (arp
 * 0.73 to 0.60, acid 0.64 to 0.54, lead 0.58 to 0.48), which is the opposite of the proposed
 * resolution. So what is implemented is the measured curve and nothing else: an exponential tilt
 * @c exp(kTensionTilt * D(beat, bar parity) * instability(pitch class)) on the same per-position
 * weights the arc's colour uses, where D is the measured deviation. A constant factor at a position
 * cancels in the sampler (CorpusSample.inl), so the tilt changes the *shape* of a position's
 * distribution and nothing else, and the two weightings multiply rather than fight.
 *
 * **Motivic operators** (16.09.2026). Partial redrawing alone is not how a composer varies a motif.
 * A'' now also takes one systematic transformation, drawn per phrase: a rhythmic phase shift of the
 * two-bar motif by one sixteenth (syncopation), a contour-preserving expansion (the up/down contour
 * kept sign for sign while the intervals widen), or octave jumps on single offbeat sixteenths -- the
 * Goa lead idiom. Schoenberg's developing variation as Frisch describes it ("Brahms and the
 * Principle of Developing Variation", University of California Press 1984) is the frame; the three
 * operators are the ones PLAN 6.5 names. Every one of them stays inside the scale, the ambitus and
 * the register the masking rule works with.
 *
 * **Depth rule.** Acid lines stay at or above D3 (147 Hz), leads above B3, arps and pads above G3.
 */
#pragma once
#include "phos/Form.h"
#include "phos/Params.h"
#include "phos/Score.h"
#include <cstdint>
#include <vector>

namespace phos {

class PitchModel;
enum class CorpusRoleId : int;

/** @brief The melodic parts. */
enum class MelodyPart : int { Acid = 0, Lead, Arp, Pad, Count };
constexpr int kMelodyParts = static_cast<int>(MelodyPart::Count);   ///< number of melodic parts
constexpr int kAcidLowest = 50;                                     ///< D3: lowest acid note
constexpr int kLeadLowest = 59;                                     ///< B3: lowest lead note
constexpr int kArpLowest = 55;                                      ///< G3: lowest arp note
constexpr int kPadLowest = 55;                                      ///< G3: lowest pad note
constexpr int kPadHighest = 79;                                     ///< G5: highest pad note

/**
 * @brief How an arp picks its steps and its notes (MelodyPlan::arpStyle).
 *
 * The first four are what the program has always offered. Euclid and Polymeter were added on
 * 16.09.2026: Euclid spreads the onsets of a bar as evenly as possible with Bjorklund's algorithm
 * (Toussaint, "The Euclidean algorithm generates traditional musical rhythms", BRIDGES 2005) --
 * E(5,16), E(7,16) and their neighbours, the same generator the percussion lanes use -- and
 * Polymeter runs a three-sixteenth cell against the 4/4 bar, so the cell starts one sixteenth later
 * in every bar and comes home every three bars.
 */
enum class ArpStyle : int { Corpus = 0, Up, Down, UpDown, Euclid, Polymeter, Count };
constexpr int kNumArpStyles = static_cast<int>(ArpStyle::Count);   ///< number of arp styles

/** @brief One note of a pattern: position and length in sixteenths, pitch relative to the part's root. */
struct MelodyNote {
    int16_t step = 0;        ///< sixteenths from the pattern start
    int16_t len = 1;         ///< sixteenths (to the next note when sliding)
    int8_t  rel = 0;         ///< semitones above the part's root
    uint8_t velocity = 100;  ///< MIDI velocity
    uint8_t flags = 0;       ///< NoteFlag bits
};

/**
 * @brief The melodic material of one borrowed mode: the same track, recoloured over the same tonic.
 *
 * Modal interchange (Form.h) redraws only the *pitches*. Every maker in Melody.cpp draws its rhythm,
 * its accents and its slides before any pitch exists, and it draws them from the track's melody seed,
 * so running a maker again with the same seed and another mode gives back the same rhythm with other
 * notes -- which is what interchange means and what "a change to the allowed set, not a new
 * subsystem" asks for. (The one way the two streams can part is a constrained draw that fails and
 * falls back, or a masked draw that retries; both are rare, both stay deterministic, and both affect
 * only the mode they happen in.)
 */
struct ModeMaterial {
    std::vector<MelodyNote> acid[2];    ///< acid pattern A and variation B in this mode
    std::vector<MelodyNote> lead[2];    ///< the two eight-bar lead phrases
    std::vector<MelodyNote> arp[4];     ///< one bar per chord
    std::vector<int> padVoicing[4];     ///< the pad voicings
    bool built = false;                 ///< false: this mode is not used by the track's form
};

/** @brief Everything melodic that is decided once per track. */
struct MelodyPlan {
    bool present[kMelodyParts] = {};         ///< which parts the track uses at all
    int  chordBars = 2;                       ///< bars per chord (2 or 4)
    int  chordDegree[4] = {};                 ///< scale degree of each chord
    int  root[kMelodyParts] = { 50, 64, 57, 55 }; ///< MIDI root of each part (the pad's is unused)
    int  acidSteps = 16;                      ///< acid pattern length (16 or 32)
    std::vector<MelodyNote> acid[2];          ///< acid pattern A and variation B
    std::vector<MelodyNote> lead[2];          ///< two eight-bar lead phrases (128 steps)
    std::vector<MelodyNote> arp[4];           ///< one bar per chord
    int  arpStyle = 0;                        ///< ArpStyle: corpus, up, down, up-down, Euclid, polymeter
    bool arpOctaveJump = false;               ///< every other two bars an octave up
    int  leadLo = 127, leadHi = 0;            ///< the lead's pitch range (for the masking rule)
    int  arpLo = 127, arpHi = 0;              ///< the arp's pitch range
    int  acidSquelch = -1;                    ///< override of acid.squelch, -1 = the knob
    int  leadOsc = -1;                        ///< override of lead.osc, -1 = the knob
    int  delay[kMelodyParts][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } };   ///< overrides of the delay times
    std::vector<int> padVoicing[4];           ///< MIDI notes of each chord's pad voicing
    int  padGatePattern = 0;                  ///< the track's gate pattern
    float recipe[kMelodyParts] = {};          ///< one sound direction per part, -1..1 (brightness)
    float colour = 0.0f;                      ///< how much the lead leaned on the flat second (0..1, for tests)
    int  scale = 0;                           ///< the track's own mode; the arrays above are its material
    ModeMaterial mode[kNumScales];            ///< material of every *borrowed* mode the form uses
    int  leadOperator[2] = { 0, 0 };          ///< the motivic operator of each lead phrase (MotifOperator)
    int  leadJumps[2] = { 0, 0 };             ///< notes MotifOperator::OctaveJump lifted in each phrase
    bool arpPolymeter = false;                ///< the arp runs a 3/16 cell against the 4/4 bar
    int  arpPulses = 0;                       ///< Euclidean arps: onsets per bar (E(pulses, 16))
    int  arpRotation = 0;                     ///< and the rotation Bjorklund's pattern is turned by
};

/**
 * @brief The systematic transformations a lead motif's last repetition may take (PLAN 6.5).
 *
 * Partial redrawing keeps a motif recognisable but never *develops* it. These three are the
 * operators the plan names, in Schoenberg's sense of developing variation (Frisch, "Brahms and the
 * Principle of Developing Variation", University of California Press 1984).
 */
enum class MotifOperator : int {
    None = 0,     ///< A'' as it always was: the motif with its last notes redrawn to a chord tone
    PhaseShift,   ///< the whole two-bar motif displaced by one sixteenth (syncopation)
    Expand,       ///< the up/down contour kept sign for sign while the intervals widen
    OctaveJump,   ///< single offbeat sixteenths thrown an octave up -- the Goa lead idiom
    Count
};
constexpr int kNumMotifOperators = static_cast<int>(MotifOperator::Count);   ///< number of operators

/** @brief The sign of each successive interval of a line: -1, 0 or +1, one entry less than notes. */
std::vector<int> melodyContour(const std::vector<MelodyNote>& notes);

/**
 * @brief Widens every interval of a line while keeping its contour sign for sign (MotifOperator::Expand).
 *
 * Exposed so that the self test can measure the operator itself rather than only its effect: given a
 * parent line and the constraint set of each position, the result must have the same contour, sign
 * for sign, and no interval narrower than the parent's.
 *
 * @param parent  the line to expand, as intervals to the part's root
 * @param allowed one entry per position, @c allowed[i][symbol] != 0 where the symbol may stand
 * @param lo,hi   the ambitus, in the same intervals
 * @param out     receives the expanded line
 * @return false when the expansion would leave the ambitus or could widen nothing at all
 */
bool expandContour(const std::vector<int>& parent, const std::vector<std::vector<uint8_t>>& allowed,
                   int lo, int hi, std::vector<int>& out);

/** @brief The shared pitch model of a corpus role (built on first use). */
const PitchModel& corpusPitchModel(CorpusRoleId role);

/** @brief Pitch classes (semitones above the key root) of the triad on @p degree of @p scale. */
void chordTones(int scale, int degree, int out[3]);

/** @brief Index of the chord that sounds in bar @p barInTrack. */
inline int chordIndexAt(const MelodyPlan& m, int barInTrack) { return (barInTrack / m.chordBars) % 4; }

/**
 * @brief Makes the melodic plan of a track.
 * @param p          knob values (compose.* amounts and variation)
 * @param style      the style profile (part amounts, squelch chance, chord moves, colour)
 * @param seed       the track's melody seed
 * @param key,scale  the track's key and mode
 * @param firstTrack the first track plays the knobs' sounds (no overrides)
 * @param colour     0..1: how much weight the flat second and the augmented second get in the lead
 * @param scaleMask  bit per mode: the modes the form's sections borrow (FormPlan::scaleMask). Every
 *                   bit other than @p scale gets its own recoloured material; 0 means the track
 *                   stays in one mode, which is exactly the behaviour before 16.09.2026.
 */
MelodyPlan makeMelodyPlan(const ParamStore& p, const StyleProfile& style, uint64_t seed, int key, int scale,
                          bool firstTrack, float colour, uint32_t scaleMask = 0);

/**
 * @brief Composes one bar of the melodic parts.
 * @param p          knob values (swing)
 * @param m          the track's plan
 * @param bar        absolute bar
 * @param barInTrack bar within the track
 * @param scale      the track's scale (for the lead's and arp's chords)
 * @param bp         what the instrumentation matrix says plays in this bar
 * @param out        receives the notes
 */
void composeMelodyBar(const ParamStore& p, const MelodyPlan& m, int bar, int barInTrack, int scale,
                      const BarPlan& bp, std::vector<NoteEvent>& out);

/** @brief A bar plan that plays every part the track has (the level-match probe). */
BarPlan allPartsBar(const MelodyPlan& m);

/**
 * @brief The pad voicing of a chord: four chord tones in [kPadLowest, kPadHighest] containing all three
 *        pitch classes, with the least total movement from @p previous (or from a centred reference).
 * @param scale,degree the chord
 * @param key          the key's pitch class
 * @param previous     the voicing before, or null
 */
std::vector<int> voiceChord(int scale, int degree, int key, const std::vector<int>* previous);

/** @brief Total movement of the voices between two sorted voicings of equal size, in semitones. */
int voicingMovement(const std::vector<int>& a, const std::vector<int>& b);

/**
 * @brief Composes the effects that start in one bar.
 * @param f          the track's form (its effects sit at the section boundaries)
 * @param trackBeat  beat at which the track starts
 * @param barInTrack bar within the track
 * @param out        receives Part::Sfx notes (pitch kSfxBaseNote + type)
 */
void composeSfxBar(const FormPlan& f, double trackBeat, int barInTrack, std::vector<NoteEvent>& out);

/** @brief Semitones the bass moves in @p barInTrack when it follows the chords (0 on the tonic). */
int bassChordShift(const MelodyPlan& m, int scale, int barInTrack, int bassRoot);

} // namespace phos
