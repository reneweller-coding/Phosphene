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
};

/**
 * @brief Makes the percussion plan of a track.
 * @param p         knob values (compose.* and the lanes' role, active and density)
 * @param seed      seed of this track's percussion decisions
 * @param firstTrack the first track keeps the kit's sound exactly (no recipe, no overrides)
 */
PercPlan makePercPlan(const ParamStore& p, uint64_t seed, bool firstTrack);

/** @brief Lanes (by index) that take part in the groove in a given bar of a track. */
int activeLayers(const PercPlan& plan, float percVariation, uint64_t trackSeed, int barInTrack);

/**
 * @brief Composes one bar of percussion.
 * @param p          knob values
 * @param plan       the track's percussion plan
 * @param trackSeed  seed of the track
 * @param bar        absolute bar index
 * @param barInTrack bar index within the track
 * @param bpm        the track's tempo (for per-lane shifts in milliseconds)
 * @param keyRoot,scale the track's key, for tom runs in the mode
 * @param withFills  false for the level-match probe
 * @param out        receives the notes (Part::Perc, lane, GM pitch plus any shift)
 */
void composePercBar(const ParamStore& p, const PercPlan& plan, uint64_t trackSeed, int bar, int barInTrack,
                    double bpm, int keyRoot, int scale, bool withFills, std::vector<NoteEvent>& out);

/** @brief The fill a phrase ends with (exposed for tests). */
FillType chooseFill(const ParamStore& p, uint64_t trackSeed, int barInTrack);

/**
 * @brief Normalised offsets of the percussion recipe for one lane, indexed by perc::.
 * @param macro  brightness, tightness, grit
 * @param amount Sound Variation
 * @param out    perc::Count entries
 */
void percRecipeOffsets(const float* macro, float amount, float* out);

} // namespace phos
