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
 * **Acid.** A one- or two-bar cell A and its variations A' and A'', each redrawing one or two notes
 * of the one before from the model given their neighbours (the same sampler with every other note
 * fixed); A' also moves one accent. A A A' A'' per phrase, and a second set of cells after the
 * track's first breakdown. The corpus carries no usable accents or slides (a MIDI loop rarely
 * records them), so these are design values, set by the genre rules below.
 *
 * **Lead.** An eight-bar phrase A A' B A'': A is a two-bar motif; A' keeps its rhythm and every note
 * that still fits the new chords, redrawing only strong notes that do not; B has its own rhythm and
 * continues from A'; A'' is A again with its last notes redrawn towards a chord tone at the end. Two
 * phrases per track, alternating by eight bars.
 *
 * **Arp.** One bar per chord, every sixteenth, a low anchor under a high stream of sus2 / sus4 / add9
 * tones: up, down, up-down, Euclidean or polymetric, or drawn from the corpus arp model inside those
 * streams (see the genre rules below).
 *
 * **Layers.** Which parts play in which bar is no longer decided here. Since Phase 5 the
 * instrumentation matrix of the form grammar (Form.h) answers that per bar from the section type and
 * the energy arc; this file only builds the material and plays the bar it is handed. Since 19.09.2026 the
 * masking rule between the line voices lives here, at the sixteenth: composeMelodyBar's register guard keeps
 * lead, counter-lead, stab and arp in disjoint registers wherever two of them sound at once.
 *
 * **The new voices (19.09.2026, round "voices").** The counter-lead answers the lead in its held notes and
 * over its B phrase, an octave above it (makeCounter); the stab plays short root-position chords of the
 * arp's sus material on off-beat sixteenths (makeStab); the tonic drone holds root and fifth under a whole
 * run of the form -- its low octave where kick and bass rest, an octave up and quieter where they play.
 *
 * **Pads.** Four-note voicings in root position -- the root between D3 and C#4, the fifth above it,
 * two upper voices that complete the triad -- the upper pair chosen by the smallest total movement
 * of the voices from the voicing before, the voice-leading rule of the plan (5.7), exact rather than
 * greedy. Pads hold each chord and carry the sections without lead or acid; where kick and bass
 * rest they add the root an octave lower (the sub foundation, 18.09.2026).
 *
 * **Colour (dissonance).** Farbood's tension model counts dissonance among the four quantities an
 * energy arc should move. The flat second and the upper note of an augmented second -- the two
 * intervals Easwaran names as the genre's colour -- therefore get a share of the notes that grows
 * with the track's place on the arc and with the style profile. Since 18.09.2026 that share is a
 * calibrated target per role, met by placing the colour tones as neighbour tones (see the genre
 * rules below), and no longer a weight in the sampler's sets.
 *
 * **Colour (tension curve, measured 16.09.2026).** The per-position weights also carry the tension
 * curve the corpus was measured to have. A review proposed a schedule over
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
 * distribution and nothing else.
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
 * **Depth rule.** Acid lines stay at or above D3 (147 Hz), leads at or above C4, arps above G3, pads at or
 * above D3 -- except the pad's sub foundation, which exists only where kick and bass are silent.
 *
 * **Genre rules above the corpus (18.09.2026).** The user decided that genre rules stand *above* the
 * trained MIDI data: "Die Regeln sollten auf jeden Fall über den trainierten Midi-Dateien stehen! Die
 * Midi-Dateien können Mist enthalten." Everything described above still happens, but only inside the
 * rules below, which are hard constraints the learned models cannot leave:
 *  - *Colour tones are neighbour tones.* The flat second and the upper note of an augmented second
 *    appear only on weak sixteenths, one sixteenth long, and the next note is the tonic. They are
 *    kept out of every sampler set and placed at drawn "colour slots" instead, whose number is a
 *    target share per role (kColourShare in Melody.cpp) scaled by the arc's colour. That is the one
 *    mechanism for colour: before this round the arc's weight and the neural model's mode table both
 *    lifted the same tones, and the lead ended up parked on the flat second (0.41 of its notes).
 *  - *No mid-bar collapse, variation per phrase.* Every part fills both halves of the bar; a cell
 *    plays A A A' A'' over four (or eight) bars, and a second set of cells takes over after the
 *    track's first breakdown (MelodyContext::material), so new material arrives at section boundaries.
 *  - *Acid:* 11 to 14 onsets in 16, at most three rests (preferably on kick steps), three to five
 *    pitch classes around 1, 5, b3 and b7, no pitch three times in a row (an octave counts as a
 *    change), accents on the "e" and "a" sixteenths, more slides, D3 to D4 with octave jumps to D5.
 *  - *Arp:* sixteen sixteenths, split into a low anchor stream and a high stream an octave up
 *    (Bregman, *Auditory Scene Analysis*, MIT Press 1990: a fast alternation between two registers
 *    is heard as two streams); the Euclidean and polymetric styles decide which steps jump up; sus2,
 *    sus4 and add9 material; G3 to G5; a short gate.
 *  - *Lead:* a dense one- or two-bar riff (sixteenths, eighths with pickups, or a gallop) in A A' B
 *    A''; the fifth as a resting tone; C4 to G4 and never above G4 (20.09.2026, kLeadHighest -- the
 *    rule replaces the earlier "median around A4 to C5, never above A5").
 *  - *Pad:* root position with the fifth above the root at the bottom, from D3; where the form
 *    silences kick and bass the root an octave lower as well (the sub foundation).
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

