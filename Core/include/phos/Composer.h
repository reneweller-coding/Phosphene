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
 * **Per-track character (19.09.2026).** The user's wish: the tracks must not all sound alike. The bass
 * directions now carry the four characters of that round's brief -- clean sub plus bite at the
 * centre, gritty (grit), rubbery and resonant (squelch), plucky and short (pluck) -- including the new
 * bite layer's parameters, with three to four times the old weights; the kick's click and end pitch
 * reach as far as the reference kicks spread, its length less far than before, because the reference
 * kicks hardly spread there. The acid gets a third recipe: a point in the
 * triangle of its three voicings (NumAcidVoicings), drawn per track by the same best-candidate rule
 * and applied as offsets from the knobs (acidVoicingOffsets). Measured over twenty tracks of the
 * listening seed (testRecipeSpread): the kick's click band spreads 4.8 dB between its quartiles (3.1
 * with the old table), the bass's pluck 9.9 dB (6.0).
 *
 * **Level match.** A short recipe, a thin pattern or a clean bass is quieter, and a set must not jump
 * several decibels at every track change (measured before this existed: 3.5 dB between neighbouring
 * tracks). When a track is planned, the composer renders two bars of its sound -- kick, primary bass
 * pattern, recipe and constraints -- in a private engine, measures the integrated loudness to
 * ITU-R BS.1770, and gives the track the gain that brings it to the first track's loudness, at most
 * +-9 dB. The probe's percussion plays as many layers as the track's first core does (since
 * 19.09.2026; before, all the track has, which over-read the tracks by different amounts). The
 * melodic parts come and go inside a track, so they are not part of that measurement; each
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
 *
 * **Knobs that move while the composer works (25.09.2026).** The plans depend on the knobs planKnobIds()
 * lists, and are thrown away when one of them moves. A call checks them once, when it begins -- the outermost
 * call, not the track() and trackOfBar() it makes itself -- so everything one call reads comes from one set of
 * plans, and a change takes effect at the next call: for Conductor::pump, which composes a bar per call, at the
 * next bar. Until this date every inner call checked again; a host's automation writing a knob while
 * composeBars ran threw the plans away up to five times inside one bar, the track index it had found no longer
 * belonged to the plan it read, and bars outside a plan's range read its arrays out of bounds (the host test's
 * crash after 1 h 46 min, docs/rounds/2026-09.md). The plans also live in a std::deque now, so planning a later
 * track never moves an earlier one. A knob that changes during the call is still read by the plans that call
 * makes; a host that wants a plan made from one consistent set of knobs hands the composer a snapshot (the
 * plugin does, PluginProcessor.h, refreshComposeParams).
 * @author Rene Weller
 */
