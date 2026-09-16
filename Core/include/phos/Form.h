/**
 * @file Form.h
 * @brief The form of a track: sections, the energy arc over the set, the instrumentation matrix, and
 *        the style profiles that weight all of it.
 *
 * **Sections.** Grosz, Solberg, Katz, Vu, Jensenius, Patel-Grosz ("An outline of the narrative grammar
 * of electronic dance music", Musicae Scientiae 2025) describe an EDM track as a sequence of
 * categories with a necessity order -- Core is obligatory, Buildup and Cut/Outro nearly so, then
 * Breakdown/Intro and the Pre-Drop Break -- and a two-peak standard form. Phosphene uses their
 * categories directly: Intro, Groove (the first core), Breakdown, Buildup, PDB, Core (a drop),
 * Cut and Outro. Butler ("Unlocking the Groove", 2006) supplies the hypermetre: everything in powers
 * of two, every section starting on a multiple of eight bars.
 *
 * **The grammar** is context-free with weights per style profile (PLAN 6.2):
 * @code
 *   Track -> Intro Body Outro
 *   Body  -> Groove Build Drop Break Build Drop                (Full-On standard)
 *          | Groove Drop Break Drop Break Drop                 (Progressive, flatter)
 *          | Intro2 Build Drop Break Build Drop Drop2          (Goa, long second drop)
 * @endcode
 * with hard constraints as in the plan: section lengths in {8, 16, 32, 64}, no core shorter than 16,
 * a buildup of 8 or 16, the breakdowns 15 to 30 % of the body, intro and outro 8 or 16 bars (Easwaran,
 * "Psytrance and the Spirituality of Electronics", 2004: about 30 s of atmosphere, which at 145 BPM is
 * 16 bars). A track's total length is made a multiple of 32 bars by changing exactly one section to
 * another of its allowed lengths, so that the bass swap between two tracks always falls on a 32-bar
 * boundary (PLAN 6.7).
 *
 * **PDB** (pre-drop break) is the last bar of a buildup: kick and bass drop out, the snare roll ends on
 * beat 3, and beat 4 is empty but for a single formant shot (Grosz et al. measure 1.5 to 2.5 s, which
 * is one bar at psytrance tempo). Four variants, weighted per style: the whole bar empty, the second
 * half empty, only beat 4 empty, or the kick alone on beat 4.
 *
 * **Cut** is the 1 to 3 s at the end of a core before a breakdown in which everything but a reverb tail
 * stops (Grosz et al.); here the first one or two beats of the breakdown.
 *
 * **The energy arc.** E(t) in [0, 1] over the whole set from a dramaturgy preset (Warm-up, Peak-Time,
 * Morning, Closing, Flat), built from raised-cosine segments so that it has no corners. A section's
 * energy is its type's nominal energy (drop 1.0, buildup rising, breakdown 0.3, intro 0.4) scaled by
 * the arc, and the energy drives the four quantities of Farbood's tension model ("A parametric,
 * temporal model of musical tension", Music Perception 2012): loudness (a small offset on the track
 * gain, at most +-2 dB), density (percussion layers and melodic parts present), register (octave of
 * arp and lead) and dissonance (how much weight the flat second and the augmented second get in the
 * lead's constraint sets).
 *
 * **Change every four or eight bars** (Easwaran; Butler's hypermetre): inside every section each
 * eight-bar group gets its own change -- a layer, a figure, a fill or a register -- and no group
 * repeats its predecessor's. The bass figure at the end of a group is drawn from a set whose members
 * differ in their last note for every bass pattern, so two consecutive eight-bar groups of a core can
 * never hold the same notes.
 *
 * **Style profiles** (PLAN 2.6) are weight vectors: tempo centre and range, scale weights for the key
 * journey, grammar body weights, extra chord moves (Goa adds i-bII and i-bVII on top of the corpus
 * successions), part amounts, squelch chance, hat density, PDB variant weights and the bass slot
 * envelope of PLAN 6.6 (flat by default: nine reference tracks play three equally loud notes).
 * The tempo centres of Goa and Full-On are measured, not assumed: Tools/ref_style.py reads the artist
 * tag of the 40 reference recordings and estimates the tempo from the autocorrelation of the kick
 * band's onset envelope (Goa median 142.8 BPM over 13 tracks, Full-On 144.6 over 18).
 */
#pragma once
#include "phos/Harmony.h"
#include "phos/Params.h"
#include "phos/Score.h"
#include <cstdint>
#include <vector>