// MelodyPart, kMelodyParts, mpIndex() and partBit() live in Form.h, because the instrumentation
// matrix needs them as well.
constexpr int kAcidLowest = 50;                                     ///< D3: lowest acid note
constexpr int kAcidHighest = 62;                                    ///< D4: top of the acid's own register
constexpr int kAcidJumpHighest = 74;                                ///< D5: highest octave jump of the acid
/** @name The two leads' registers (20.09.2026, round "dialogue")
 *  The user's rule, after listening to the standalone on 19.09.2026: "beide Leads klingen zu hoch".
 *  The lead speaks from **C4** and the counter-lead answers exactly one octave above it, from **C5**.
 *  Until this round the two spanned B3 .. A5 and C5 .. A6, nearly two octaves each, and both reached
 *  well over the arp's own G3 .. G5; what the user heard as "too high" was the upper half of those
 *  windows, which the register weight only discouraged and never forbade.
 *
 *  **Why an octave and not a fifth.** The rule's note names are "C4-G4" and "C5-G5", and its
 *  parentheses are "about 260-400 Hz" and "500-1000 Hz" -- but 500 to 1000 Hz is C5 to B5, not C5 to
 *  G5, so the rule's own two halves do not agree. A window of a fifth decides the question by itself
 *  and decides it wrongly: eight semitones carry eight of the twelve pitch classes, so in four keys of
 *  twelve the **tonic is not in the window at all** and rules 1 and 18 -- the tonic as the centre, the
 *  fifth as the resting tone -- cannot be met by any line. Measured before the window was widened: 69
 *  of 110 counter answers ended on the tonic or the fifth instead of the 9 in 10 the rule asks for.
 *  So the floor of each window is the rule's note (C4, C5), the top is an octave above it (B4, B5),
 *  and the counter's window is then exactly the 500 .. 1000 Hz the rule's own parenthesis names. The
 *  lead's 262 .. 494 Hz reaches a major third over the "400 Hz" of its parenthesis; the register
 *  weight (kLeadCentre, kLeadSigma in Melody.cpp) puts the mass of the line in its lower half.
 *
 *  **What it costs, measured rather than hidden:** an octave-wide window has no room for an upward
 *  octave jump, so MotifOperator::OctaveJump -- the Goa lead idiom of PLAN 6.5 -- can no longer place
 *  a note and is no longer drawn for the lead (Melody.cpp, makeLead). docs/PLAN.md carries the number.
 *  @{ */
