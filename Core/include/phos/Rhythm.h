/**
 * @file Rhythm.h
 * @brief Percussion patterns: Euclidean rhythms, syncopation, roles, fills, layers.
 *
 * **Measured, not assumed.** Tools/ref_perc_profile.py folded the onset strength of twelve reference
 * tracks into the sixteenth grid (docs/PLAN.md, 6.6). In the hat band the strongest position in seven
 * of twelve tracks is the eighth offbeat (31 to 49 % of the onsets), and the two remaining sixteenths
 * carry 10 to 25 % each: a quieter sixteenth layer at roughly half the offbeat's strength. That is
 * what the hat roles play by default.
 *
 * **Euclidean rhythms.** k onsets spread as evenly as possible over n steps -- Bjorklund's algorithm,
 * which Toussaint showed generates a large family of traditional rhythms ("The Euclidean algorithm
 * generates traditional musical rhythms", BRIDGES 2005). Toms, congas, rims, zaps and blips take
 * their patterns from it, never on the kick's beats.
 *
 * **Syncopation.** The Longuet-Higgins and Lee measure ("The rhythmic interpretation of monophonic
 * music", Music Perception 1984): metrical weights 0 for the downbeat, -1 half bar, -2 quarter notes,
 * -3 eighths, -4 sixteenths; a note followed by a rest on a stronger position contributes the
 * difference. Sioros, Miron, Davies, Gouyon and Madison ("Syncopation creates the sensation of groove
 * in synthesized music examples", Frontiers in Psychology 2014) found groove rising with moderate
 * syncopation. So a Euclidean lane's rotation is chosen to put its syncopation at a target between the
 * least and most syncopated rotations, not at either end.
 *
 * **Phrases, fills, layers.** Optional hits are decided once per four-bar phrase and repeat inside it,
 * which keeps a groove a groove. The last bar of every eighth bar carries a fill; after a sixteen-bar
 * fill a crash may open the next phrase. Within a track the percussion enters layer by layer, one lane
 * per sixteen-bar block, and now and then a block breathes with a layer less.
 */
#pragma once
#include "phos/Params.h"
#include "phos/Score.h"
#include <array>
#include <cstdint>
#include <vector>

