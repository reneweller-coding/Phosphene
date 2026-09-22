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
 * **Since 19.09.2026 (round "arrangement") the user's two-drop form replaces the grammar below**: every
 * track is intro (32: 16 without kick, then kick and bass), groove (32, a percussion layer every eight
 * bars), buildup 1 (16, the last four a pre-drop break), drop 1 (32), breakdown (32), the big buildup (32,
 * a sixteen-bar roll), drop 2 (48, the climax) and outro (32, the last sixteen kick, bass and a hat), with
 * a template per subgenre family (Form.cpp, kTemplates) and the DJ overlap between tracks (kDjOverlap).
 * The rules stand above the measured values the older text below describes; what is left of those is
 * kept where the rules do not speak (the PDB variants, the cut, the energy arc, modal interchange).
 *
 * **The grammar (until 19.09.2026)** was context-free with weights per style profile (PLAN 6.2):
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
 *
 * **Modal interchange over a static bass pedal** (16.09.2026). A track's key is fixed, but its
 * *mode* need not be: in Goa and full-on the bass rolls on the tonic while the melodic layer moves
 * between Dorian or Aeolian in the groove, Phrygian in the main drive and Phrygian dominant or
 * double harmonic -- the raised third, the Hijaz colour -- at the peak. The pedal is what lets the
 * mode move without losing the low end (Easwaran 2004 on the drone; the technique itself is common
 * modal interchange over a pedal point, Persichetti, "Twentieth-Century Harmony", 1961, ch. 2).
 *
 * Each section therefore carries its own @c scale. It is drawn from the section's own seed out of
 * the style profile's @c interchangeWeight -- Goa reaches for Phrygian dominant and double harmonic,
 * Progressive stays on Dorian and Aeolian -- and the energy of the section decides how far it may
 * reach: a mode's weight is multiplied by @c exp(kInterchangeEnergy * (energy - 0.7) * colourTones),
 * so the colourful modes are only really available where the arc is high. @c interchangeChance is
 * how often a section borrows at all; at 0 every section keeps the track's mode and the whole
 * feature is off, which is what the self test compares against.
 *
 * **The bass never moves.** The mode is a property of the melodic layer alone. The bass root, the
 * bass register, its gate limit, the learned bass phrase and the chord shift of
 * compose.bass_follows_chords all read the *track's* mode, never a section's, so the bass part of a
 * render with interchange on is bit-identical to one with it off (self test, "modal interchange").
 */
#pragma once
#include "phos/Harmony.h"
#include "phos/Params.h"
#include "phos/Score.h"
#include <cstdint>
#include <vector>