constexpr int kLeadLowest = 60;                                     ///< C4 (262 Hz): the lead window's lowest bottom (MelodyPlan::leadWindowLo moves it up by the style's registerShift)
constexpr int kLeadHighest = 71;                                    ///< B4 (494 Hz): highest lead note
/** @} */
constexpr int kArpLowest = 55;                                      ///< G3: lowest arp note
constexpr int kArpHighest = 79;                                     ///< G5: highest arp note
constexpr int kPadLowest = 50;                                      ///< D3: lowest pad note while kick and bass play
constexpr int kPadHighest = 79;                                     ///< G5: highest pad note
constexpr int kPadFoundationLowest = 30;                            ///< F#1: lowest note of the sub foundation (22.09.2026; was D2 -- the brief wants octave 1 or 2)
constexpr int kAcidVariants = 3;                                    ///< A, A', A'' of one acid cell
constexpr int kMaterialSets = 2;                                    ///< cell sets: before and after the first breakdown
constexpr int kAcidCells = kMaterialSets * kAcidVariants;           ///< entries of MelodyPlan::acid
constexpr int kArpCells = kMaterialSets * kAcidVariants * 4;        ///< entries of MelodyPlan::arp (set, variant, chord)
constexpr double kArpGate = 0.2;                                    ///< arp note length, in sixteenths (rule: 15-35 %)
/** @name The new voices' registers (19.09.2026, round "voices")
 *  The counter-lead answers exactly an octave above the lead (kCounterLowest above); the arp
 *  may leave G3..G5 upwards only to clear a lead it shares a bar with (composeMelodyBar); the stab's chord
 *  is rooted where the arp's anchor is and moves by octaves between D3 and C7 to clear the lead; the drone's root lies an octave under the pad's where kick and
 *  bass rest, and on the pad's own octave where they play.
 *  @{ */
constexpr int kCounterLowest = 72;                                  ///< C5 (523 Hz): lowest counter-lead note
constexpr int kCounterHighest = 83;                                 ///< B5 (988 Hz): highest counter-lead note at the lowest window
constexpr int kLeadWindow = 12;                                     ///< the lead's window is one octave (23.09.2026)
constexpr int kArpOverHighest = 91;                                 ///< G6: the arp's ceiling when it clears a lead from above
constexpr int kStabLowest = 50;                                     ///< D3: lowest stab note (the depth rule's floor, like the acid's)
constexpr int kStabHighest = 96;                                    ///< C7: highest stab note (2.1 kHz, the top of the leads' pocket)
constexpr int kDroneLowest = 38;                                    ///< D2: the drone's root where the floor is silent
/** @} */
/**
 * @brief The minimum distance in semitones between two line voices that sound at the same instant.
 *
 * The rule of the brief -- "two voices never double the same register at the same time" -- made
 * concrete: at every sixteenth, the notes of lead, counter-lead, stab and arp that sound there must lie
 * in disjoint pitch spans with at least this gap between them (Melody.cpp, the register guard).
 */
constexpr int kRegisterGap = 3;

/** @brief Index of an acid cell: material set @p set (0, 1), variant @p variant (0 = A, 1 = A', 2 = A''). */
constexpr int acidCell(int set, int variant) { return set * kAcidVariants + variant; }
/** @brief Index of an arp cell: material set, variant (A, A', A'') and chord (0..3). */
constexpr int arpCell(int set, int variant, int chord) { return (set * kAcidVariants + variant) * 4 + chord; }

/**
 * @brief Which variant of a cell a bar plays: A A A' A'' over a phrase of four cell lengths.
 * @param barInPhrase bar within the part's phrase
 * @param cellBars    the cell's length in bars (1 or 2)
 */
constexpr int variantOfBar(int barInPhrase, int cellBars)
{
    const int cell = barInPhrase / (cellBars < 1 ? 1 : cellBars);
    return cell <= 1 ? 0 : (cell == 2 ? 1 : 2);
}

/**
 * @brief What a bar needs to know beyond its BarPlan, decided by the composer from the whole form.
 *
 * Both fields are functions of the track's plan and the bar's place in it, never of what was played
 * before, so a bar composed alone is the bar composed in sequence.
 */
