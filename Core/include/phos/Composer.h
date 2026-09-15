/**
 * @file Composer.h
 * @brief The composer: tracks with their own key, tempo, patterns and sound; bars of kick and bass;
 *        and the conductor that feeds the engine.
 *
 * **Determinism.** Every decision comes from the set seed mixed with the index of the track, block,
 * phrase or bar it belongs to -- never from a random stream carried along. Track plans depend on
 * their predecessors (a key moves from the previous key, a sound keeps its distance from the last
 * ones), so they are computed in order once and cached; everything below a track is computed from
 * its plan and its own indices. Bar 3000 is bar 3000 whether the night was played from the start or
 * rendered in pieces.
 *
 * **A night that does not repeat.** Three knobs decide how far things move, each 0 = never:
 *  - Track Variation: key walks by fifths and whole tones, occasional change of mode, tempo drifting
 *    within Tempo Range around the knob with mean reversion and 16-bar ramps between tracks, the
 *    track's primary and secondary bass pattern, its gate, its length.
 *  - Sound Variation: a kick and a bass "recipe" per track, and slow filter arcs within the track.
 *  - Bass Variation: phrase figures (octaves, fifths, the flat second), switches to the secondary
 *    pattern for the last bars of a 16-bar block, and two-beat bass breaks that let the kick breathe.
 * The first track plays exactly the knobs; variation starts with the second.
 *
 * **Sound recipes.** A sound is a point in a small space of perceptual directions rather than a
 * random value per parameter: for the kick length, punch, body, grit and click; for the bass
 * brightness, pluck, squelch, grit and weight. The directions follow the dimensions timbre research
 * keeps finding -- attack, spectral centroid and spectral change over time (Grey 1977; McAdams,
 * Winsberg, Donnadieu, De Soete and Krimphoff 1995) -- and each is a fixed set of parameter moves in
 * the knobs' normalised domain, so a recipe is musically coherent (a longer kick is longer in hold,
 * decay and body together) and keeps its size whatever the knob ranges are. The mathematical
 * constraints are applied afterwards in the engine: the kick's tail limit at the first bass slot and
 * the kick lock.
 *
 * **Level match.** A short recipe, a thin pattern or a clean bass is quieter, and a set must not jump
 * several decibels at every track change (measured before this existed: 3.5 dB between neighbouring
 * tracks). When a track is planned, the composer renders two bars of its sound -- kick, primary bass
 * pattern, recipe and constraints -- in a private engine, measures the integrated loudness to
 * ITU-R BS.1770, and gives the track the gain that brings it to the first track's loudness, at most
 * +-9 dB. The melodic parts come and go inside a track, so they are not part of that measurement; each
 * of them is probed alone instead (two bars of its own line and sound) and brought to the loudness the
 * same part had in the first track, so an FM lead after a supersaw or a squelched acid after a dry one
 * does not jump either. The composer still never listens to the live output: the probe is a deterministic render of
 * its own plan. The measurement uses the knobs as they are when the plan is made.
 *
 * **Distance between tracks.** Each recipe is chosen by Mitchell's best-candidate method ("Spectrally
 * optimal sampling for distribution ray tracing", SIGGRAPH 1991): twelve candidates are drawn from a
 * truncated normal distribution, and the one farthest from the recipes of the previous four tracks
 * wins. Random choices cluster; best-candidate choices spread out like blue noise, so two consecutive
 * tracks never land on nearly the same sound, and the normal draw keeps the recipes from piling up
 * at the corners of the space.
 */
#pragma once
#include "phos/Clock.h"
#include "phos/Melody.h"
#include "phos/Rhythm.h"
#include "phos/Score.h"
#include <cstdint>
#include <vector>