namespace phos {

/**
 * @brief The melodic parts: the acid and the six polyphonic voices, in the order of PolyInstance.
 *
 * 19.09.2026, round "voices": the counter-lead, the stab and the tonic drone joined, each beside the
 * voice it belongs with. Every array kept per melodic part (MelodyPlan, TrackPlan, StyleProfile) and
 * the bits of BarPlan::parts follow this order, and nothing may address them by a bare number: use
 * mpIndex() and partBit().
 */
enum class MelodyPart : int { Acid = 0, Lead, Counter, Arp, Stab, Pad, Drone, Count };
constexpr int kMelodyParts = static_cast<int>(MelodyPart::Count);   ///< number of melodic parts
/** @brief The index of a melodic part in the arrays kept per part. */
constexpr int mpIndex(MelodyPart p) { return static_cast<int>(p); }
/** @brief The bit of a melodic part in BarPlan::parts and BarPlan::partsNext. */
constexpr uint8_t partBit(MelodyPart p) { return static_cast<uint8_t>(1u << static_cast<int>(p)); }
/** @brief The polyphonic instance that plays a melodic part (not for the acid, which is no Poly). */
constexpr PolyInstance melodyPoly(MelodyPart p) { return static_cast<PolyInstance>(static_cast<int>(p) - 1); }
/** @brief The score part of a melodic part. */
constexpr Part melodyScorePart(MelodyPart p) { return p == MelodyPart::Acid ? Part::Acid : polyPart(melodyPoly(p)); }
static_assert(melodyScorePart(MelodyPart::Drone) == Part::Drone && melodyScorePart(MelodyPart::Counter) == Part::Counter
                  && kMelodyParts == kPolyInstances + 1,
              "the melodic parts are the acid followed by the polyphonic instances in their order");
static_assert(kMelodyParts <= 8, "BarPlan::parts holds one bit per melodic part");

/** @brief The style profiles (compose.style). */
enum class StyleId : int { Goa = 0, FullOn, Progressive, DarkForest, HiTech, Count };
constexpr int kNumStyles = static_cast<int>(StyleId::Count);   ///< number of style profiles
extern const char* const kStyleNames[kNumStyles];              ///< display names

/** @brief The dramaturgy presets of the energy arc (compose.arc). */
enum class ArcId : int { WarmUp = 0, PeakTime, Morning, Closing, Flat, Count };
constexpr int kNumArcs = static_cast<int>(ArcId::Count);   ///< number of arcs
extern const char* const kArcNames[kNumArcs];              ///< display names

/**
 * @brief The form templates of the two-drop architecture (19.09.2026, round "arrangement"), one per
 *        subgenre family: Full-On (also Hi-Tech), Progressive, Goa and Dark Forest.
 *
 * Until this round these were the three grammar bodies of PLAN 6.2; the user's arrangement rules replaced
 * them (Form.cpp, kTemplates). The name stays because FormPlan::body and the style profiles' bodyWeight
 * keep their meaning: which template a track was built from.
 */
constexpr int kNumBodies = 4;
constexpr int kNumPdbVariants = 4;   ///< whole bar, half bar, beat 4 only, kick alone on beat 4 (the last is never drawn since 19.09.2026: beat 4 stays empty)

/**
 * @name The DJ intro and outro inside a continuous set (19.09.2026, round "arrangement")
 *
 * A track's intro runs 32 bars: 16 without a kick (atmosphere, a quiet sixteenth hat, the shaker), then
 * kick and bass. Its outro runs 32 bars and ends on 16 bars of kick, bass and one hat. Inside a set the
 * two ends overlap the way a DJ mixes them: the first kDjOverlap bars of track N+1's intro sound over the
 * last kDjOverlap bars of track N's outro. That puts N's kick-bass-hat bars under N+1's kick-free
 * atmosphere, so **exactly one kick and one bass sound at every moment** -- N's until the hand-over bar,
 * N+1's from it, which is also N+1's kick entry at its own bar 17. Sixteen is the one overlap for which
 * both halves of the user's rule hold literally; with 32 either N's last 16 bars or N+1's kick entry would
 * have to give way (Composer.cpp, transitionBar).
 * @{ */
constexpr int kDjOverlap = 16;       ///< the shortest blend: Hi-Tech's, and the last track's outro tail when no track follows
constexpr int kDjOverlapMax = 32;    ///< the longest blend (23.09.2026, round "DJ")
constexpr int kIntroKickBar = 16;    ///< bar of the intro (0-based) on which kick and bass enter in the set's first track
constexpr int kOutroBareBars = 8;    ///< the outro's last bars: kick, bass and one hat only (16 until 23.09.2026)
/** @} */

/**
 * @brief How many bars the incoming track of @p incoming's style blends over the outgoing one (23.09.2026,
 *        round "DJ").
 *
 * The user: "Da wird ausgeblendet, dann wird es irgendwie leiser, dann ganz ploetzlich wieder lauter" -- the
 * sixteen-bar overlap ended in a jump of track gain and master offset at the hand-over, over a bare outro.
 * Psytrance DJs blend long -- 32 bars and more -- and *swap* the bass at a phrase boundary rather than
 * mixing two basslines (Club Ready DJ School, "How to mix psy trance"; We Are Crossfader, "How to DJ trance
 * music"). So the blend is 32 bars: the incoming track's whole intro over the outgoing outro, both keeping
 * their own floor -- the outgoing kick and bass to the swap, the incoming kick and bass from it (its intro
 * has no kick before the hand-over) -- and the gains ramp over the last eight bars into the swap
 * (Composer.cpp, transitionBar). Hi-Tech, fast and hard, keeps the short sixteen. The blend can never be
 * longer than the incoming intro or the outgoing outro (Composer.cpp, makeTrack).
 */
inline int djOverlapBars(StyleId incoming) { return incoming == StyleId::HiTech ? kDjOverlap : kDjOverlapMax; }
extern const char* const kPdbVariantNames[kNumPdbVariants];   ///< display names
constexpr int kBassSlots = 3;        ///< bass notes per beat at most; the slot envelope of PLAN 6.6
constexpr int kMaxSections = 16;     ///< sections a track can have (the templates need eight)
/**
 * @name The track lengths the templates can build exactly
 * Every template must be able to hit every requested length, because the template is a track's own
 * decision while the length belongs to the set walk: a rerolled track must not move the tracks after it.
 * Lengths are multiples of 16 bars. 128 bars are 3:32 at 145 BPM, 320 bars are 8:50; the default is 256,
 * and the set walk moves a track by at most one 16-bar block (240 .. 272 bars, the user's 220 .. 280).
 * @{ */
constexpr int kMinTrackBars = 128;   ///< shortest track the templates build
constexpr int kMaxTrackBars = 320;   ///< longest
constexpr int kTrackBarStep = 16;    ///< track lengths are multiples of this
/** @} */

/**
 * @brief The macro contours a lead phrase may take (22.09.2026, round "Lead"; Melody.cpp, makeLead).
 *
 * The user's brief names five archetypes. Each is eight *bar offsets* in scale steps that the cell
 * is transposed by, bar for bar -- centred on the cell, so the phrase keeps its register -- and a
 * matching filter arc (kLeadArchetypes in Melody.cpp): contour lives on the two-to-eight-bar level,
 * where Margulis (*On Repeat*, Oxford 2014) puts the hook of riff music, and the cell itself stays
 * what it is -- the repetition is the point.
 */
enum class LeadArchetype : int {
    PhrygianSurge = 0,   ///< flat, then rising to the seventh bar, back under it in the eighth
    ArchAndDrop,         ///< up over the first half, down over the second
    PedalAndBounce,      ///< the cell held home; the operators alone move it
    DescendingCascade,   ///< starts high and steps down over the phrase
    TensionCall,         ///< rises in calls that do not resolve until the last bar
    Count
};
constexpr int kNumLeadArchetypes = static_cast<int>(LeadArchetype::Count);   ///< number of archetypes

/**
 * @brief The operators a lead bar may apply to the cell (22.09.2026; Melody.cpp, applyCellOp).
 *
 * The brief: "Würfle eine 1-Takt-Keimzelle und erzeuge alles Weitere durch deterministische
 * Operatoren". Eight bars are one cell and one operator per bar; the operators are what makes A2
 * recognisably A1's answer rather than another draw.
 */
enum class CellOp : int {
    Keep = 0,        ///< the cell as it is
    EndCadence,      ///< the last beat bent onto the tonic, the note before it a step off it
    TransposeUp,     ///< the cell one scale step up (on top of the archetype's offset)
    TransposeDown,   ///< one scale step down
    InvertEnd,       ///< the last beat's contour mirrored round its first note
    Shift16,         ///< the cell rotated by one sixteenth: every beat note lands just after its beat
    Thin,            ///< two or three weak sixteenths dropped -- the question's open end
    Count
};
constexpr int kNumCellOps = static_cast<int>(CellOp::Count);   ///< number of cell operators

/**
 * @brief How the counter-lead answers the lead (23.09.2026, round "Counter"; Melody.cpp, makeCounter).
 *
 * The user's genre matrix: Full-On call-and-response, Progressive and Darkpsy timbral, Hi-Tech
 * micro-hocketing, Goa polyphonic; and his supplement's "MOTIF_ECHO" as the cheapest authentic counter.
 * Drawn per track from the style's weights (LeadStyle::counterMode), or set by compose.counter_mode.
 */
enum class CounterMode : int {
    Echo = 0,    ///< the lead's own beat notes and accents, delayed a dotted eighth or a beat, a fifth/octave/fourth up
    Answer,      ///< answers in fixed rhythmic templates after each statement, a line of its own over B
    Timbral,     ///< a texture: one long note per two-bar unit, sustained envelope and portamento
    Hocket,      ///< short notes on the off sixteenths the lead leaves open
    Count
};
constexpr int kNumCounterModes = static_cast<int>(CounterMode::Count);   ///< number of counter modes

/**
 * @brief A style's lead vector (22.09.2026, round "Lead").
 *
 * The user's genre matrix -- Full-On call-and-response, Progressive timbral and sparse, Goa
 * polyphonic and dense, Darkpsy timbral with tension tones, Hi-Tech micro-hocketing -- as one set of
 * weights over the same cost function rather than five code branches, so a hybrid is a point between
 * two vectors and the engine stays one testable pipeline. The knobs compose.lead_density and
 * compose.pitch_entropy override `density` and `entropy` when they are not "Auto".
 */
struct LeadStyle {
    float  density = 0.5f;       ///< 0..1: onsets per bar, 8..10 at 0, 10..12 at 0.5, 12..16 at 1 (never under eight: rule 16)
    float  homing = 1.0f;        ///< regression to the register centre (von Hippel and Huron 2000): tightens the register prior
    float  stability = 1.0f;     ///< how hard metric weight pulls a note to the stable degrees (1 and 5, then b3)
    float  proximity = 1.0f;     ///< the critic's weight on the corpus interval histogram: 1 smooth, 0 free leaps
    float  entropy = 1.0f;       ///< multiplier on compose.melody_temperature
    float  slideChance = 0.3f;   ///< chance that a tension-to-resolution step slides (portamento)
    float  accentChance = 0.3f;  ///< chance that an onset after a rest is accented
    float  interlock = 0.5f;     ///< how much the bass pattern's holes attract the lead's onsets
    float  arcDepth = 0.05f;     ///< depth of the phrase's cutoff arc, normalised
    double archetype[kNumLeadArchetypes] = { 0.2, 0.2, 0.2, 0.2, 0.2 };   ///< weight of each LeadArchetype
    double cellOp[kNumCellOps] = { 0.0, 0.2, 0.2, 0.1, 0.15, 0.15, 0.2 };  ///< weight of each CellOp for a variation bar (Keep is never drawn)
    /** @name 23.09.2026, round "Counter": the register and the counter's mode
     *  The literature puts psytrance leads at 250 Hz .. 2 kHz with the weight around 500 Hz .. 1 kHz
     *  (Psychedelic Island, "The science of frequency in psytrance"; Dance Midi Samples, "Making a
     *  psytrance lead"); the lead's one-octave window sat at its lower end (C4 .. B4, 262 .. 494 Hz) and
     *  the user heard it as "teils zu tief". The window moves up by `registerShift` semitones (plus a
     *  semitone of per-track jitter), the counter an octave above it.
     *  @{ */
    int    registerShift = 5;                    ///< semitones the lead window (and with it the counter's) stands above C4
    double counterMode[kNumCounterModes] = { 0.4, 0.5, 0.1, 0.0 };   ///< weight of each CounterMode (echo, answer, timbral, hocket)
    /** @} */
};

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
    float  partAmount[kMelodyParts] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };   ///< multipliers on each melodic part's amount (MelodyPart order)
    float  squelchChance = 1.0f;            ///< multiplier on compose.squelch_chance
    float  hatDensity = 1.0f;               ///< multiplier on compose.perc_density
    double pdbWeight[kNumPdbVariants] = {}; ///< weights of the pre-drop break variants
    float  slotGate[kBassSlots] = { 1.0f, 1.0f, 1.0f };   ///< gate multiplier per bass slot in a beat
    float  slotVel[kBassSlots] = { 1.0f, 1.0f, 1.0f };    ///< velocity multiplier per bass slot
    float  breakShare = 0.22f;              ///< share of the body the breakdowns should take (0.15..0.30)
    float  colour = 1.0f;                   ///< how much weight the flat second and augmented second may get
    float  introBars = 16.0f;               ///< preferred intro length in bars (8 or 16)
    /**
     * @name Modal interchange (16.09.2026)
     * Which modes a section of this style may borrow over the tonic pedal, and how often it does.
     * @{ */
    double interchangeWeight[kNumScales] = {};   ///< weight of each mode as a section's borrowed mode
    float  interchangeChance = 0.0f;             ///< 0..1: how often a section borrows a mode at all
    /** @} */
    StyleId id = StyleId::FullOn;                ///< which style this is (22.09.2026: the harmony tables are indexed by it)
    LeadStyle lead;                              ///< the lead vector (22.09.2026, round "Lead")
};