struct MelodyContext {
    int material = 0;         ///< 0 before the track's first breakdown, 1 after it (new cells at a boundary)
    int foundationBars = 0;   ///< > 0 where a pad chord starts in a bar without kick and bass: how many
                              ///< consecutive bars from here on keep kick and bass silent (0 where the
                              ///< drone lays the floor instead: the two never double it)
    /**
     * @brief > 0 where a drone note starts in this bar: how many bars it holds (19.09.2026).
     *
     * A drone note is one held chord per *run* -- consecutive bars of the drone with the same floor
     * (kick and bass silent, or playing) inside one section -- so its length has to be known where it
     * starts, like the pad's sub foundation. The composer counts it from the form (Composer.cpp,
     * melodyContext); a probe sets it by hand.
     */
    int droneBars = 0;
    bool droneLow = false;    ///< the drone run lies on a silent floor: its low octave (D2..C#3)
    int droneTail = 0;        ///< bars before the run's end in which the low root stops (the kick returns after it)
    /**
     * @brief Pad or acid sound somewhere in this drone run (20.09.2026, round "dialogue").
     *
     * The drone is a continuous carpet since that round, so it meets the pad's root and fifth and the
     * acid's octave nearly everywhere. Where it does, it holds its **root alone** and leaves the fifth
     * to whoever already has it; a run with the band to itself holds root and fifth (and the octave
     * where the track takes it). Decided once per run -- the drone's attack is 1.5 s.
     */
    bool droneShaded = false;
};

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
    std::vector<MelodyNote> acid[kAcidCells];   ///< acid cells in this mode (acidCell)
    std::vector<MelodyNote> lead[2];    ///< the two eight-bar lead phrases
    std::vector<MelodyNote> counter[2]; ///< the counter-lead's answers to them (19.09.2026)
    std::vector<MelodyNote> arp[kArpCells];     ///< arp cells (arpCell)
    std::vector<int> padVoicing[4];     ///< the pad voicings of the core chords
    std::vector<int> breakVoicing[4];   ///< and of the main breakdown's (22.09.2026)
    std::vector<int> padVoicing2[4];    ///< and of the second half's (23.09.2026)
    bool built = false;                 ///< false: this mode is not used by the track's form
};

/**
 * @brief How the pad states its chord inside a chord block (22.09.2026, round "Figuren").
 *
 * `Held` is what every track did before this round and is still what an intro, a breakdown and an
 * outro play, whatever the track drew: there the pad *is* the music, and an articulated pad under
 * nothing sounds like a mistake. The other three are for the sections that have a kick under them.
 */
enum class PadFigure : int {
    Held = 0,   ///< one chord for the whole block, struck on its first bar
    Pulse,      ///< struck again every bar, each note a bar long
    Offbeat,    ///< two hits a bar, on the "and" of two and of four, an eighth and a half long
    Swell,      ///< the block in two halves: the first quiet and short, the second the full chord
    Syncope,    ///< the downbeat and the "and" of three, the way a stab sits against the kick
    Count
};
/** @brief Display names of PadFigure, in its order. */
extern const char* const kPadFigureNames[static_cast<int>(PadFigure::Count)];

/**
 * @brief The set's motif (23.09.2026, round "Set-Kurve"): one lead cell the first track states and a later
 *        track recalls, so that a night has a thread and not only a sequence of tracks.
 *
 * The cell is a rhythm (a bit per sixteenth) with an archetype and a density band; its pitches are drawn
 * afresh in the recalling track's key and mode, the way a motif returns in another key. It is decided by the
 * set walk (Composer.cpp, walkAt) from the set seed alone, so rerolling a track moves no other track's recall
 * (PLAN 6.8). `phrase` says which of the two lead phrases plays it in this track: 0 in the first track, 1 in
 * the track that recalls it, -1 in every other.
 */
struct SetMotif {
    uint16_t cell = 0;        ///< the cell's onset mask, 0 = no motif
    int8_t   archetype = 0;   ///< LeadArchetype (Form.h)
    int8_t   band = 1;        ///< the density band the cell was drawn in
    int8_t   phrase = -1;     ///< which lead phrase plays it, -1 = none
};

/**
 * @brief Draws the set's motif cell: a lead cell the rules admit in @p band, with no bass to interlock with
 *        (the set has no one bass), from @p seed alone.
 */
