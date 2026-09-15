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
 * **Layers.** Per 16-bar block the track decides which parts play: acid from the second block, arp
 * from the third, the lead from the fourth in two of every three blocks, none in the first block. When
 * lead and arp play together the arp moves by octaves until its range and the lead's overlap by at most
 * two semitones -- masking would otherwise blur both (the plan's masking rule) -- or it sits the block
 * out.
 *
 * **Depth rule.** Acid lines stay at or above D3 (147 Hz), leads above B3 and arps above G3.
 */
#pragma once
#include "phos/Params.h"
#include "phos/Score.h"
#include <cstdint>
#include <vector>

namespace phos {

class PitchModel;
enum class CorpusRoleId : int;

/** @brief The melodic parts. */
enum class MelodyPart : int { Acid = 0, Lead, Arp, Count };
constexpr int kMelodyParts = static_cast<int>(MelodyPart::Count);   ///< number of melodic parts
constexpr int kMelodyMaxBlocks = 64;                                ///< 16-bar blocks a track can have
constexpr int kAcidLowest = 50;                                     ///< D3: lowest acid note
constexpr int kLeadLowest = 59;                                     ///< B3: lowest lead note
constexpr int kArpLowest = 55;                                      ///< G3: lowest arp note

/** @brief One note of a pattern: position and length in sixteenths, pitch relative to the part's root. */
struct MelodyNote {
    int16_t step = 0;        ///< sixteenths from the pattern start
    int16_t len = 1;         ///< sixteenths (to the next note when sliding)
    int8_t  rel = 0;         ///< semitones above the part's root
    uint8_t velocity = 100;  ///< MIDI velocity
    uint8_t flags = 0;       ///< NoteFlag bits
};

/** @brief Everything melodic that is decided once per track. */
struct MelodyPlan {
    bool present[kMelodyParts] = {};         ///< which parts the track uses at all
    int  chordBars = 2;                       ///< bars per chord (2 or 4)
    int  chordDegree[4] = {};                 ///< scale degree of each chord
    int  root[kMelodyParts] = { 50, 64, 57 }; ///< MIDI root of each part
    int  acidSteps = 16;                      ///< acid pattern length (16 or 32)
    std::vector<MelodyNote> acid[2];          ///< acid pattern A and variation B
    std::vector<MelodyNote> lead[2];          ///< two eight-bar lead phrases (128 steps)
    std::vector<MelodyNote> arp[4];           ///< one bar per chord
    int  arpStyle = 0;                        ///< 0 corpus, 1 up, 2 down, 3 up-down
    bool arpOctaveJump = false;               ///< every other two bars an octave up
    uint8_t blockParts[kMelodyMaxBlocks] = {};///< bit per MelodyPart for each 16-bar block
    int8_t  arpShift[kMelodyMaxBlocks] = {};  ///< octave shift of the arp per block (masking rule)
    float acidArc[kMelodyMaxBlocks] = {};     ///< normalised acid cutoff offset reached at the end of each block
    int  acidSquelch = -1;                    ///< override of acid.squelch, -1 = the knob
    int  leadOsc = -1;                        ///< override of lead.osc, -1 = the knob
    int  delay[kMelodyParts][2] = { { -1, -1 }, { -1, -1 }, { -1, -1 } };   ///< overrides of the delay times
    float recipe[kMelodyParts] = {};          ///< one sound direction per part, -1..1 (brightness)
};

/** @brief The shared pitch model of a corpus role (built on first use). */
const PitchModel& corpusPitchModel(CorpusRoleId role);

/** @brief Pitch classes (semitones above the key root) of the triad on @p degree of @p scale. */
void chordTones(int scale, int degree, int out[3]);

/** @brief Index of the chord that sounds in bar @p barInTrack. */
inline int chordIndexAt(const MelodyPlan& m, int barInTrack) { return (barInTrack / m.chordBars) % 4; }

/**
 * @brief Makes the melodic plan of a track.
 * @param p          knob values (compose.* amounts and variation)
 * @param seed       the track's melody seed
 * @param key,scale  the track's key and mode
 * @param bars       the track's length
 * @param firstTrack the first track plays the knobs' sounds (no overrides)
 */
MelodyPlan makeMelodyPlan(const ParamStore& p, uint64_t seed, int key, int scale, int bars, bool firstTrack);

/**
 * @brief Composes one bar of the melodic parts.
 * @param p          knob values (swing)
 * @param m          the track's plan
 * @param bar        absolute bar
 * @param barInTrack bar within the track
 * @param scale      the track's scale (for the lead's and arp's chords)
 * @param allParts   play every present part regardless of the block schedule (the level probe)
 * @param out        receives the notes
 */
void composeMelodyBar(const ParamStore& p, const MelodyPlan& m, int bar, int barInTrack, int scale, bool allParts,
                      std::vector<NoteEvent>& out);

/** @brief Semitones the bass moves in @p barInTrack when it follows the chords (0 on the tonic). */
int bassChordShift(const MelodyPlan& m, int scale, int barInTrack, int bassRoot);

} // namespace phos