/** @brief How much a section's energy lifts the colourful modes when a mode is borrowed. */
constexpr double kInterchangeEnergy = 1.6;

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

/**
 * @name Drop 2 as the climax (19.09.2026, round "arrangement")
 * The user: drop 2 is "the most energetic part of the whole track -- its spectral energy must exceed every
 * other point". Drop 1 takes kDrop1Share of a drop's nominal energy, and after the arc has scaled every
 * section, drop 2 is lifted (or, where it would pass 1, the others lowered) until it stands kClimaxMargin
 * above every other section. The energy drives the track gain by 5 dB per unit (Composer.cpp,
 * energyGainDb), so the margin alone is 1 dB on kick, bass and percussion (0.08 = 0.4 dB measured only
 * +0.17 dB at the master's output over drop 1 on the listening seed); the audible rest of the climax is
 * density -- open hats, ride, the second lead, the arp an octave up, squelches in the gaps.
 * @{ */
constexpr float kDrop1Share = 0.88f;
constexpr float kClimaxMargin = 0.20f;
/** @} */

/**
 * @name Drop 2 audible (19.09.2026, round "polish")
 * Measured over 30 tracks (5 styles, 2 seeds, 3 tracks each) with the arrangement round's form: drop 2 stood
 * only +0.62 LU (K-weighted, median; minimum +0.36) over drop 1 at the output, and the loudest other eight-bar
 * window of the track was the end of the big buildup -- as loud as drop 2 and, above 1.5 kHz, 2.2 dB *brighter*
 * (its thirty-second snare roll over the riser). The master's compressor and true-peak limiter take about half
 * of any gain difference, and drop 2 already sits at the ceiling, so drop 2 cannot be pushed up; what is left
 * is to take the rest down and to make drop 2 fuller where loudness is not the currency:
 *  - drop 1 stands kDrop1HoldDb under its energy's gain and holds one more percussion layer back (Form.cpp);
 *  - the buildup into drop 2 ramps down to kBuildHeadroomDb under its energy's gain by its last bar, and its
 *    thirty-second roll plays softer (Rhythm.cpp), so the drop is the arrival and not the build's peak;
 *  - drop 2 opens the lead's and the acid's filters by kClimaxOpen and widens the lead, counter, arp and pad
 *    by kClimaxWidth (Composer.cpp, sectionControls);
 *  - the intro stands kIntroTrimDb under its energy's gain: with those changes alone, the intro's kick-and-bass
 *    bars (17..32) were the loudest eight bars outside drop 2 in most tracks, 0.25 .. 0.65 LU under it -- sparse
 *    material passes the compressor and the limiter that press the drops -- and 1.7 LU over the outgoing
 *    track's bare outro at the DJ hand-over.
 * sectionTrimDb() is the gain side of this, used by the section controls and by the level probe alike, so
 * Auto Gain aims at the track the form really plays.
 * @{ */