uint16_t drawMotifCell(uint64_t seed, int band);

/** @brief Everything melodic that is decided once per track. */
struct MelodyPlan {
    bool present[kMelodyParts] = {};         ///< which parts the track uses at all (MelodyPart order)
    /// The knobs behind `present`, kept so that a section can ask how *much* a part should play
    /// rather than only whether it exists at all (21.09.2026; makeMelodyPlan, Form.cpp drawLead).
    float amount[kMelodyParts] = {};
    /**
     * @name How the pad states its chord (22.09.2026, round "Figuren")
     *
     * The user, on the pad: "sie war nicht sehr abwechslungsreich sondern klang immer gleich und hat
     * immer dasselbe gespielt". Until this round that was literally true of its rhythm: one held
     * chord per chord block, struck on the bar line, for the whole track -- every track. The only
     * movement it ever had was the trance gate, and that was one pattern for 256 bars.
     *
     * A figure is a way of *stating* the chord, not a different chord: the onsets inside a chord
     * block and how long each one holds. They are the four ways a psytrance pad is actually played --
     * held under everything, re-struck each bar so the attack is heard, stabbed on the offbeats
     * between the kick and the bass, or swelling into the next block. The track draws one; the
     * sections that exist to be a carpet (intro, breakdown, outro) hold whatever it drew, because
     * that is what those sections are for.
     * @{ */
    int  padFigure = 0;                       ///< PadFigure: how the pad states the chord in a drop
    int  padFigureGroove = 0;                 ///< PadFigure: and in a groove, never the same one
    int  padGateAlt = 0;                      ///< a second gate pattern, for the sections that do not use padGatePattern
    /** @} */
    /**
     * @name Harmony (rebuilt 22.09.2026, round "Harmonik")
     *
     * The user's brief: psytrance pads do not play cadences over a moving bass, they set *modal
     * tension over a bass that stays on the tonic* (the Bordun principle; compose.bass_follows_chords
     * is off by default for exactly that). So the core of a track is a **pendulum** of two chords --
     * i <-> bII (Phrygian family), i <-> bVII, i <-> iv, i <-> bVI, i <-> v -- expressed over the
     * four slots the lines' material is built on, changing every 4, 8 or 16 bars; and each chord has a
     * **type** from Harmony.h that the style likes and the mode allows. The main breakdown, the one
     * before the last drop, has harmony of its own: the aeolian three (i - bVI - bVII) where the mode
     * has both, else the tonic held for the whole of it. Any other breakdown holds the tonic.
     * @{ */
    int  chordBars = 8;                       ///< bars per chord in the cores (4, 8 or 16)
    int  chordDegree[4] = {};                 ///< scale degree of each core chord (the pendulum over four slots)
    int  chordType[4] = {};                   ///< ChordType of each core chord (the pad's; the lines take the triad's tones)
    int  breakDegree[4] = {};                 ///< the main breakdown's chords
    int  breakType[4] = {};                   ///< and their types
    int  breakChordBars = 8;                  ///< bars per chord in the main breakdown
    bool breakHolds = true;                   ///< the main breakdown holds one chord (the mode has no aeolian three)
    /** @name Harmony's fuzziness (23.09.2026, round "Harmonie")
     *  The user: "das absolute Verbot klassischer Harmonien scheint mir etwas zuuuu streng ausgelegt [...]
     *  Auch Harmonie-Wechsel innerhalb eines Stueckes koennten wir erlauben". Beside the pendulum a track
     *  may play a *loop* -- the minor loops the literature grants Progressive and Full-On (i - bVI - bVII,
     *  i - bVII - bVI - bVII, i - iv - i - v, i - bIII - bVII - iv) in classical types (triads, sevenths) --
     *  and behind the main breakdown a second progression. The Bordun stays: bass and lines anchor the
     *  tonic (round "Harmonik"), so the change is the pad's and the stab's colour, not a modulation.
     *  @{ */
    int  progression = 0;                     ///< 0 pendulum, 1 loop (kLoops in Melody.cpp)
    int  loop = -1;                           ///< which loop, or -1
    bool secondHalf = false;                  ///< the bars behind the main breakdown take chordDegree2 / chordType2
    int  progression2 = 0;                    ///< the second half's family
    int  loop2 = -1;                          ///< and its loop
    int  chordDegree2[4] = {};                ///< the second half's core chords
    int  chordType2[4] = {};                  ///< and their types
    /** @} */
    /** @} */
    int  root[kMelodyParts] = { 50, 64, 76, 57, 57, 55, 38 }; ///< MIDI root of each part (the pad's, the stab's and the drone's are unused)
    int  acidSteps = 16;                      ///< acid pattern length (16 or 32)
    std::vector<MelodyNote> acid[kAcidCells]; ///< acid cells: set 0/1 x A, A', A'' (acidCell); [0] is A
    std::vector<MelodyNote> lead[2];          ///< two eight-bar lead phrases (128 steps)
    std::vector<MelodyNote> arp[kArpCells];   ///< one bar per chord, per set and variant (arpCell); [0..3] are A
    int  arpTones = 0;                        ///< the arp's tone material: 0 sus2, 1 sus4, 2 add9
    uint16_t arpHigh = 0;                     ///< bit per sixteenth: the step belongs to the high stream
    int  arpStyle = 0;                        ///< ArpStyle: corpus, up, down, up-down, Euclid, polymeter
    bool arpOctaveJump = false;               ///< every other two bars an octave up
    int  leadLo = 127, leadHi = 0;            ///< the lead's pitch range (for the masking rule)
    int  arpLo = 127, arpHi = 0;              ///< the arp's pitch range
    int  acidSquelch = -1;                    ///< override of acid.squelch, -1 = the knob
    int  leadOsc = -1;                        ///< override of lead.osc, -1 = the knob (superseded by the voice recipes, kept for the report)
    int  delay[kMelodyParts][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 }, { -1, -1 } };   ///< overrides of the delay times
    std::vector<int> padVoicing[4];           ///< MIDI notes of each core chord's pad voicing
    std::vector<int> breakVoicing[4];         ///< and of the main breakdown's chords (22.09.2026)
    std::vector<int> padVoicing2[4];          ///< and of the second half's chords (23.09.2026; empty when secondHalf is false)
    int  padGatePattern = 0;                  ///< the track's gate pattern
    float recipe[kMelodyParts] = {};          ///< one sound direction per part, -1..1 (brightness)
    /** @name The new voices (19.09.2026, round "voices"; Melody.cpp, makeCounter, makeStab, makeDrone)
     *  @{ */
    std::vector<MelodyNote> counter[2];       ///< the counter-lead's two eight-bar phrases, one per lead phrase
    uint16_t stabMask[2] = { 0, 0 };          ///< the stab's onsets over two bars, a bit per sixteenth (never a beat)
    uint8_t stabBars = 0;                     ///< bit per bar of a four-bar phrase: which bars the stab plays
    int  stabTones = 0;                       ///< the stab chord's material: 0 sus2, 1 sus4, 2 add9 (root position)
    bool droneOctave = false;                 ///< the drone adds the octave above its root where the pad is silent
    int  droneEvolveBars = 16;                ///< period of the drone's slow timbral evolution, 8, 16 or 32 bars
    /** @} */
    float colour = 0.0f;                      ///< the arc's colour (0..1): scales the target colour share
    int  scale = 0;                           ///< the track's own mode; the arrays above are its material
    int  key = 6;                             ///< the track's key (pitch class of the tonic)
    ModeMaterial mode[kNumScales];            ///< material of every *borrowed* mode the form uses
    /** @name The lead's design (22.09.2026, round "Lead"; Melody.cpp, makeLead)
     *  One cell, one archetype and one operator per bar, per phrase -- what the renderer's `--tracks`
     *  prints and what the self test reads the phrase against.
     *  @{ */
    int      leadArchetype[2] = { 0, 0 };     ///< LeadArchetype of each phrase
    int8_t   leadOps[2][8] = {};              ///< CellOp of each bar of each phrase
    int8_t   leadShift[2][8] = {};            ///< the transposition each bar really got, in scale steps
    uint16_t leadCell[2] = { 0, 0 };          ///< the cell's onset mask, a bit per sixteenth
    int      leadDensityBand = 1;             ///< 0 sparse (8..10 onsets), 1 medium (10..12), 2 dense (12..16)
    bool     leadCellFromCorpus[2] = { false, false };   ///< the cell's rhythm came from the corpus templates
    bool     leadQuotesSet[2] = { false, false };        ///< the phrase plays the set's motif (SetMotif; 23.09.2026)
    /** @} */
    /** @name Register and counter mode (23.09.2026, round "Counter")
     *  @{ */
    int      leadWindowLo = kLeadLowest;      ///< MIDI note of the lead window's bottom (one octave wide; the counter's an octave above)
    int      counterMode = 1;                 ///< CounterMode of the track (Form.h)
    /** @} */
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
 * @param colour     0..1: scales the target share of colour tones in acid, lead and arp (0.6..1.0 of it)
 * @param scaleMask  bit per mode: the modes the form's sections borrow (FormPlan::scaleMask). Every
 *                   bit other than @p scale gets its own recoloured material; 0 means the track
 *                   stays in one mode, which is exactly the behaviour before 16.09.2026.
 * @param bassMask   the track's bass onsets over a bar, a bit per sixteenth (22.09.2026: the lead's
 *                   rhythm interlocks with it; 0 = no bass to interlock with)
 * @param motif      the set's motif and which lead phrase of this track plays it (SetMotif; 23.09.2026);
 *                   the default plays none, which is every track's behaviour before that date
 */