#pragma once
#include "phos/Clock.h"
#include "phos/Form.h"
#include "phos/Melody.h"
#include "phos/Probe.h"   // how the probe renders are scheduled; a program that plans tracks may opt in there
#include "phos/Rhythm.h"
#include "phos/Score.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace phos
{

class Engine;
class ParamStore;

/** @brief The units that can be locked and rerolled (PLAN 6.8). */
enum class LockUnitE : int
{
    SET = 0,
    TRACK,
    SECTION,
    PATTERN_LANE,
    COUNT
};
constexpr int            NumLockUnits = static_cast<int>( LockUnitE::COUNT );   ///< number of lockable unit kinds
extern const char *const LockUnitNames[NumLockUnits];                           ///< "set", "track", "section", "lane"

/**
 * @brief Index of a section unit: the section @p si of track @p track.
 * @param[in] track  the track
 * @param[in] si     the section
 * @return the unit's index
 */
constexpr int sectionUnitIndex( int track, int si )
{
    return track * MaxSections + si;
}
/**
 * @brief Index of a pattern-lane unit: percussion lane @p lane of track @p track.
 * @param[in] track  the track
 * @param[in] lane   the percussion lane
 * @return the unit's index
 */
constexpr int laneUnitIndex( int track, int lane )
{
    return track * PercLanes + lane;
}

constexpr int NumKickMacros = 5;   ///< length, punch, body, grit, click
constexpr int NumBassMacros = 5;   ///< brightness, pluck, squelch, grit, weight

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
constexpr int BassPhraseBars = 8;
/** @brief Slots of a bass phrase: eight bars, four beats, at most three notes a beat (Patterns.h). */
constexpr int            BassPhraseSlots = BassPhraseBars * 4 * 3;
extern const char *const KickMacroNames[NumKickMacros];   ///< display names
extern const char *const BassMacroNames[NumBassMacros];   ///< display names

/**
 * @brief The acid voicings a track's acid sound is interpolated between (19.09.2026).
 *
 * The three candidates the user heard on 18.09.2026 (docs/rounds/2026-09.md, "Fundament und Mix"): a clean,
 * round 303 ("clean"), the 303 into a distortion pedal that became the default ("driven"), and the
 * half-pulse, dispersed Goa acid ("liquid"). A track's acid is a point in the triangle they span --
 * barycentric weights, one per voicing, summing to 1 -- so that tracks differ by more than three
 * presets and any blend is a legal sound. Driven is the knobs; the first track is driven exactly.
 */
constexpr int            NumAcidVoicings = 3;
extern const char *const AcidVoicingNames[NumAcidVoicings];   ///< "clean", "driven", "liquid"

/**
 * @brief The sound of one polyphonic voice in one track (19.09.2026, round "voices").
 *
 * The user: "Auch die anderen Synthesizer können gerne mehr verschiedene Klangfarben haben, es sollen
 * ja nicht alle Stücke gleich klingen." Without recipes a track changed the pad's table position and
 * gate pattern and nothing else of it, the lead's detune and cutoff, the arp's filter decay and detune
 * -- measured on the listening seed, every pad of the set sounded the same. A recipe is drawn per voice
 * and per track from the set seed and the style: three discrete choices -- the oscillator, the wavetable
 * (from the voice's own palette of the built-in and the library tables, ComposerInternal.h, VoicePalettes)
 * and the filter's response -- and five perceptual directions in the knobs' normalised domain, the
 * attack, spectral-centroid and spectral-flux axes timbre research keeps finding (Grey 1977; McAdams et
 * al. 1995): brightness, softness, thickness, space and motion. Like the kick and bass recipes they are
 * offsets from the knobs, and the first track plays the knobs exactly.
 */
constexpr int            NumVoiceMacros = 5;                ///< brightness, softness, thickness, space, motion
extern const char *const VoiceMacroNames[NumVoiceMacros];   ///< display names
/** @brief One polyphonic voice's sound for one track: discrete choices and the five perceptual directions (see above). */
struct VoiceRecipe
{
    int m_osc = -1;      ///< override of poly.osc, -1 = the knob
    int m_table = -1;    ///< override of poly.table, -1 = the knob
    int m_filter = -1;   ///< override of poly.filter_type, -1 = the knob
    int m_delayL = -1;   ///< override of the left delay time, -1 = the knob
    int m_delayR = -1;   ///< ... of the right one
    /** @name The second oscillator (22.09.2026, round "Klangfarben"; Params.h, poly::Osc2)
     *  Two discrete choices, because that is what they are: *which* oscillator answers the first one
     *  and *at which interval*. The mix between them is continuous and rides on the thickness
     *  direction like everything else that is a matter of degree (VoiceLoadings).
     *  @{ */
    int  m_osc2 = -1;         ///< override of poly.osc2 (PolyOsc2, 0 = off), -1 = the knob
    int  m_osc2Semis = 0;     ///< override of poly.osc2_interval (PolyOsc2Interval)
    bool m_hasOsc2 = false;   ///< whether the two above were drawn at all
    /** @} */
    float m_macro[NumVoiceMacros] = {};   ///< the five directions, each -1..1
};

struct SoundPreset;   // SoundPresets.h
/** @brief The synths a track's sound presets are for, in the order of the own-sound switches: kick, bass, acid, the six voices. */
constexpr int SoundSynths = 9;

/** @brief What the set walk decides for a track: the journey through the night. */
struct TrackWalk
{
    int m_bars = 256;   ///< length in bars (a multiple of 32)
    /** @brief The set's motif and this track's part in it (Melody.h, SetMotif; 23.09.2026): the first track
     *         states it in its first lead phrase, the track that carries the set's end recalls it in its
     *         second, a few tracks in between recall it too. */
    SetMotif    m_motif;
    int         m_key = 6;       ///< pitch class of the key
    int         m_scale = 1;     ///< index into ScaleNames
    double      m_bpm = 145.0;   ///< tempo the track settles on
    int         m_style = 1;     ///< StyleId of the track (22.09.2026: the style walks when compose.style_mix is on)
    float       m_kickMacro[5] = {};                                     ///< kick recipe, each -1..1
    float       m_bassMacro[5] = {};                                     ///< bass recipe, each -1..1
    float       m_acidVoicing[NumAcidVoicings] = { 0.0f, 1.0f, 0.0f };   ///< barycentric weights of the acid voicings
    VoiceRecipe m_voice[PolyInstances];   ///< the sound of each polyphonic voice (PolyInstance order)
    int         m_soundPreset[SoundSynths] = {
                -1, -1, -1, -1, -1, -1, -1, -1, -1
    };   ///< the track's preset of each synth (SoundSynths order)
};

/** @brief Everything that is decided once per track. */
struct TrackPlan
{
    int    m_index = 0;                       ///< track number in the set
    int    m_firstBar = 0;                    ///< bar the track starts on
    int    m_bars = 256;                      ///< length in bars (a multiple of 32)
    int    m_key = 6;                         ///< pitch class of the key
    int    m_scale = 1;                       ///< index into ScaleNames
    double m_bpm = 145.0;                     ///< tempo the track settles on
    int    m_style = 1;                       ///< StyleId the track plays in (from the walk)
    int    m_primaryPattern = 0;              ///< bass pattern most of the time
    int    m_secondaryPattern = 1;            ///< bass pattern for variations
    float  m_gate = 0.7f;                     ///< bass note length as a fraction of the slot
    int    m_kickEngine = -1;                 ///< override of kick.engine, -1 = the knob
    int    m_kickClip = -1;                   ///< override of kick.clip, -1 = the knob
    float  m_kickMacro[NumKickMacros] = {};   ///< recipe, each -1..1
    float  m_bassMacro[NumBassMacros] = {};   ///< recipe, each -1..1
    float  m_acidVoicing[NumAcidVoicings] = {
        0.0f, 1.0f, 0.0f
    };   ///< barycentric weights of the acid voicings (clean, driven, liquid)
    VoiceRecipe m_voice[PolyInstances];   ///< the sound of each polyphonic voice (PolyInstance order; 19.09.2026)
    /**
     * @brief The track's sound of each synth as a preset of the bank (26.09.2026; SoundPresets.h): an index into
     *        factoryPresets() of kick, bass, acid and the six voices (SoundSynths order), -1 for the knobs.
     *
     * The user: "Könnte der Composer dann beim Generieren der Stücke auch aus diesen Presets auswählen und die
     * entsprechenden Presets im jeweiligen Synthesizer anzeigen?" -- and, having listened: "Stellt der Composer auch
     * die Preset-Werte auf Absolutwerte ... ein?" A preset replaces the recipe of its synth (the recipe's offsets are
     * zero where one plays) and arrives absolute (ControlEvent::Kind::Base); the section rides stay offsets on it.
     */
    int        m_soundPreset[SoundSynths] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    double     m_loudness = 0.0;   ///< probe loudness of the track's sound, LUFS (0 when Level Match is off)
    float      m_gainDb = 0.0f;    ///< level correction against the first track
    uint64_t   m_percSeed = 0;     ///< seed of the track's percussion decisions
    PercPlan   m_perc;             ///< the track's percussion plan (Rhythm.h)
    uint64_t   m_melodySeed = 0;   ///< seed of the track's melodic decisions
    MelodyPlan m_melody;           ///< chords, acid, lead, arp and pad material (Melody.h)
    uint64_t   m_formSeed = 0;     ///< seed of the track's form
    FormPlan   m_form;             ///< sections, their energies and the effects on their boundaries
    uint64_t   m_sectionSeed[MaxSections] = {};    ///< seed of each section (lockable, rerollable)
    float      m_arcIn = 0.7f;                     ///< the set's energy arc where the track starts
    float      m_arcOut = 0.7f;                    ///< ... and where it ends
    double     m_partLoudness[MelodyParts] = {};   ///< probe loudness of each melodic part alone, LUFS
    /**
     * @name The level match's reference (26.09.2026)
     * What every track is matched against: the first track's loudness and its parts' *with the composer's presets off*
     * -- the recipes' sound the mix was calibrated on. Where the first track plays presets it is matched to these too
     * (it used to be the reference as it stood, and with presets a reference drawn by chance: the Full-On breakdown's
     * counter came out 7 dB under the calibrated one). Equal to loudness and partLoudness where it plays none.
     * @{ */
    double m_refLoudness = 0.0;                   ///< the first track's loudness with the presets off, LUFS
    double m_refPartLoudness[MelodyParts] = {};   ///< ... and its parts', LUFS
    /** @} */
    float m_partGainDb[MelodyParts] = {};   ///< level correction of each melodic part against the first track's
    /**
     * @brief The master offset has not been measured yet; it arrives later (22.09.2026).
     *
     * Only ever true when a host asked for it (Composer::setDeferMasterGain). Auto Gain is three
     * quarters of the time it takes to plan a track -- two whole-mix renders with a secant step --
     * and a plugin that waits for it shows nine seconds of silence after the button. With this the
     * track can play as soon as the level match is done, about two, and the offset is measured
     * behind it and rides in over a ramp while the intro plays.
     */
    bool m_masterDeferred = false;
    /**
     * @brief The level, presence and audibility measurements of this plan are still to come (23.09.2026).
     *
     * Only ever true in a live set (Composer::setDeferMasterGain; the first track since 23.09.2026, every track since
     * 26.09.2026): the plugin plays at once and measures behind the music (Composer::completeMeasurement), for the
     * track that plays and for the next one before it comes. Until then every correction of the plan is zero --
     * for the first track the right value everywhere but on the lines (presence, audibility) and the master,
     * because it is the level match's reference; a later track measured in time never plays without its own.
     */
    bool m_measureDeferred = false;
    /** @brief completeMeasurement() changed the lines' corrections, and the host has not sent them yet. */
    bool   m_correctionsPending = false;
    double m_mixLoudness = 0.0;     ///< probe loudness of the whole mix after the master, before the loudness offset
    float  m_masterGainDb = 0.0f;   ///< the offset that brings the mix to master.target_lufs (Auto Gain)
    /** @name The presence match (19.09.2026, round "polish"; ComposerLevels.cpp, matchPresence)
     *  @{ */
    double m_presenceDb = 0.0;   ///< the drops' presence estimate before the match, dB against the reference median
    double m_presenceAfterDb =
        0.0;   ///< the same estimate with presenceGainDb on the lines (the match's own prediction)
    float m_presenceGainDb = 0.0f;   ///< the gain the match puts on the lines (lead, counter, arp, stab), dB
    /** @name The audibility match (23.09.2026; ComposerLevels.cpp, matchAudibility)
     *  @{ */
    double m_audibleInMix
        [MelodyParts] = {};   ///< each part's partial loudness in the drops' mix (Audibility.h), before the lift
    float m_audibilityLiftDb[MelodyParts] = {};   ///< what the match added to partGainDb
    /** @} */
    /** @} */
    /** @name The learned bass phrase (compose.bass_model = Neural; PLAN 6.9, stage B, role 3)
     *  Two phrases of BassPhraseBars bars, one for the track's primary and one for its secondary
     *  bass pattern, drawn once per track on the composer's thread. Each entry is the interval in semitones from
     *  the bass root of the note in slot `s` of beat `beat` of bar `barInPhrase`, at index
     *  `(barInPhrase * 4 + beat) * 3 + s`. `bassNeural` is false whenever the knob says Pattern or no
     *  weight file was found, and then nothing here is read and the bass is what it always was.
     *  @{ */
    bool   m_bassNeural = false;                 ///< whether the phrases below were drawn
    int8_t m_bassRel[2][BassPhraseSlots] = {};   ///< [primary, secondary][slot] semitones from the root
    /** @} */
    /** @name The drawn bass rhythm (compose.bass_rhythm = Corpus; Corpus.h, BassRhythm)
     *  Two rhythm phrases of BassPhraseBars bars, one for the track's primary and one for its
     *  secondary slot, drawn once per track from the track's own seed. Each entry is a 16-bit onset
     *  mask over the sixteenths of a bar, never with an onset on a kick step. `bassRhythm` is false
     *  whenever the knob says Pattern, and then nothing here is read and the bass plays the five
     *  pattern families of Patterns.h exactly as it always did -- bit for bit, which the self test
     *  section `bassRhythm` measures against a render made with the knob off.
     *  @{ */
    bool     m_bassRhythm = false;                 ///< whether the masks below are used
    uint16_t m_bassMask[2][BassPhraseBars] = {};   ///< [primary, secondary][bar in phrase] onset mask
    float    m_bassShortestSlot = 0.25f;           ///< tightest note span in the two phrases, in beats
    /** @} */
};

/**
 * @brief Which half of a track's sound a batch of control events writes (19.09.2026, round "arrangement").
 *
 * Over the DJ overlap two tracks sound at once (Form.h, DjOverlap): the incoming track's voices -- pads,
 * drone, the polyphonic recipes and levels -- from its first bar, its floor -- key, kick, bass, kit, acid,
 * track and master gain, the section rides -- only from the hand-over, where its kick and bass take over
 * from the outgoing track's. A track that starts the set writes both at once (All).
 */
enum class ControlScopeE : int
{
    ALL = 0,
    VOICES,
    FLOOR
};

/**
 * @brief The bar on which a track's own kick and bass take over the floor (its first bar in a set's first track).
 * @param[in] t  the track's plan
 * @return the bar
 */
inline int handoverBar( const TrackPlan &t )
{
    return t.m_firstBar + t.m_form.m_handover;
}

/** @brief Composes the set from a seed and the knobs. */
class Composer
{
public:
    /**
     * @brief @p seed identifies the set.
     * @param[in] seed  the set's seed
     */
    explicit Composer( uint64_t seed = 1 ) : m_seed( seed ) {}
    /**
     * @brief Changes the set seed (forgets cached plans).
     * @param[in] seed  the set's seed
     */
    void setSeed( uint64_t seed )
    {
        m_seed = seed;
        m_plans.clear();
        m_walk.clear();
        ++m_planGeneration;
    }
    /**
     * @brief The family jam (02.10.2026, the plugin's Jam.h), composer thread: while @p hold the tracks walked from now
     *        on keep the key of the track before them -- a follower's engine moves the whole set to the leader's root,
     *        and so the track that blends in by as much as the one that blends out -- and take the scale @p scale
     *        (-1: their own walk's). The tracks already walked stay as they are; the walk's draws keep their place.
     * @param[in] hold   true to keep the key
     * @param[in] scale  the scale to keep to, -1 for the walk's
     */
    void setJam( bool hold, int scale )
    {
        m_jamHold = hold;
        m_jamScale = scale;
    }
    /**
     * @brief Counts the times the cached plans were thrown away (a seed, a lock, a reroll, a knob the plans depend
     *        on). A measurement made on a copy of this composer belongs to the generation it was started in.
     * @return the generation
     */
    uint64_t planGeneration() const { return m_planGeneration; }
    /**
     * @brief Takes over what a copy of this composer measured for track @p index behind the music (the host's
     *        background job, 23.09.2026): the level, presence and audibility match and Auto Gain. Nothing else of the
     *        plan is touched -- the planning of a later track writes into an earlier one (the DJ overlap's tail), and
     *        that must survive. Refused when the plans were thrown away since the copy was made.
     * @param[in] index       the track
     * @param[in] measured    the plan the copy measured
     * @param[in] generation  the generation the measurement was started in
     * @return true when taken over (then correctionsPending is set)
     */
    bool adoptMeasurement( int index, const TrackPlan &measured, uint64_t generation ) const;
    /**
     * @brief A flag that stops probe renders early (their readings are then worthless and are not cached). For a
     *        copy measuring in the background, so that a host can end it at once when it is torn down.
     * @param[in] flag  the flag, or null
     */
    void setAbortFlag( const std::atomic<bool> *flag ) { m_abort = flag; }
    /**
     * @brief Plan tracks without their Auto Gain offset; a host measures it afterwards.
     *
     * For the plugin and nothing else: `phos_render` is the determinism oracle and plans whole, and
     * the Quest app has one small core and no one waiting at a button. Off by default, so every
     * program that does not ask is exactly where it was.
     * @param[in] on  true to defer
     */
    void setDeferMasterGain( bool on ) { m_deferMaster = on; }
    /**
     * @brief Measures a plan planned without its probes: the level match, the presence and the audibility match
     *        (TrackPlan::measureDeferred). Auto Gain stays deferred (completeMasterGain).
     *
     * With deferral on (setDeferMasterGain), the first track of a set is planned without any probe render at all
     * (23.09.2026, round "Planung"): the button used to be followed by 8 to 11 seconds of silence while the
     * level, presence and audibility probes ran, and a track's first sixteen bars do not need a single one of
     * them. The plan has every correction at zero -- the first track corrects nothing in the level match, it is the
     * reference; its lines' presence and audibility gains are what arrive later -- and the host calls this on
     * its composer thread once the music runs, then sends the lines' new levels (levelControls, with a ramp).
     * Since 26.09.2026 every track of a live set is planned so, not only the first: planning one took 15 to 30 s of
     * probes on the composer thread, longer than the eight bars the engine's rings hold. Measuring a later track
     * measures the first one before it, because every later track is matched against the first.
     * The plan ends up the same, number for number, as one planned whole (testDeferredPlan).
     * @param[in] p      the knobs
     * @param[in] index  the track
     * @return true when it measured something; false when the plan was not deferred
     */
    bool completeMeasurement( const ParamStore &p, int index ) const;
    /**
     * @brief Whether completeMeasurement() changed corrections the host has not yet sent
     *        (TrackPlan::correctionsPending).
     * @param[in] index  the track
     * @return true when corrections wait to be sent
     */
    bool correctionsPending( int index ) const
    {
        return index >= 0 && index < static_cast<int>( m_plans.size() ) &&
               m_plans[static_cast<size_t>( index )].m_correctionsPending;
    }
    /**
     * @brief The host sent them.
     * @param[in] index  the track
     */
    void clearCorrectionsPending( int index ) const
    {
        if ( index >= 0 && index < static_cast<int>( m_plans.size() ) )
            m_plans[static_cast<size_t>( index )].m_correctionsPending = false;
    }
    /**
     * @brief The melodic parts' level corrections of @p plan as control events at @p beat, ramped over @p rampBeats
     *        -- exactly what trackStartControls writes for them (it calls the same code), for a host that sends
     *        them later (completeMeasurement).
     * @param[in]     p          the knobs
     * @param[in]     plan       the track's plan
     * @param[in]     beat       the beat of the events
     * @param[in]     rampBeats  the ramp in beats
     * @param[in,out] out        the events, added to
     */
    void levelControls( const ParamStore &p, const TrackPlan &plan, double beat, float rampBeats,
                        std::vector<ControlEvent> &out ) const;
    /**
     * @brief The preset @p plan plays on synth @p synth (SoundSynths order), or null: none drawn, or Sound Variation at
     *        0.
     * @param[in] p      the knobs
     * @param[in] plan   the track's plan
     * @param[in] synth  the synth (SoundSynths order)
     * @return the preset, or null
     */
    static const SoundPreset *presetOf( const ParamStore &p, const TrackPlan &plan, int synth );
    /**
     * @brief Whether the master offset is being deferred.
     * @return true while deferred
     */
    bool defersMasterGain() const { return m_deferMaster; }
    /**
     * @brief Measures the master offset of a deferred track and puts it into the cached plan.
     *
     * Runs the two whole-mix probes, so never on the audio thread and never while that track's plan
     * is being read by another thread. Returns the gain in dB; 0 when the track was not deferred (it
     * already has it) or Auto Gain is off.
     * @param[in] p      the knobs
     * @param[in] index  the track
     * @return the gain in dB
     */
    float completeMasterGain( const ParamStore &p, int index ) const;
    /**
     * @brief The set seed.
     * @return the set's seed
     */
    uint64_t seed() const { return m_seed; }

    /**
     * @brief The plan of track @p index (computed in order and cached).
     *
     * The reference stays valid while later tracks are planned (the cache is a deque), and until the next call
     * of a member that takes the knobs, which throws the plans away when a planKnobIds() knob has moved. A plan
     * read across two such calls is copied.
     * @param[in] params  the knobs
     * @param[in] index   the track
     * @return its plan
     */
    const TrackPlan &track( const ParamStore &params, int index ) const;
    /**
     * @brief The parameter ids the plans depend on (25.09.2026): every compose knob, the kick's engine and clip,
     *        the bass's release and each percussion lane's Active, Role, Density and Engine. The preferences'
     *        revision counts as well (Preferences.h); it is not a parameter and is not in the list.
     * @param[in] params  the knobs
     * @return the parameter ids
     */
    static std::vector<int> planKnobIds( const ParamStore &params );
    /**
     * @brief Index of the track that owns @p bar: the one whose kick and bass sound there. Over the DJ
     *        overlap that is the outgoing track, until its last bar.
     * @param[in] params  the knobs
     * @param[in] bar     the bar
     * @return the track's index
     */
    int trackOfBar( const ParamStore &params, int bar ) const;
    /**
     * @brief Index of the track whose intro sounds over the outgoing track's outro in @p bar (the DJ
     *        overlap, Form.h), or -1 where only one track sounds. Plans the next track when asked about
     *        the last DjOverlap bars of one.
     * @param[in] params  the knobs
     * @param[in] bar     the bar
     * @return the incoming track's index, or -1
     */
    int incomingOfBar( const ParamStore &params, int bar ) const;
    /**
     * @brief The tempo map of the first @p bars bars: held per track, ramped over the DJ overlap (the last 16 bars of a
     *        track) into the next.
     * @param[in] params  the knobs
     * @param[in] bars    the bars
     * @return the tempo map
     */
    TempoMap tempoMap( const ParamStore &params, int bars ) const;
    /**
     * @brief Every section mark of the first @p bars bars, for the MIDI export and the arrange view.
     * @param[in] params  the knobs
     * @param[in] bars    the bars
     * @return the section marks
     */
    std::vector<SectionMark> sections( const ParamStore &params, int bars ) const;

    /** @name Locks and rerolls (PLAN 6.8)
     *  A locked unit keeps the seed it had before any reroll; rerolling a unit advances its own
     *  variation counter, which is mixed into its seed and into nothing else.
     *  @{ */
    /**
     * @brief Locks or unlocks a unit.
     * @param[in] unit    the kind of unit
     * @param[in] index   the unit
     * @param[in] locked  true to lock
     */
    void setLock( LockUnitE unit, int index, bool locked );
    /**
     * @brief Whether a unit is locked.
     * @param[in] unit   the kind of unit
     * @param[in] index  the unit
     * @return true when locked
     */
    bool isLocked( LockUnitE unit, int index ) const;
    /**
     * @brief Advances the variation counter of an unlocked unit (a locked one does not move).
     * @param[in] unit   the kind of unit
     * @param[in] index  the unit
     */
    void reroll( LockUnitE unit, int index );
    /**
     * @brief The variation counter of a unit (0 = never rerolled).
     * @param[in] unit   the kind of unit
     * @param[in] index  the unit
     * @return the counter
     */
    uint32_t variation( LockUnitE unit, int index ) const;
    /**
     * @brief Sets a variation counter directly (used when a .phosset is read).
     * @param[in] unit   the kind of unit
     * @param[in] index  the unit
     * @param[in] value  the counter
     */
    void setVariation( LockUnitE unit, int index, uint32_t value );
    /** @brief Forgets every lock and every reroll. */
    void clearLocks();
    /**
     * @brief The locks, for serialisation: unit -> indices.
     * @param[in] unit  the kind of unit
     * @return the locked indices
     */
    const std::map<int, uint8_t> &locks( LockUnitE unit ) const { return m_locked[static_cast<int>( unit )]; }
    /**
     * @brief The variation counters, for serialisation.
     * @param[in] unit  the kind of unit
     * @return the counters by index
     */
    const std::map<int, uint32_t> &variations( LockUnitE unit ) const { return m_variation[static_cast<int>( unit )]; }
    /** @} */

    /**
     * @brief Composes whole bars.
     * @param[in]     params    knob values (compose.* and the kick and bass knobs the recipes start from)
     * @param[in]     firstBar  index of the first bar (bar 0 starts at beat 0)
     * @param[in]     count     number of bars
     * @param[out]    notes     receives the notes, sorted
     * @param[in,out] controls  if not null, receives the sound changes, sorted
     */
    void composeBars( const ParamStore &params, int firstBar, int count, std::vector<NoteEvent> &notes,
                      std::vector<ControlEvent> *controls = nullptr ) const;

    /**
     * @brief One track alone (23.09.2026, round "DJ-Export"): composeBars then sends only what track @p index
     *        plays, as if the set held nothing else; -1 (the default) is the set.
     *
     * A DJ wants each track as its own file, and a track cut out of the set carries the neighbours' blends. In
     * solo mode a bar the track owns is composed without the incoming guest (so the outro ends on its bare kick,
     * bass and hat, the classic DJ outro), a bar in which it is the guest is composed as the guest alone (the
     * intro without the outgoing track under it), with the track's floor settings written at its first bar
     * because no earlier track put them there, and every other bar is silent. Nothing else changes: the plans
     * are the set's, so the file is the track the set plays.
     * @param[in] index  the track, -1 for the set
     */
    void setSoloTrack( int index ) { m_soloTrack = index; }
    /**
     * @brief The track composeBars is limited to, or -1.
     * @return the track, or -1
     */
    int soloTrack() const { return m_soloTrack; }

    /**
     * @brief Normalised parameter offsets of a recipe.
     * @param[in]  kickModule  true for the kick table, false for the bass table
     * @param[in]  macros      NumKickMacros or NumBassMacros values
     * @param[in]  amount      Sound Variation
     * @param[out] out         receives the offset per parameter of the module, indexed like its table (must hold
     *                         ParamStore::moduleCount entries)
     */
    static void recipeOffsets( bool kickModule, const float *macros, float amount, float *out );

    /**
     * @brief Normalised acid offsets of a point in the voicing triangle (NumAcidVoicings).
     *
     * Each voicing is a set of parameter values; the offset of a parameter is the weighted sum of the
     * voicings' normalised distances from that parameter's *default* (the driven voicing), times the
     * reach min(1, 2 x Sound Variation) -- at the default Sound Variation of 0.5 a track at a corner of
     * the triangle plays that voicing exactly, at 0 every track plays the knobs. Offsets, not values:
     * a knob the user has moved keeps its meaning, the track moves around it.
     * @param[in]  p         the parameter store (for the normalised domains and the defaults)
     * @param[in]  weights   NumAcidVoicings barycentric weights
     * @param[in]  amount    Sound Variation
     * @param[out] out       receives acid::Count offsets, indexed like the acid table
     * @param[out] disperse  receives the Disperse override (a discrete parameter), or -1 for the knob
     */
    static void acidVoicingOffsets( const ParamStore &p, const float *weights, float amount, float *out,
                                    int &disperse );

    /**
     * @brief Normalised offsets of a polyphonic voice's recipe (19.09.2026).
     * @param[in]  voice   which instance (the loadings differ: an arp keeps its short attack, a drone its dark filter)
     * @param[in]  r       the recipe
     * @param[in]  amount  Sound Variation
     * @param[out] out     receives poly::Count offsets, indexed like the poly table
     */
    static void voiceRecipeOffsets( PolyInstanceE voice, const VoiceRecipe &r, float amount, float *out );
    /** @brief A voice's palette, read-only (23.09.2026: the factory presets lay it out, SoundPresets.h). */
    struct VoicePaletteView
    {
        const double *m_osc;        ///< weight per PolyOsc
        const int    *m_builtin;    ///< built-in wavetables, six slots, -1 ends
        const int8_t *m_lane;       ///< wavetable lanes, three slots, -1 ends
        const double *m_filter;     ///< weight per PolyFilter
        const double *m_osc2;       ///< weight per PolyOsc2 (index 0 = none)
        const double *m_interval;   ///< weight per PolyOsc2Interval
    };
    /**
     * @brief The palette a voice's recipes are drawn from (ComposerInternal.h, VoicePalettes), for the presets and the
     *        tests.
     * @param[in] voice  the voice
     * @return its palette
     */
    static VoicePaletteView voicePalette( PolyInstanceE voice );

    /**
     * @brief How many real wavetable candidates a voice's palette offers (ComposerInternal.h, VoicePalettes).
     *
     * Added 20.09.2026 (round "wavetable-selection") so the self test can check the widened candidate
     * counts directly, against the exact bound the recipe draw itself uses (a shared helper, not a
     * re-implementation) -- a rendered, 20-track measurement of the tables a track actually ends up
     * using is what proves the wider selection is audible (testVoicesSound), but it is a noisy way to
     * catch a narrower regression (an unrelated upstream RNG draw can shift which of a wide pool gets
     * hit this run); this is the direct one.
     * @param[in] voice  which polyphonic instance
     * @return the count of `tables[]` entries before the first -1 (or the array's end)
     */
    static int voicePaletteTableCount( PolyInstanceE voice );

private:
    /**
     * @brief Drops the cached plans when a parameter that shapes them has changed since they were made.
     * @param[in] params  the knobs
     */
    void validate( const ParamStore &params ) const;
    /**
     * @brief One call of a member that reads the plans: validates the knobs when it is the outermost one
     *        (25.09.2026; see the file comment), and counts the depth so the calls it makes do not.
     */
    struct Entry
    {
        /**
         * @brief Enters: validates @p params when no other call is running on @p c.
         * @param[in] c       the composer
         * @param[in] params  the knobs
         */
        Entry( const Composer &c, const ParamStore &params ) : m_composer( c )
        {
            if ( m_composer.m_depth++ == 0 ) m_composer.validate( params );
        }
        ~Entry() { --m_composer.m_depth; }            ///< leaves
        Entry( const Entry & ) = delete;              ///< one entry, one leave
        Entry &operator=( const Entry & ) = delete;   ///< one entry, one leave
        const Composer &m_composer;                   ///< the composer entered
    };
    /**
     * @brief Plans track @p index from its walk, its seeds and the knobs: key, tempo, form, melody, recipes and levels.
     * @param[in] params  the knobs
     * @param[in] index   the track
     * @return its plan
     */
    TrackPlan makeTrack( const ParamStore &params, int index ) const;
    /**
     * @brief Draws the track's two learned bass phrases, or leaves the plan on the pattern families.
     * @param[in]     params  the knobs
     * @param[in,out] plan    the track's plan
     */
    void makeBassPhrases( const ParamStore &params, TrackPlan &plan ) const;
    /**
     * @brief Draws the track's two bass *rhythm* phrases, or leaves the plan on the pattern families.
     * @param[in]     params  the knobs
     * @param[in,out] plan    the track's plan
     */
    void makeBassRhythm( const ParamStore &params, TrackPlan &plan ) const;
    /**
     * @brief The style, key and tempo walk of the set up to track @p index (made and cached on demand).
     * @param[in] params  the knobs
     * @param[in] index   the track
     * @return the walk
     */
    const TrackWalk &walkAt( const ParamStore &params, int index ) const;
    /**
     * @brief The control events that set a track's sound at its start: recipes, voice levels, track gain and part
     *        levels.
     * @param[in]     params  the knobs
     * @param[in]     plan    the track's plan
     * @param[in]     beat    the track's first beat
     * @param[in,out] out     the events, added to
     * @param[in]     scope   which events
     */
    void trackStartControls( const ParamStore &params, const TrackPlan &plan, double beat,
                             std::vector<ControlEvent> &out, ControlScopeE scope = ControlScopeE::ALL ) const;
    /**
     * @brief The bass's slow filter arcs within a track, at @p beat (ramped, or set at once).
     * @param[in]     params   the knobs
     * @param[in]     plan     the track's plan
     * @param[in]     inTrack  the bar within the track
     * @param[in]     beat     the bar's beat
     * @param[in]     ramp     true to ramp, false to set at once
     * @param[in,out] out      the events, added to
     */
    void arcControls( const ParamStore &params, const TrackPlan &plan, int inTrack, double beat, bool ramp,
                      std::vector<ControlEvent> &out ) const;
    static constexpr int ProbeLines = -3;     ///< probeLoudness: lead, counter, arp and stab in the drops
    static constexpr int ProbeRest = -4;      ///< probeLoudness: everything but the lines in the drops
    static constexpr int ProbeAudible = -5;   ///< probeLoudness: the drops with every part, for the audibility meter
    /**
     * @brief A probe render of the track: its loudness (LUFS), and with @p bands its band powers.
     * @param[in]     params        the knobs
     * @param[in]     plan          the track's plan
     * @param[in]     part          a melodic part alone (0..), the foundation (-1), the whole mix through the master
     *                              (-2), or one of the two halves of the presence probe: the lines alone (ProbeLines)
     *                              or everything else (ProbeRest), in four bars of each drop, before the master's
     *                              dynamics
     * @param[in]     masterGainDb  the master gain in dB
     * @param[in,out] bands         if not null, receives the power in 1.5..6 kHz and in 40..140 Hz (linear, L plus R)
     * @param[out]    audible       receives the audibility readings, or null
     * @return the loudness in LUFS
     */
    double probeLoudness( const ParamStore &params, const TrackPlan &plan, int part = -1, float masterGainDb = 0.0f,
                          double *bands = nullptr, double *audible = nullptr ) const;
    /**
     * @brief The audibility match (23.09.2026, compose.audibility_match): the counter, the arp and the stab lifted
     *        until each is heard with a share of the lead's partial loudness in the drops (Composer.cpp).
     * @param[in]     params  the knobs
     * @param[in,out] plan    the track's plan
     */
    void matchAudibility( const ParamStore &params, TrackPlan &plan ) const;
    /**
     * @brief Auto Gain: two readings of the whole mix and a secant step to master.target_lufs.
     * @param[in]     params        the knobs
     * @param[in,out] plan          the track's plan
     * @param[in]     firstReading  the first reading, when measureTrack has rendered it already; null renders it here
     */
    void matchMaster( const ParamStore &params, TrackPlan &plan, const double *firstReading = nullptr ) const;
    /**
     * @brief Measures the drops' presence and sets presenceDb and presenceGainDb (compose.presence_match).
     * @param[in]     params        the knobs
     * @param[in,out] plan          the track's plan
     * @param[in]     linesReading  the band powers of the two presence probes, when measureTrack has rendered them
     *                              already (both or neither); null renders them here, one after the other
     * @param[in]     restReading   the band powers of the two presence probes, when measureTrack has rendered them
     *                              already (both or neither); null renders them here, one after the other
     */
    void matchPresence( const ParamStore &params, TrackPlan &plan, const double *linesReading = nullptr,
                        const double *restReading = nullptr ) const;
    /**
     * @brief Runs every probe of a plan and the three matches that read them, serially or in dependency
     *        stages (round "speed", 20.09.2026; Probe.h). The plan comes out the same either way.
     * @param[in]     params  the knobs
     * @param[in,out] plan    the track's plan
     */
    void measureTrack( const ParamStore &params, TrackPlan &plan ) const;
    /**
     * @brief measureTrack() without Auto Gain: the level match, the presence and the audibility match.
     * @param[in]     params  the knobs
     * @param[in,out] plan    the track's plan
     * @param[out]    mix0    receives Auto Gain's first reading where it could run beside the presence probes
     * @return whether it did (then *mix0 holds it)
     */
    bool measureLevels( const ParamStore &params, TrackPlan &plan, double *mix0 ) const;
    /**
     * @brief The part-level events of trackStartControls and levelControls, one formula for both.
     * @param[in]     params  the knobs
     * @param[in]     plan    the track's plan
     * @param[in]     beat    the beat of the events
     * @param[in]     length  the ramp in beats
     * @param[in]     floor   true for the floor's parts
     * @param[in]     voices  true for the voices
     * @param[in,out] out     the events, added to
     */
    void pushPartLevels( const ParamStore &params, const TrackPlan &plan, double beat, float length, bool floor,
                         bool voices, std::vector<ControlEvent> &out ) const;
    /**
     * @brief The control events of bar @p bar of @p plan at @p beat into @p out: the section's rides and the voicing (@p
     *        scope: which of them).
     * @param[in]     params  the knobs
     * @param[in]     plan    the track's plan
     * @param[in]     bar     the bar's plan
     * @param[in]     beat    the bar's beat
     * @param[in,out] out     the events, added to
     * @param[in]     scope   which events
     */
    void sectionControls( const ParamStore &params, const TrackPlan &plan, const BarPlan &bar, double beat,
                          std::vector<ControlEvent> &out, ControlScopeE scope = ControlScopeE::ALL ) const;
    /**
     * @brief The lead's cutoff offset at a bar, as sectionControls ramps it (22.09.2026).
     * @param[in] params  the knobs
     * @param[in] plan    the track's plan
     * @param[in] bar     the bar's plan
     * @return the cutoff offset
     */
    float leadCutoffValue( const ParamStore &params, const TrackPlan &plan, const BarPlan &bar ) const;
    /**
     * @brief The lead phrase's filter arc at a bar: a one-bar ramp on the lead's cutoff (22.09.2026, round "Lead").
     * @param[in]     params   the knobs
     * @param[in]     plan     the track's plan
     * @param[in]     bar      the bar's plan
     * @param[in]     inTrack  the bar within the track
     * @param[in]     beat     the bar's beat
     * @param[in,out] out      the events, added to
     */
    void leadArcControls( const ParamStore &params, const TrackPlan &plan, const BarPlan &bar, int inTrack, double beat,
                          std::vector<ControlEvent> &out ) const;
    /**
     * @brief The incoming track's share of an overlap bar (19.09.2026): its intro's percussion, voices and
     *        effects, and the controls of its voices. Nothing of its kick or bass: there is none yet.
     * @param[in]     params    the knobs
     * @param[in]     track     the incoming track
     * @param[in]     bar       the bar
     * @param[in,out] out       the notes, added to
     * @param[in,out] controls  the controls, added to, or null
     */
    void transitionBar( const ParamStore &params, int track, int bar, std::vector<NoteEvent> &out,
                        std::vector<ControlEvent> *controls ) const;
    /**
     * @brief The tonic drone's slow evolution (19.09.2026): every MelodyPlan::droneEvolveBars bars a new
     *        target for its cutoff, table position and detune, ramped over the whole period, around the
     *        drone's recipe. Every event lies on its own bar, so Conductor::pump never holds one back.
     * @param[in]     params   the knobs
     * @param[in]     plan     the track's plan
     * @param[in]     inTrack  the bar within the track
     * @param[in]     beat     the bar's beat
     * @param[in,out] out      the events, added to
     */
    void droneControls( const ParamStore &params, const TrackPlan &plan, int inTrack, double beat,
                        std::vector<ControlEvent> &out ) const;
    /**
     * @brief The set seed after the set unit's rerolls.
     * @return the seed
     */
    uint64_t setSeed() const;
    /**
     * @brief The seed of a track: frozen at the original when the track is locked.
     * @param[in] index  the track
     * @return its seed
     */
    uint64_t trackSeed( int index ) const;
    /**
     * @brief The seed of one section of a track.
     * @param[in] track  the track
     * @param[in] si     the section
     * @return its seed
     */
    uint64_t sectionSeedOf( int track, int si ) const;
    /**
     * @brief The seed of one percussion lane of a track.
     * @param[in] track  the track
     * @param[in] lane   the percussion lane
     * @return its seed
     */
    uint64_t laneSeedOf( int track, int lane ) const;

    uint64_t         m_seed;                      ///< the set seed
    bool             m_deferMaster = false;       ///< setDeferMasterGain: the plugin measures Auto Gain after the start
    mutable uint64_t m_planGeneration = 0;        ///< planGeneration()
    const std::atomic<bool> *m_abort = nullptr;   ///< setAbortFlag()
    mutable std::deque<TrackPlan>
        m_plans;            ///< the tracks planned so far, made on demand (a deque: growing never moves one)
    int m_soloTrack = -1;   ///< setSoloTrack: the one track composeBars sends, -1 = the set
    mutable std::deque<TrackWalk> m_walk;              ///< the style, key and tempo walk, made on demand
    bool                          m_jamHold = false;   ///< setJam: the walk holds its key
    int                           m_jamScale = -1;     ///< setJam: the scale the walk takes, -1 its own
    mutable std::vector<float>    m_planKnobs;         ///< the knobs the plans were made with (validate)
    mutable int m_depth = 0;   ///< Entry: how many calls that read the plans are running (only the outermost validates)
    mutable bool            m_bassModelReported = false;   ///< the missing-weight-file line is printed once
    std::map<int, uint8_t>  m_locked[NumLockUnits];        ///< unit index -> locked
    std::map<int, uint32_t> m_variation[NumLockUnits];     ///< unit index -> reroll counter
};

/**
 * @brief Keeps the engine's event rings filled a few bars ahead of the play position.
 *
 * In the plugin this runs on the composer thread; the offline renderer calls pump() before every
 * block. Bars are pushed whole and in order; what does not fit waits for the next call.
 */
class Conductor
{
public:
    /**
     * @brief Binds an engine and a composer.
     * @param[in,out] engine    the engine
     * @param[in]     composer  the composer
     */
    Conductor( Engine &engine, const Composer &composer );
    /** @brief Starts again from bar 0. */
    void rewind();
    /**
     * @brief Composes and pushes bars until @p horizonBeats beyond the engine's position are covered.
     * @param[in]     params        knob values to compose with
     * @param[in]     horizonBeats  how far ahead to keep the rings filled
     * @param[in,out] record        if not null, every pushed note is appended here as well (for MIDI export)
     */
    void pump( const ParamStore &params, double horizonBeats, std::vector<NoteEvent> *record = nullptr );
    /**
     * @brief Index of the next bar to be composed.
     * @return the bar's index
     */
    int nextBar() const { return m_nextBar; }

private:
    Engine                   &m_engine;           ///< where the events go
    const Composer           &m_composer;         ///< where they come from
    int                       m_nextBar = 0;      ///< the next bar to compose
    std::vector<NoteEvent>    m_notes;            ///< the last composed bar's notes
    std::vector<ControlEvent> m_controls;         ///< and its control events
    size_t                    m_notePos = 0;      ///< how many of the notes are in the engine's rings
    size_t                    m_controlPos = 0;   ///< ... and of the control events
};

}   // namespace phos