constexpr float kDrop1HoldDb = 1.5f;       ///< drop 1's gain under its energy's (dB, before the master)
constexpr float kBuildHeadroomDb = 2.0f;   ///< the big buildup's gain at its last bar under its energy's (dB)
constexpr float kIntroTrimDb = 1.5f;       ///< the intro's gain under its energy's (dB)
constexpr float kClimaxOpen = 0.05f;       ///< drop 2's filter lift, normalised cutoff
constexpr float kClimaxWidth = 0.15f;      ///< drop 2's stereo width lift, normalised
/** @} */

/** @brief One section of a track's form. */
struct Section {
    SectionType type = SectionType::Groove;   ///< what kind of section it is
    int   startBar = 0;      ///< bars from the start of the track (always a multiple of 8)
    int   bars = 16;         ///< length in bars (8, 16, 32 or 64)
    float energy = 0.5f;     ///< 0..1: the type's energy scaled by the set arc at this point
    float energyTo = 0.5f;   ///< energy at the end of the section (buildups rise towards the drop)
    int   pdbVariant = 0;    ///< buildups: which pre-drop break the last bar plays
    float cutBeats = 0.0f;   ///< breakdowns: beats of silence at the start (the cut of Grosz et al.)
    int   scale = 0;         ///< the mode the *melodic* layer takes here; the bass ignores it entirely
    /** @name The two-drop form (19.09.2026, round "arrangement")
     *  @{ */
    bool  climax = false;    ///< drops: this is drop 2, the most energetic part of the track
    int   rollBars = 0;      ///< buildups: bars of the snare roll at its end (quarters -> ... -> thirty-seconds)
    int   pdbBars = 0;       ///< buildups: bars of the pre-drop break at its end (kick and bass out; 0 = none)
    int   breakPerc = 0;     ///< breakdowns: percussion layers that keep playing (Dark Forest's modular percussion)
    bool  spiral = false;    ///< breakdowns: the arp spirals through the whole of it (Goa: no total silence)
    bool  dry = false;       ///< drops: no impact and no sweep into it, the stab carries it (Progressive)
    /** @} */
};