MelodyPlan makeMelodyPlan(const ParamStore& p, const StyleProfile& style, uint64_t seed, int key, int scale,
                          bool firstTrack, float colour, uint32_t scaleMask = 0, unsigned bassMask = 0,
                          const SetMotif& motif = SetMotif{});

extern const char* const kLeadArchetypeNames[kNumLeadArchetypes];   ///< names of LeadArchetype (Form.h)
extern const char* const kCellOpNames[kNumCellOps];                 ///< names of CellOp (Form.h)
/** @brief The lead's and the counter's windows of a track (23.09.2026): one octave each, the counter an octave up. */
inline int leadWindowHi(const MelodyPlan& m) { return m.leadWindowLo + kLeadWindow - 1; }
inline int counterWindowLo(const MelodyPlan& m) { return m.leadWindowLo + 12; }
inline int counterWindowHi(const MelodyPlan& m) { return m.leadWindowLo + 12 + kLeadWindow - 1; }
/** @brief The cutoff arc of an archetype at a bar of the phrase, mean-free (Composer.cpp writes depth x this). */
double leadArc(int archetype, int barInPhrase);

/**
 * @brief Composes one bar of the melodic parts.
 * @param p          knob values (swing)
 * @param m          the track's plan
 * @param bar        absolute bar
 * @param barInTrack bar within the track
 * @param scale      the track's scale (for the lead's and arp's chords)
 * @param bp         what the instrumentation matrix says plays in this bar
 * @param out        receives the notes
 * @param ctx        the cell set and the pad foundation of this bar (Composer::melodyContext); the
 *                   default is the first cell set without a foundation, which is what every caller
 *                   that plays material outside the form (probes, the transition's pads) wants
 */