namespace phos {

class Engine;
class ParamStore;

constexpr int kNumKickMacros = 5;   ///< length, punch, body, grit, click
constexpr int kNumBassMacros = 5;   ///< brightness, pluck, squelch, grit, weight
extern const char* const kKickMacroNames[kNumKickMacros];   ///< display names
extern const char* const kBassMacroNames[kNumBassMacros];   ///< display names

/** @brief Everything that is decided once per track. */
struct TrackPlan {
    int    index = 0;               ///< track number in the set
    int    firstBar = 0;            ///< bar the track starts on
    int    bars = 256;              ///< length in bars (a multiple of 16)
    int    key = 6;                 ///< pitch class of the key
    int    scale = 1;               ///< index into kScaleNames
    double bpm = 145.0;             ///< tempo the track settles on
    int    primaryPattern = 0;      ///< bass pattern most of the time
    int    secondaryPattern = 1;    ///< bass pattern for variations
    float  gate = 0.7f;             ///< bass note length as a fraction of the slot
    int    kickEngine = -1;         ///< override of kick.engine, -1 = the knob
    int    kickClip = -1;           ///< override of kick.clip, -1 = the knob
    float  kickMacro[kNumKickMacros] = {};   ///< recipe, each -1..1
    float  bassMacro[kNumBassMacros] = {};   ///< recipe, each -1..1
    double loudness = 0.0;          ///< probe loudness of the track's sound, LUFS (0 when Level Match is off)
    float  gainDb = 0.0f;           ///< level correction against the first track
    uint64_t percSeed = 0;          ///< seed of the track's percussion decisions
    PercPlan perc;                  ///< the track's percussion plan (Rhythm.h)
    uint64_t melodySeed = 0;        ///< seed of the track's melodic decisions
    MelodyPlan melody;              ///< chords, acid, lead, arp and their schedule (Melody.h)
    double partLoudness[kMelodyParts] = {};   ///< probe loudness of each melodic part alone, LUFS
    float  partGainDb[kMelodyParts] = {};     ///< level correction of each melodic part against the first track's
};

/** @brief Composes the set from a seed and the knobs. */
class Composer {
public:
    /** @brief @p seed identifies the set. */
    explicit Composer(uint64_t seed = 1) : seed_(seed) {}
    /** @brief Changes the set seed (forgets cached plans). */
    void setSeed(uint64_t seed) { seed_ = seed; plans_.clear(); }
    /** @brief The set seed. */
    uint64_t seed() const { return seed_; }

    /** @brief The plan of track @p index (computed in order and cached). */
    const TrackPlan& track(const ParamStore& params, int index) const;
    /** @brief Index of the track that contains @p bar. */
    int trackOfBar(const ParamStore& params, int bar) const;
    /** @brief The tempo map of the first @p bars bars: held per track, ramped over the last 16 bars into the next. */
    TempoMap tempoMap(const ParamStore& params, int bars) const;

    /**
     * @brief Composes whole bars.
     * @param params   knob values (compose.* and the kick and bass knobs the recipes start from)
     * @param firstBar index of the first bar (bar 0 starts at beat 0)
     * @param count    number of bars
     * @param notes    receives the notes, sorted
     * @param controls if not null, receives the sound changes, sorted
     */
    void composeBars(const ParamStore& params, int firstBar, int count, std::vector<NoteEvent>& notes,
                     std::vector<ControlEvent>* controls = nullptr) const;

    /**
     * @brief Normalised parameter offsets of a recipe.
     * @param kickModule true for the kick table, false for the bass table
     * @param macros     kNumKickMacros or kNumBassMacros values
     * @param amount     Sound Variation
     * @param out        receives the offset per parameter of the module, indexed like its table
     *                   (must hold ParamStore::moduleCount entries)
     */
    static void recipeOffsets(bool kickModule, const float* macros, float amount, float* out);

private:
    void validate(const ParamStore& params) const;
    TrackPlan makeTrack(const ParamStore& params, int index) const;
    void trackStartControls(const ParamStore& params, const TrackPlan& plan, double beat, std::vector<ControlEvent>& out) const;
    void arcControls(const ParamStore& params, const TrackPlan& plan, int inTrack, double beat, bool ramp, std::vector<ControlEvent>& out) const;
    double probeLoudness(const ParamStore& params, const TrackPlan& plan, int part = -1) const;
    void melodyControls(const ParamStore& params, const TrackPlan& plan, int inTrack, double beat, std::vector<ControlEvent>& out) const;

    uint64_t seed_;
    mutable std::vector<TrackPlan> plans_;
    mutable std::vector<float> planKnobs_;
};

/**
 * @brief Keeps the engine's event rings filled a few bars ahead of the play position.
 *
 * In the plugin this runs on the composer thread; the offline renderer calls pump() before every
 * block. Bars are pushed whole and in order; what does not fit waits for the next call.
 */
class Conductor {
public:
    /** @brief Binds an engine and a composer. */
    Conductor(Engine& engine, const Composer& composer);
    /** @brief Starts again from bar 0. */
    void rewind();
    /**
     * @brief Composes and pushes bars until @p horizonBeats beyond the engine's position are covered.
     * @param params       knob values to compose with
     * @param horizonBeats how far ahead to keep the rings filled
     * @param record       if not null, every pushed note is appended here as well (for MIDI export)
     */
    void pump(const ParamStore& params, double horizonBeats, std::vector<NoteEvent>* record = nullptr);
    /** @brief Index of the next bar to be composed. */
    int nextBar() const { return nextBar_; }

private:
    Engine& engine_;
    const Composer& composer_;
    int nextBar_ = 0;
    std::vector<NoteEvent> notes_;
    std::vector<ControlEvent> controls_;
    size_t notePos_ = 0, controlPos_ = 0;
};

} // namespace phos