namespace phos {

/** @brief The style profiles (compose.style). */
enum class StyleId : int { Goa = 0, FullOn, Progressive, DarkForest, HiTech, Count };
constexpr int kNumStyles = static_cast<int>(StyleId::Count);   ///< number of style profiles
extern const char* const kStyleNames[kNumStyles];              ///< display names

/** @brief The dramaturgy presets of the energy arc (compose.arc). */
enum class ArcId : int { WarmUp = 0, PeakTime, Morning, Closing, Flat, Count };
constexpr int kNumArcs = static_cast<int>(ArcId::Count);   ///< number of arcs
extern const char* const kArcNames[kNumArcs];              ///< display names

constexpr int kNumBodies = 3;        ///< grammar bodies of PLAN 6.2
constexpr int kNumPdbVariants = 4;   ///< whole bar, half bar, beat 4 only, kick alone on beat 4
extern const char* const kPdbVariantNames[kNumPdbVariants];   ///< display names
constexpr int kBassSlots = 3;        ///< bass notes per beat at most; the slot envelope of PLAN 6.6
constexpr int kMaxSections = 16;     ///< sections a track can have (the grammar needs at most 10)
/**
 * @name The track lengths the grammar can build exactly
 * Every body must be able to hit every requested length, because the body is a track's own decision
 * while the length belongs to the set walk: a rerolled track must not move the tracks after it. The
 * window is what all three bodies have in common. 128 bars are 3:32 at 145 BPM, 320 bars are 8:50 --
 * the reference recordings run 7.7 min (Full-On median) to 8.6 min (Goa).
 * @{ */
constexpr int kMinTrackBars = 128;   ///< shortest track the grammar builds
constexpr int kMaxTrackBars = 320;   ///< longest
/** @} */

/**
 * @brief A style profile: the weights that make Goa a Goa and Full-On a Full-On.
 *
 * Everything is either an absolute musical quantity (tempo) or a multiplier on a knob, so that the
 * default profile (Full-On, every multiplier 1) plays the knobs exactly as they are set.
 */
struct StyleProfile {
    const char* name = "";                  ///< the profile's name
    double bpmCentre = 145.0;               ///< tempo the tracks settle around
    double bpmRange = 4.0;                  ///< how far a track may wander from the centre
    double scaleWeight[kNumScales] = {};    ///< weight of each mode in the key journey
    double bodyWeight[kNumBodies] = {};     ///< weight of each grammar body
    double chordExtra[12] = {};             ///< extra weight on a chord-root move of n semitones
    float  partAmount[4] = { 1.0f, 1.0f, 1.0f, 1.0f };   ///< multipliers on acid, lead, arp and pad amount
    float  squelchChance = 1.0f;            ///< multiplier on compose.squelch_chance
    float  hatDensity = 1.0f;               ///< multiplier on compose.perc_density
    double pdbWeight[kNumPdbVariants] = {}; ///< weights of the pre-drop break variants
    float  slotGate[kBassSlots] = { 1.0f, 1.0f, 1.0f };   ///< gate multiplier per bass slot in a beat
    float  slotVel[kBassSlots] = { 1.0f, 1.0f, 1.0f };    ///< velocity multiplier per bass slot
    float  breakShare = 0.22f;              ///< share of the body the breakdowns should take (0.15..0.30)
    float  colour = 1.0f;                   ///< how much weight the flat second and augmented second may get
    float  introBars = 16.0f;               ///< preferred intro length in bars (8 or 16)
};

/** @brief The profile of a style (a static table). */
const StyleProfile& styleProfile(StyleId id);
/** @brief The style the knobs select. */
StyleId styleOf(const ParamStore& p);
/** @brief The arc the knobs select. */
ArcId arcOf(const ParamStore& p);

/**
 * @brief The energy arc E(t) of a dramaturgy preset, @p t in [0, 1] over the whole set.
 *
 * Raised-cosine segments between the preset's control points: a curve without corners, so that
 * neighbouring tracks never jump in energy.
 */
double arcEnergy(ArcId arc, double t);

/** @brief Nominal energy of a section type, before the arc scales it. */
float typeEnergy(SectionType type);

/** @brief One section of a track's form. */
struct Section {
    SectionType type = SectionType::Groove;   ///< what kind of section it is
    int   startBar = 0;      ///< bars from the start of the track (always a multiple of 8)
    int   bars = 16;         ///< length in bars (8, 16, 32 or 64)
    float energy = 0.5f;     ///< 0..1: the type's energy scaled by the set arc at this point
    float energyTo = 0.5f;   ///< energy at the end of the section (buildups rise towards the drop)
    int   pdbVariant = 0;    ///< buildups: which pre-drop break the last bar plays
    float cutBeats = 0.0f;   ///< breakdowns: beats of silence at the start (the cut of Grosz et al.)
};

/** @brief An effect placed in a track: start and length in beats from the track's first bar. */
struct SfxEvent {
    double beat = 0.0;     ///< start, beats after the track starts
    float  length = 4.0f;  ///< beats
    int    type = 0;       ///< SfxType
};

/** @brief The form of one track. */
struct FormPlan {
    Section section[kMaxSections];    ///< the sections, in order
    int     count = 0;                ///< how many
    int     bars = 0;                 ///< total length (a multiple of 32)
    int     body = 0;                 ///< which grammar body was drawn
    std::vector<SfxEvent> sfx;        ///< effects at the section boundaries, sorted by start
};

/**
 * @brief Draws a track's form.
 * @param s        the style profile (body weights, PDB weights, break share)
 * @param seed     the track's form seed
 * @param target   preferred length in bars; the result is the nearest length the grammar can build
 * @param arcIn    the set's energy arc where the track starts
 * @param arcOut   the set's energy arc where the track ends
 */
FormPlan makeFormPlan(const StyleProfile& s, uint64_t seed, int target, double arcIn, double arcOut);

/** @brief Index of the section that contains @p barInTrack (the last one if the bar is past the end). */
int sectionOfBar(const FormPlan& f, int barInTrack);

/**
 * @brief Whether every hard constraint of PLAN 6.2 holds: lengths in {8, 16, 32, 64}, no core and no
 *        breakdown under 16 bars, buildups of 8 or 16, every section start on a multiple of eight, and
 *        the breakdowns between 15 and 30 % of the track.
 */
bool formConstraintsHold(const FormPlan& f);

/** @brief What a track can play at all, and where its parts sit, for the instrumentation matrix. */
struct PartAvailability {
    bool part[4] = {};       ///< acid, lead, arp, pad exist in this track
    int  leadLo = 59, leadHi = 79;   ///< the lead's pitch range (the masking rule)
    int  arpLo = 55, arpHi = 74;     ///< the arp's pitch range
    int  percLayers = 4;     ///< percussion layers the track's kit offers
};

/** @brief What plays in one bar: the instrumentation matrix evaluated. */
struct BarPlan {
    int         index = 0;              ///< index of the section in the form
    SectionType type = SectionType::Groove;   ///< its type
    int         barInSection = 0;       ///< bar within the section
    float       energy = 0.5f;          ///< the section's energy at this bar (buildups rise)
    uint8_t     kickBeats = 0xF;        ///< bit per beat: does the kick play
    uint8_t     bassBeats = 0xF;        ///< bit per beat: does the bass play
    int         percLayers = 0;         ///< how many percussion layers play
    uint8_t     parts = 0;              ///< bit per melodic part (1 acid, 2 lead, 4 arp, 8 pad)
    int8_t      arpOctave = 0;          ///< octave shift of the arp (masking rule and register)
    int8_t      leadOctave = 0;         ///< octave shift of the lead (register)
    bool        padGate = false;        ///< the pad plays through the trance gate
    bool        fills = true;           ///< percussion fills allowed in this bar
    bool        hatsDense = false;      ///< buildup: the closed hat plays every sixteenth
    int         rollBar = -1;           ///< 0..3 = which of the last four bars of a buildup this is
    bool        pdb = false;            ///< this bar is the pre-drop break
    int         pdbVariant = 0;         ///< which variant
    float       cutBeats = 0.0f;        ///< beats of silence at the start of the bar
    int         groupFigure = -1;       ///< bass figure at beat 1 of the group's last bar, -1 = none
    int         group = 0;              ///< eight-bar group within the track
    uint8_t     partsNext = 0;          ///< the parts of the following bar (an acid slide needs a note to slide into)
};

/**
 * @brief The instrumentation matrix: what plays in bar @p barInTrack.
 * @param f           the track's form
 * @param a           what the track has to offer
 * @param sectionSeed one seed per section (kMaxSections entries); a section is a lockable unit, so its
 *                    decisions hang off its own seed rather than off the track's
 * @param barInTrack  bar within the track
 */
BarPlan planBar(const FormPlan& f, const PartAvailability& a, const uint64_t* sectionSeed, int barInTrack);

/**
 * @brief Places the effects at the section boundaries (PLAN 5.8, Solberg and Dibben 2019).
 *
 * Risers climb over the last eight bars of a buildup and end exactly on the drop; the formant shot is
 * the pre-drop "Abriss" on the last beat before it; an impact marks the drop, and a sweep falls into
 * it (Solberg and Dibben found the descending sweep to be the drop marker). Where a core gives way to
 * a breakdown a downlifter runs, and the last eight bars of the track carry the sweep that masks the
 * key change into the next track.
 */
void makeFormSfx(FormPlan& f, uint64_t seed, float amount);

/** @brief Weights of the bass figures that a group may end on; they differ in their last note. */
extern const int kGroupFigures[4];

} // namespace phos