void composeMelodyBar(const ParamStore& p, const MelodyPlan& m, int bar, int barInTrack, int scale,
                      const BarPlan& bp, std::vector<NoteEvent>& out, const MelodyContext& ctx = {});

/**
 * @brief The pad voicing of a bar without kick and bass: the root an octave under the voicing's
 *        root, then the root, the fifth and the voicing's third (the other upper voice is dropped,
 *        so the pad never needs more than four voices per chord and a chord change never steals).
 * @param voicing a root-position voicing of voiceChord (root, fifth, two upper voices)
 */
std::vector<int> foundationVoicing(const std::vector<int>& voicing);

/**
 * @brief The tones an arp cell may use over a chord: the anchor (the chord root in the low
 *        octave) and the sus2 / sus4 / add9 material an octave above it (Melody.h, rule 13).
 * @param tones  0 sus2 (1 2 5), 1 sus4 (1 4 5), 2 add9 (1 3 5 9)
 * @param anchor receives the anchor as a MIDI note
 * @return the high stream's MIDI notes, ascending, all in [kArpLowest, kArpHighest]
 */
std::vector<int> arpHighTones(int scale, int degree, int key, int tones, int& anchor);

/**
 * @brief The stab's chord over a chord of the track, in root position (19.09.2026): the arp's anchor
 *        as the root, then sus2 (1 2 5 8), sus4 (1 4 5 8) or add9 (1 3 5 9) -- no colour tone, no
 *        imperfect fifth.
 * @param tones 0 sus2, 1 sus4, 2 add9 (MelodyPlan::stabTones)
 * @return ascending MIDI notes, the root first
 */