/** @brief An effect placed in a track: start and length in beats from the track's first bar. */
struct SfxEvent {
    double beat = 0.0;     ///< start, beats after the track starts
    float  length = 4.0f;  ///< beats
    int    type = 0;       ///< SfxType
    /**
     * @brief Which variant a voice or a bed event plays (0 = let the engine derive it from the event's beat,
     *        as it did before 19.09.2026), and since 23.09.2026 the bank preset of an effects-strip event
     *        (Sfx.h, SfxPreset; 1-based, families of up to 512). Drawn from the track's form seed, so which
     *        phrase speaks or which preset sounds is a decision of the seed and not of where the event
     *        happens to fall (Engine.cpp).
     */
    uint16_t variant = 0;
};

/** @brief The form of one track. */
struct FormPlan {
    Section section[kMaxSections];    ///< the sections, in order
    int     count = 0;                ///< how many
    int     bars = 0;                 ///< total length (a multiple of 32)
    int     body = 0;                 ///< which template the form was built from (Form.cpp, kTemplates)
    std::vector<SfxEvent> sfx;        ///< effects at the section boundaries, sorted by start
    /** @brief Bit per mode: which modes the melodic layer needs material for (bit @c trackScale always set). */
    uint32_t scaleMask = 0;
    /**
     * @brief Bars at the start of the track over which the previous track's kick and bass still sound
     *        (0 for the first track of a set, kDjOverlap for every later one; set by the composer).
     *
     * The floor is not silent there, so neither the pad's sub foundation nor the drone's low octave may
     * take it (planBar reads it into BarPlan::floorSilent).
     */
    int handover = 0;
    /**
     * @brief Bars at the end of the track over which the *next* track's intro sounds (23.09.2026, round
     *        "DJ"): the next track's handover, written into this plan when that track is made (Composer.cpp);
     *        kDjOverlap until then and for the set's last track. planBar builds the outro's blend from it.
     */
    int overlapTail = kDjOverlap;
};