namespace phos {

constexpr int kStepsPerBar = 16;   ///< the sixteenth grid of a 4/4 bar

/**
 * @brief Semitones the buildup's snare roll rises by from its first hit to the drop (16.09.2026).
 *
 * An octave, because that is the gesture's own name and the size a rising roll is written at; the
 * rise is linear in the position within the four roll bars, so the last hit before the pre-drop
 * break's silence stands about eleven semitones up. The lane's high pass follows the same shift
 * through @c perc.cut_track (Perc.h), which is the thinning half of the gesture.
 */
constexpr int kRollSemitones = 12;

/** @brief Bjorklund's Euclidean rhythm E(pulses, steps), rotated right by @p rotation steps. */
std::vector<bool> euclid(int pulses, int steps, int rotation);

/** @brief Longuet-Higgins and Lee metrical weight of a sixteenth step in a 4/4 bar. */
int lhlWeight(int step);

/** @brief Longuet-Higgins and Lee syncopation of a cyclic one-bar pattern of 16 steps. */
int lhlSyncopation(const bool* steps);

/** @brief Kinds of fill in the last bar of a phrase. */
enum class FillType : int { None = 0, SnareRoll, TomRun, ZapBurst, ClapTriplet, HatDrop, Count };

/** @brief Percussion decisions that hold for a whole track. */
struct PercPlan {
    int  layerOrder[kPercLanes] = {};   ///< lanes in the order they enter the groove
    int  layers = 0;                    ///< how many of them take part at most
    int  hatMode = 0;                   ///< 0 closed offbeat, 1 open offbeat + closed sixteenths, 2 closed offbeat + shaker
    bool clapBackbeat = true;           ///< clap on 2 and 4
    int  pulses[kPercLanes] = {};       ///< Euclidean onsets per bar for the Euclidean roles
    int  rotation[kPercLanes] = {};     ///< and their rotation
    float macro[3] = {};                ///< sound recipe: brightness, tightness, grit (-1..1)
    int  engineOverride[kPercLanes] = {};   ///< -1 = the knob
    int  modeSetOverride[kPercLanes] = {};  ///< -1 = the knob
    /**
     * @brief How many lanes at the head of layerOrder are hats (the closed hat, and the open hat or the
     *        shaker its hat mode leans on) -- 19.09.2026, round "arrangement".
     *
     * Behind them the order is fixed by the user's rule: "every 8 bars one percussion layer joins (clap
     * on 2 and 4, then congas, then ride)", then the rest by weight. The groove adds one layer per group
     * on top of the hats (Form.cpp).
     */
    int  hatLayers = 1;
};

/**
 * @brief Makes the percussion plan of a track.
 * @param p         knob values (compose.* and the lanes' role, active and density)
 * @param seed      seed of this track's percussion decisions
 * @param firstTrack the first track keeps the kit's sound exactly (no recipe, no overrides)
 * @param laneSeeds one seed per lane for its Euclidean pattern, or null to derive them from @p seed;
 *                  a pattern lane is a lockable unit (PLAN 6.8), so its pattern hangs off its own seed
 */
PercPlan makePercPlan(const ParamStore& p, uint64_t seed, bool firstTrack, const uint64_t* laneSeeds = nullptr);

/**
 * @brief What the form asks of the percussion in one bar (Form.h, the instrumentation matrix).
 *
 * Since Phase 5 the number of layers, the fills and the buildup's snare roll come from the section,
 * not from the bar number: percussion layers grow through an intro, return one per four bars in a
 * buildup and leave again through an outro.
 */
struct PercBarSpec {
    int   layers = 4;        ///< how many layers of the plan play
    bool  fills = true;      ///< fills allowed in this bar
    bool  hatsDense = false; ///< the closed hat plays every sixteenth (buildup)
    int   rollBar = -1;      ///< 0..rollBars-1: which bar of the buildup's snare roll this is
    bool  pdb = false;       ///< the pre-drop break: the roll ends on beat 3, beat 4 stays empty
    bool  crash = false;     ///< open the bar with a crash (a drop's downbeat)
    float cutBeats = 0.0f;   ///< beats of silence at the start of the bar (the cut)
    /** @name 19.09.2026, round "arrangement" (Form.h, BarPlan has the same fields)
     *  @{ */
    int   rollBars = 4;      ///< length of the roll: its quarters play 1/4, 1/8, 1/16 and 1/32 notes
    bool  quietHats = false; ///< a quiet closed hat on every sixteenth but the downbeat (the intro's first half)
    bool  shaker = false;    ///< the shaker plays whether or not it is a layer
    bool  offbeatHat = false;///< the closed hat plays its eighth offbeat whether or not it is a layer
    float hatLevel = 1.0f;   ///< velocity factor of what quietHats, shaker and offbeatHat add
    bool  openHats = false;  ///< the open hat on every offbeat (drop 2)
    bool  ride = false;      ///< the ride plays whether or not it is a layer (drop 2)
    int   cycleBar = -1;     ///< bar within the section's 32-bar cycle, -1 = unknown (the fill falls back on barInTrack)
    /** @} */
};

/**
 * @brief Composes one bar of percussion.
 * @param p          knob values
 * @param plan       the track's percussion plan
 * @param trackSeed  seed of the track
 * @param bar        absolute bar index
 * @param barInTrack bar index within the track
 * @param bpm        the track's tempo (for per-lane shifts in milliseconds)
 * @param keyRoot,scale the track's key, for tom runs in the mode
 * @param spec       what the form asks for in this bar
 * @param out        receives the notes (Part::Perc, lane, GM pitch plus any shift)
 */
void composePercBar(const ParamStore& p, const PercPlan& plan, uint64_t trackSeed, int bar, int barInTrack,
                    double bpm, int keyRoot, int scale, const PercBarSpec& spec, std::vector<NoteEvent>& out);

/**
 * @brief The fill a phrase ends with (exposed for tests).
 *
 * The phrase is the section's 32-bar cycle when @p cycleBar is known (19.09.2026): a fill closes every
 * eighth bar of it, the eighth bar itself always with a snare fill or a tom run (the user's micro rule,
 * "bar 8 a snare fill or tom run"), the sixteenth and the thirty-second with the snare roll. With
 * @p cycleBar -1 the bar of the track decides, as before.
 */
FillType chooseFill(const ParamStore& p, uint64_t trackSeed, int barInTrack, int cycleBar = -1);

/**
 * @brief Normalised offsets of the percussion recipe for one lane, indexed by perc::.
 * @param macro  brightness, tightness, grit
 * @param amount Sound Variation
 * @param out    perc::Count entries
 */
void percRecipeOffsets(const float* macro, float amount, float* out);

} // namespace phos