std::vector<int> stabChord(int scale, int degree, int key, int tones);

/** @brief A bar plan that plays every part the track has (the level-match probe). */
BarPlan allPartsBar(const MelodyPlan& m);

/**
 * @brief The pad voicing of a chord in root position: the root in [kPadLowest, kPadLowest + 11], the
 *        fifth above it, then two chord tones up to kPadHighest that complete the triad, with the least
 *        total movement from @p previous (or from a centred reference).
 * @param scale,degree the chord
 * @param key          the key's pitch class
 * @param previous     the voicing before, or null
 */
/**
 * @brief Voices a chord of @p type on scale degree @p degree for the pad (rewritten 22.09.2026).
 *
 * Root position, open: the chord root lowest in D3 .. C#4, its fifth (the tritone for m(b5)) directly
 * above, and the type's colour tones above that at or over the octave -- root, fifth, and the colour
 * up high, which is the "Weite Lagen" of the brief and not the close triad in the low mids. Adjacent
 * upper voices stand at least a minor third and at most an octave apart, nothing above G5, at most
 * five voices. Among the placements that satisfy that, the one that moves least from @p previous
 * (or from a centred reference without one): the same voice leading rule as before.
 */
std::vector<int> voiceChord(int scale, int key, int degree, int type, const std::vector<int>* previous);

/** @brief The pad's chord in one bar: which, of which type, and where in its block the bar lies. */
struct PadChord {
    int  degree = 0;       ///< scale degree
    int  type = 0;         ///< ChordType
    int  slot = 0;         ///< 0 .. 3: index into padVoicing / breakVoicing
    int  blockBars = 8;    ///< bars the chord holds
    int  barInBlock = 0;   ///< 0 = the bar the chord starts on
    bool inBreak = false;  ///< from the main breakdown's own harmony
    bool secondSet = false; ///< from the second half's progression (23.09.2026): padVoicing2
};
/**
 * @brief The pad's chord for a bar (22.09.2026). The cores follow the pendulum on track-absolute
 *        blocks; a main breakdown its own progression from its first bar; any other breakdown holds
 *        the tonic.
 */
PadChord padChordAt(const MelodyPlan& m, const BarPlan& bp, int barInTrack);
/** @brief The same, from the form alone (for tests and tools without a BarPlan). */
PadChord padChordAt(const MelodyPlan& m, const FormPlan& f, int barInTrack);

/** @brief Total movement of the voices between two sorted voicings of equal size, in semitones. */
int voicingMovement(const std::vector<int>& a, const std::vector<int>& b);

/**
 * @brief Composes the effects that start in one bar.
 * @param f          the track's form (its effects sit at the section boundaries)
 * @param trackBeat  beat at which the track starts
 * @param barInTrack bar within the track
 * @param out        receives the effect notes (pitch kSfxBaseNote + type) on the part that plays them --
 *                   Sfx, Texture or Vocal (19.09.2026) -- with a voice's or the bed's variant in the lane
 */
void composeSfxBar(const FormPlan& f, double trackBeat, int barInTrack, std::vector<NoteEvent>& out);

/** @brief Semitones the bass moves in @p barInTrack when it follows the chords (0 on the tonic). */
int bassChordShift(const MelodyPlan& m, int scale, int barInTrack, int bassRoot);

} // namespace phos