/**
 * @brief Draws a track's form.
 * @param s          the style profile (body weights, PDB weights, break share, interchange weights)
 * @param seed       the track's form seed
 * @param target     preferred length in bars; the result is the nearest length the grammar can build
 * @param arcIn      the set's energy arc where the track starts
 * @param arcOut     the set's energy arc where the track ends
 * @param trackScale the track's own mode: what a section keeps when it does not borrow
 * @param sectionSeed one seed per section (kMaxSections entries), or null to derive the borrowed
 *                   modes from @p seed; a section is a lockable unit, so its mode hangs off its own
 *                   seed exactly as its instrumentation does
 */
FormPlan makeFormPlan(const StyleProfile& s, uint64_t seed, int target, double arcIn, double arcOut,
                      int trackScale = 0, const uint64_t* sectionSeed = nullptr);

/** @brief Index of the section that contains @p barInTrack (the last one if the bar is past the end). */
int sectionOfBar(const FormPlan& f, int barInTrack);

/**
 * @brief The climax trim of section @p index at the fraction @p u (0 = its first bar, 1 = its end), in dB on
 *        top of the energy's gain: -kDrop1HoldDb over every drop that is not the climax, a ramp from 0 to
 *        -kBuildHeadroomDb over the buildup that leads into the climax, -kIntroTrimDb over the intro, 0 elsewhere
 *        (see kDrop1HoldDb).
 */
float sectionTrimDb(const FormPlan& f, int index, double u);

/**
 * @brief Whether every hard constraint holds (19.09.2026): every length and every start a multiple of
 *        eight, no core and no breakdown under 16 bars, buildups of 8 to 32 bars, an intro and an outro
 *        of at least kDjOverlap bars (the DJ overlap needs them), exactly one climax and it is the last
 *        drop. The breakdown share of PLAN 6.2 (15 .. 30 %) is gone: the user's form gives the breakdown
 *        32 of 256 bars, 12.5 %.
 */
bool formConstraintsHold(const FormPlan& f);

/** @brief What a track can play at all, and where its parts sit, for the instrumentation matrix. */
struct PartAvailability {
    bool part[kMelodyParts] = {};   ///< which melodic parts exist in this track (MelodyPart order)
    float amount[kMelodyParts] = {};  ///< how much each part should play, 0..1 (MelodyPlan::amount)
    int  leadLo = 59, leadHi = 79;   ///< the lead's pitch range (the masking rule)
    int  arpLo = 55, arpHi = 74;     ///< the arp's pitch range
    int  percLayers = 4;     ///< percussion layers the track's kit offers
    int  hatLayers = 1;      ///< how many of them are the hats at the head of the layer order (PercPlan::hatLayers)
};

