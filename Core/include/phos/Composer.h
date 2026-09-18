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
 * does not jump either.
 *
 * **Loudness target.** With master.auto_gain on, a last probe renders the whole mix -- eight bars spread
 * evenly over the track as its blocks really play, with every correction, through the bus compressor,
 * the clipper and the limiter -- and the master gain is offset so that the output meets
 * master.target_lufs (measured on five seeds: within 0.6 LU). The limiter makes the
 * relation between gain and loudness non-linear, so the probe runs twice: once to measure, once at the
 * corrected gain, and the second reading refines the offset (a secant step). The composer still never listens to the live output: the probe is a deterministic render of
 * its own plan. The measurement uses the knobs as they are when the plan is made.
 *
 * **Distance between tracks.** Each recipe is chosen by Mitchell's best-candidate method ("Spectrally
 * optimal sampling for distribution ray tracing", SIGGRAPH 1991): twelve candidates are drawn from a
 * truncated normal distribution, and the one farthest from the recipes of the previous four tracks
 * wins. Random choices cluster; best-candidate choices spread out like blue noise, so two consecutive
 * tracks never land on nearly the same sound, and the normal draw keeps the recipes from piling up
 * at the corners of the space.
 *
 * **The set walk and the track's own decisions (Phase 5).** What belongs to the journey through the
 * night -- the length, key, mode and tempo of every track and its two sound recipes -- is a walk
 * computed from the set seed alone, track after track. What belongs to a single track -- its form, its
 * percussion, its melodic material, its bass patterns -- hangs off that track's own seed. The split is
 * what makes rerolling work (PLAN 6.8): rerolling one track leaves every other track bit-identical,
 * because no later track reads anything the rerolled one decided, and a locked unit keeps the seed it
 * had before any reroll.
 *
 * **Form.** Since Phase 5 a track is a sequence of sections drawn from a weighted grammar (Form.h), and
 * the instrumentation matrix of that form decides bar by bar what plays. The composer turns the
 * sections into control events: the energy of a section moves the track gain by at most +-2 dB, the
 * acid and lead cutoffs, the pad's table position and the hall sends, each as a ramp over the section
 * (Farbood's four quantities; timbre as narrative after Farrell).
 */
#pragma once
#include "phos/Clock.h"
#include "phos/Form.h"
#include "phos/Melody.h"
#include "phos/Rhythm.h"
#include "phos/Score.h"
#include <cstdint>
#include <map>
#include <vector>

namespace phos {

class Engine;
class ParamStore;

/** @brief The units that can be locked and rerolled (PLAN 6.8). */
enum class LockUnit : int { Set = 0, Track, Section, PatternLane, Count };
constexpr int kNumLockUnits = static_cast<int>(LockUnit::Count);   ///< number of lockable unit kinds
extern const char* const kLockUnitNames[kNumLockUnits];            ///< "set", "track", "section", "lane"

/** @brief Index of a section unit: the section @p si of track @p track. */
constexpr int sectionUnitIndex(int track, int si) { return track * kMaxSections + si; }
/** @brief Index of a pattern-lane unit: percussion lane @p lane of track @p track. */
constexpr int laneUnitIndex(int track, int lane) { return track * kPercLanes + lane; }

constexpr int kNumKickMacros = 5;   ///< length, punch, body, grit, click
constexpr int kNumBassMacros = 5;   ///< brightness, pluck, squelch, grit, weight

/**
 * @brief Bars of the learned bass phrase (compose.bass_model = Neural).
 *
 * **Eight, and the number was measured rather than chosen.** The model's `bars` input is what it was
 * told about the loop it was reading, and the corpus taught it that a short bass loop is a static one
 * and a long one moves. Sampled on the composer's own rolling rhythm and constraint set, the trained
 * model plays the root in 79 % of a four-bar phrase and in 58 % of an eight-bar one, against 53 % for
 * real psytrance bass lines whose every bar is that same rolling figure
 * (`Tools/train/bass_stats.py`, and the control runs recorded in docs/PLAN.md 6.9). Four bars is
 * therefore the one length at which the learned bass would come out barely less static than the
 * pattern families it replaces. Eight is also the length of the form's group (Form.cpp), so the bass
 * repeats on the same boundary everything else does.
 */
constexpr int kBassPhraseBars = 8;
/** @brief Slots of a bass phrase: eight bars, four beats, at most three notes a beat (Patterns.h). */
constexpr int kBassPhraseSlots = kBassPhraseBars * 4 * 3;
extern const char* const kKickMacroNames[kNumKickMacros];   ///< display names
extern const char* const kBassMacroNames[kNumBassMacros];   ///< display names

/** @brief What the set walk decides for a track: the journey through the night. */
struct TrackWalk {
    int    bars = 256;              ///< length in bars (a multiple of 32)
    int    key = 6;                 ///< pitch class of the key
    int    scale = 1;               ///< index into kScaleNames
    double bpm = 145.0;             ///< tempo the track settles on
    float  kickMacro[5] = {};       ///< kick recipe, each -1..1
    float  bassMacro[5] = {};       ///< bass recipe, each -1..1
};

/** @brief Everything that is decided once per track. */
struct TrackPlan {
    int    index = 0;               ///< track number in the set
    int    firstBar = 0;            ///< bar the track starts on
    int    bars = 256;              ///< length in bars (a multiple of 32)
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
    MelodyPlan melody;              ///< chords, acid, lead, arp and pad material (Melody.h)
    uint64_t formSeed = 0;          ///< seed of the track's form
    FormPlan form;                  ///< sections, their energies and the effects on their boundaries
    uint64_t sectionSeed[kMaxSections] = {};   ///< seed of each section (lockable, rerollable)
    float  arcIn = 0.7f, arcOut = 0.7f;        ///< the set's energy arc where the track starts and ends
    double partLoudness[kMelodyParts] = {};   ///< probe loudness of each melodic part alone, LUFS
    float  partGainDb[kMelodyParts] = {};     ///< level correction of each melodic part against the first track's
    double mixLoudness = 0.0;       ///< probe loudness of the whole mix after the master, before the loudness offset
    float  masterGainDb = 0.0f;     ///< the offset that brings the mix to master.target_lufs (Auto Gain)
    /** @name The learned bass phrase (compose.bass_model = Neural; PLAN 6.9, stage B, role 3)
     *  Two phrases of kBassPhraseBars bars, one for the track's primary and one for its secondary
     *  bass pattern, drawn once per track on the composer's thread. Each entry is the interval in semitones from
     *  the bass root of the note in slot `s` of beat `beat` of bar `barInPhrase`, at index
     *  `(barInPhrase * 4 + beat) * 3 + s`. `bassNeural` is false whenever the knob says Pattern or no
     *  weight file was found, and then nothing here is read and the bass is what it always was.
     *  @{ */
    bool   bassNeural = false;                       ///< whether the phrases below were drawn
    int8_t bassRel[2][kBassPhraseSlots] = {};        ///< [primary, secondary][slot] semitones from the root
    /** @} */
    /** @name The drawn bass rhythm (compose.bass_rhythm = Corpus; Corpus.h, BassRhythm)
     *  Two rhythm phrases of kBassPhraseBars bars, one for the track's primary and one for its
     *  secondary slot, drawn once per track from the track's own seed. Each entry is a 16-bit onset
     *  mask over the sixteenths of a bar, never with an onset on a kick step. `bassRhythm` is false
     *  whenever the knob says Pattern, and then nothing here is read and the bass plays the five
     *  pattern families of Patterns.h exactly as it always did -- bit for bit, which the self test
     *  section `bassRhythm` measures against a render made with the knob off.
     *  @{ */
    bool     bassRhythm = false;                       ///< whether the masks below are used
    uint16_t bassMask[2][kBassPhraseBars] = {};        ///< [primary, secondary][bar in phrase] onset mask
    float    bassShortestSlot = 0.25f;                 ///< tightest note span in the two phrases, in beats
    /** @} */
};

/** @brief Composes the set from a seed and the knobs. */
class Composer {
public:
    /** @brief @p seed identifies the set. */
    explicit Composer(uint64_t seed = 1) : seed_(seed) {}
    /** @brief Changes the set seed (forgets cached plans). */
    void setSeed(uint64_t seed) { seed_ = seed; plans_.clear(); walk_.clear(); }
    /** @brief The set seed. */
    uint64_t seed() const { return seed_; }

    /** @brief The plan of track @p index (computed in order and cached). */
    const TrackPlan& track(const ParamStore& params, int index) const;
    /** @brief Index of the track that contains @p bar. */
    int trackOfBar(const ParamStore& params, int bar) const;
    /** @brief The tempo map of the first @p bars bars: held per track, ramped over the last 16 bars into the next. */
    TempoMap tempoMap(const ParamStore& params, int bars) const;
    /** @brief Every section mark of the first @p bars bars, for the MIDI export and the arrange view. */
    std::vector<SectionMark> sections(const ParamStore& params, int bars) const;

    /** @name Locks and rerolls (PLAN 6.8)
     *  A locked unit keeps the seed it had before any reroll; rerolling a unit advances its own
     *  variation counter, which is mixed into its seed and into nothing else.
     *  @{ */
    /** @brief Locks or unlocks a unit. */
    void setLock(LockUnit unit, int index, bool locked);
    /** @brief Whether a unit is locked. */
    bool isLocked(LockUnit unit, int index) const;
    /** @brief Advances the variation counter of an unlocked unit (a locked one does not move). */
    void reroll(LockUnit unit, int index);
    /** @brief The variation counter of a unit (0 = never rerolled). */
    uint32_t variation(LockUnit unit, int index) const;
    /** @brief Sets a variation counter directly (used when a .phosset is read). */
    void setVariation(LockUnit unit, int index, uint32_t value);
    /** @brief Forgets every lock and every reroll. */
    void clearLocks();
    /** @brief The locks, for serialisation: unit -> indices. */
    const std::map<int, uint8_t>& locks(LockUnit unit) const { return locked_[static_cast<int>(unit)]; }
    /** @brief The variation counters, for serialisation. */
    const std::map<int, uint32_t>& variations(LockUnit unit) const { return variation_[static_cast<int>(unit)]; }
    /** @} */

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
    /** @brief Draws the track's two learned bass phrases, or leaves the plan on the pattern families. */
    void makeBassPhrases(const ParamStore& params, TrackPlan& plan) const;
    /** @brief Draws the track's two bass *rhythm* phrases, or leaves the plan on the pattern families. */
    void makeBassRhythm(const ParamStore& params, TrackPlan& plan) const;
    const TrackWalk& walkAt(const ParamStore& params, int index) const;
    void trackStartControls(const ParamStore& params, const TrackPlan& plan, double beat, std::vector<ControlEvent>& out) const;
    void arcControls(const ParamStore& params, const TrackPlan& plan, int inTrack, double beat, bool ramp, std::vector<ControlEvent>& out) const;
    double probeLoudness(const ParamStore& params, const TrackPlan& plan, int part = -1, float masterGainDb = 0.0f) const;
    void matchMaster(const ParamStore& params, TrackPlan& plan) const;
    void sectionControls(const ParamStore& params, const TrackPlan& plan, const BarPlan& bar, double beat,
                         std::vector<ControlEvent>& out) const;
    void transitionBar(const ParamStore& params, int track, int inTrack, int bar, std::vector<NoteEvent>& out) const;
    /** @brief The set seed after the set unit's rerolls. */
    uint64_t setSeed() const;
    /** @brief The seed of a track: frozen at the original when the track is locked. */
    uint64_t trackSeed(int index) const;
    /** @brief The seed of one section of a track. */
    uint64_t sectionSeedOf(int track, int si) const;
    /** @brief The seed of one percussion lane of a track. */
    uint64_t laneSeedOf(int track, int lane) const;

    uint64_t seed_;
    mutable std::vector<TrackPlan> plans_;
    mutable std::vector<TrackWalk> walk_;
    mutable std::vector<float> planKnobs_;
    mutable bool bassModelReported_ = false;           ///< the missing-weight-file line is printed once
    std::map<int, uint8_t> locked_[kNumLockUnits];     ///< unit index -> locked
    std::map<int, uint32_t> variation_[kNumLockUnits]; ///< unit index -> reroll counter
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