/** @brief What plays in one bar: the instrumentation matrix evaluated. */
struct BarPlan {
    int         index = 0;              ///< index of the section in the form
    SectionType type = SectionType::Groove;   ///< its type
    int         barInSection = 0;       ///< bar within the section
    int         sectionBars = 16;       ///< length of the section (22.09.2026: a pad chord never rings past its section)
    float       energy = 0.5f;          ///< the section's energy at this bar (buildups rise)
    uint8_t     kickBeats = 0xF;        ///< bit per beat: does the kick play
    uint8_t     bassBeats = 0xF;        ///< bit per beat: does the bass play
    int         percLayers = 0;         ///< how many percussion layers play
    uint8_t     parts = 0;              ///< bit per melodic part (partBit)
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
    int8_t      scale = -1;             ///< the section's mode for the melodic layer, -1 = the track's
    /** @brief Kick and bass rest for the whole bar (and it is no pre-drop break): the floor under 140 Hz
     *         belongs to the pad's sub foundation or the drone's low octave (19.09.2026). */
    bool        floorSilent = false;
    /** @name The two-drop form's percussion and markers (19.09.2026, round "arrangement")
     *  @{ */
    bool        climax = false;         ///< drop 2
    bool        mainBreak = false;      ///< the breakdown before drop 2: the pad's own harmony (22.09.2026, Melody.h)
    int         rollBars = 4;           ///< length of the snare roll rollBar counts in
    bool        quietHats = false;      ///< the intro's first half: a quiet closed hat on every sixteenth but the downbeat
    bool        shaker = false;         ///< the shaker plays whether or not it is one of the layers
    bool        offbeatHat = false;     ///< the closed hat plays its eighth offbeat whether or not it is a layer
    float       hatLevel = 1.0f;        ///< velocity factor of the hats that offbeatHat and quietHats add
    bool        openHats = false;       ///< drop 2: the open hat on every offbeat
    bool        ride = false;           ///< drop 2: the ride plays whether or not it is one of the layers
    bool        crash = false;          ///< a crash on the one (a drop's downbeat, every sixteenth bar of a drop)
    int         cycleBar = 0;           ///< bar within the section's 32-bar cycle (the micro rules: 8, 16, 24, 32)
    /** @} */
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
 * @brief Places the effects: a marker at every section transition and ear candy inside the long
 *        sections (PLAN 5.8, Solberg and Dibben 2019; 18.09.2026 round "mix-foundation").
 *
 * Risers climb over the last eight bars of a buildup and end exactly on the drop; the formant shot is
 * the pre-drop "Abriss" on the last beat before it; an impact marks every drop, and a sweep falls into
 * it (Solberg and Dibben found the descending sweep to be the drop marker). A reverse swell leads into
 * every buildup and breakdown, a downlifter falls into breakdowns and the outro, and the last eight
 * bars of the track carry the sweep that masks the key change into the next track. Inside every
 * section but the buildup, a zap, a short sweep, a reverse swell or a noise wash closes eight- or
 * sixteen-bar groups; the palette and the period are the track's own. Form.cpp has the details.
 * @param amount compose.sfx_amount: transition markers are certain from 0.5 up and thin out below it;
 *               each group boundary carries ear candy with this probability
 * @param voiceDensity compose.voice_density (19.09.2026): scales the probability of every spoken phrase,
 *               chant and chatter the psychedelic layer places; 1 is the calibrated density, 0 none
 * @param bedDensity compose.bed_density: the same for the shamanic bed (bowls, didgeridoo, jaw harp)
 */
void makeFormSfx(FormPlan& f, uint64_t seed, float amount, float voiceDensity = 1.0f, float bedDensity = 1.0f);

/**
 * @name The macro automation of a section (16.09.2026)
 *
 * Phase 5 gives every section *one* filter target and ramps to it over the whole section. For a
 * 32-bar core that is a straight line, and a 303 that holds one cutoff for 32 bars tires the ear
 * however well its accents are placed -- a real acid line lives from a hand on the knob. These
 * constants and @c sectionAutomation() are that hand, written as control events so that the movement
 * is a continuous ramp in the score, deterministic from the section's seed, and visible to the MIDI
 * export and to every host that reads automation.
 *
 * It is **added on top of** the section's own arc, never instead of it: the caller passes the base
 * value at the start and at the end of the section and the ride is measured from the straight line
 * between them, so the energy arc of Phase 5 still decides where a section sits and the ride only
 * decides how it moves inside that.
 * @{ */
constexpr float kRideCutoff = 0.60f;    ///< peak-to-peak cutoff excursion of the acid ride, normalised (4.0 octaves)
constexpr float kRideReso   = 0.07f;    ///< how far a breakdown's dive lowers the resonance
constexpr float kRideResoMedium  = 0.55f;   ///< the ride's resonance in stages 1 and 2 ("medium"; at most the knob)
constexpr float kRideResoSquelch = 0.85f;   ///< the ride's resonance from stage 3 on (the rule's 80 .. 90 %; at least the knob)
constexpr float kRideDecayShort  = 1.0f / 3.0f;   ///< stage 1 filter decay as a ratio of the knob (dry, percussive)
constexpr float kRideDecayLong   = 1.7f;    ///< stages 3 and 4 filter decay as a ratio of the knob
constexpr float kRideDive   = 0.22f;    ///< how far a breakdown dives, normalised
constexpr float kRollSend   = 0.30f;    ///< hall send the buildup's snare roll rises to
constexpr int   kRollBars   = 4;        ///< bars of a buildup the roll and its send ramp run over (where the section names none)
constexpr float kRideSweep  = 0.8f;     ///< the bar-24 sweep of the acid (19.09.2026), in half-excursions above the ride

/**
 * @brief The four-stage acid ride of the user's rule text of 18.09.2026, as key points in time.
 *
 * Over a cycle of 32 bars (or a whole shorter section, so a buildup's cycle ends on the drop):
 *  1. bars 1-8: cutoff almost closed, resonance medium, short filter decay -- a dry, percussive click;
 *  2. bars 9-16: the decay returns to the knob and the cutoff starts to open;
 *  3. bars 17-24: the resonance climbs to 80-90 % -- the squelch;
 *  4. bars 25-32: the cutoff opens fully (by bar 28) and holds, then dives back to closed over the
 *     last bar -- the "radical filter dive just before the drop".
 *
 * The cutoff points are in units of half the excursion and already **centred**: the raw shape
 * (-1, -1, -0.2, 0.5, 1, 1, -1) spends more time closed than open, and its mean over the cycle
 * (meanRaw, -0.175 for 32 bars) is subtracted, so the ride swings around the section's own line. That
 * is what keeps Composer's level match honest, which measures the knobs and not the automation.
 */
struct RideShape {
    static constexpr int kPoints = 7;
    double length = 32.0;       ///< bars of one cycle
    double stage[4] = {};       ///< first bar of each stage within the cycle
    double bar[kPoints] = {};   ///< when each cutoff key point is reached, bars into the cycle
    double hold[kPoints] = {};  ///< bars after reaching point k before the ramp to point k+1 starts
    float  cutoff[kPoints] = {};///< cutoff at each point, in half-excursions, centred on the line
    float  meanRaw = 0.0f;      ///< the mean of the uncentred shape that was subtracted
};

/** @brief The ride's shape for a section of @p sectionBars bars (32-bar cycles, or one shorter one). */
RideShape acidRideShape(double sectionBars);
/** @} */

/**
 * @brief Writes the macro automation of one section: the acid's ride and the buildup's send.
 *
 * Called once at the first bar of a section, after the section's own controls. Everything it writes
 * is a @c ControlEvent::Kind::Offset with a ramp length, so nothing steps.
 *
 * @param p        the knobs (only to look parameter ids up)
 * @param s        the section
 * @param seed     the section's own seed: the ride is a lockable section's property
 * @param beat     the beat the section starts on
 * @param base0    the acid cutoff offset the section starts at (normalised), as the caller computed it
 * @param base1    the same at the end of the section
 * @param knobs    true for the first section of the first track, which plays the knobs exactly
 * @param out      receives the events
 * @param resoBase  the track's acid resonance offset (its voicing; normalised, 19.09.2026): the ride's
 *                  medium and squelch stages are measured from the voiced resonance, not from the knob
 * @param decayBase the same for the filter decay
 */
void sectionAutomation(const ParamStore& p, const Section& s, uint64_t seed, double beat,
                       float base0, float base1, bool knobs, std::vector<ControlEvent>& out,
                       float resoBase = 0.0f, float decayBase = 0.0f);

/** @brief Weights of the bass figures that a group may end on; they differ in their last note. */
extern const int kGroupFigures[4];

} // namespace phos
