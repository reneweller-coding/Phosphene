/**
 * @file Composer.cpp
 * @brief Track plans and their walk, bass phrases, curation, the bars of a set, and the conductor
 *        (the control events are in ComposerControls.cpp, the measurements in ComposerLevels.cpp).
 */
#include "phos/SoundPresets.h"
#include "phos/Composer.h"
#include "phos/Audibility.h"
#include "phos/Preferences.h"
#include "phos/Probe.h"
#include "phos/Corpus.h"
#include "phos/Disperser.h"
#include "phos/Dsp.h"
#include "phos/Engine.h"
#include "phos/Harmony.h"
#include "phos/Loudness.h"
#include "phos/Model.h"
#include "phos/Params.h"
#include "phos/Patterns.h"
#include "phos/Sfx.h"
#include "phos/Util.h"
#include "phos/WaveTableFile.h"
#include "ComposerInternal.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

#include "Salts.h"
using namespace phos::salts::composer;   // this file's seed salts (Salts.h)

namespace phos {

const char* const KickMacroNames[NumKickMacros] = { "length", "punch", "body", "grit", "click" };
const char* const BassMacroNames[NumBassMacros] = { "brightness", "pluck", "squelch", "grit", "weight" };
const char* const LockUnitNames[NumLockUnits] = { "set", "track", "section", "lane" };
const char* const AcidVoicingNames[NumAcidVoicings] = { "clean", "driven", "liquid" };
const char* const VoiceMacroNames[NumVoiceMacros] = { "brightness", "softness", "thickness", "space", "motion" };

namespace {

/**
 * @brief How far the set's energy arc moves a track's tempo centre, in BPM per unit of arc energy
 *        (23.09.2026, round "Set-Kurve").
 *
 * The arc already shapes every section's energy and the style journey; the tempo followed only its
 * style's centre. DJ sets rise in tempo towards their peak and fall towards the end -- a few BPM over a
 * night, never a jump -- so the centre the mean-reverting walk homes on now moves with the arc, relative
 * to where the arc stood when the set began (the first track keeps the knobs). Over the arcs of Form.cpp:
 * Warm-up 0.25 -> 0.85 is +3.6 BPM by the end, Peak-Time 0.70 -> 1.00 -> 0.85 is +1.8 then back to +0.9,
 * Closing 0.90 -> 0.20 is -4.2; Flat, the default, moves nothing, so a set without an arc keeps its
 * tempo.
 */
constexpr double ArcTempoPerUnit = 6.0;

/**
 * @brief The delay times a voice's role may draw (20.09.2026, round "dialogue").
 *
 * Indices into DelayBeats (Params.h): 0 = 1/16 (0.25 beats), 1 = 1/8, 2 = a dotted eighth (0.75, "3/16"),
 * 3 = a quarter, 4 = a dotted quarter, 5 = a half. The user's rule gives each of the two leads an echo of
 * its own -- the lead a dotted eighth, the counter a fast sixteenth ping-pong -- and a per-track recipe
 * may vary only inside that family, so the two never swap characters. A ping-pong is two *different*
 * times on the two sides (TempoDelay); the counter's pair is therefore 1/16 against 1/8 or 1/16 against
 * a dotted eighth, both of which put its repeats between the lead's. Pad and drone send nothing into
 * their delay (delay_send 0), so their family exists only to keep the table complete.
 */
struct VoiceDelayFamily {
    int m_left[3];      ///< the left side's times (DelayBeats indices)
    int m_leftCount;    ///< how many of them
    int m_right[3];     ///< the right side's times
    int m_rightCount;   ///< how many of them
};
/** @brief Each polyphonic voice's echo family, in PolyInstance order. */
const VoiceDelayFamily VoiceDelay[PolyInstances] = {
    /* lead    */ { { 2, 2, 2 }, 1, { 2, 4, 4 }, 2 },   // dotted eighth against itself or a dotted quarter
    /* counter */ { { 0, 0, 0 }, 1, { 1, 2, 2 }, 2 },   // 1/16 against 1/8 or a dotted eighth: the ping-pong
    /* arp     */ { { 2, 1, 1 }, 2, { 1, 3, 3 }, 2 },   // as before: the arp's own short echo
    /* stab    */ { { 2, 3, 3 }, 2, { 3, 1, 1 }, 2 },
    /* pad     */ { { 3, 4, 4 }, 2, { 4, 5, 5 }, 2 },
    /* drone   */ { { 4, 5, 5 }, 2, { 5, 4, 4 }, 2 },
};
static_assert(sizeof(VoiceDelay) / sizeof(VoiceDelay[0]) == PolyInstances, "one delay family per polyphonic instance");

/** @brief Draws a recipe: each direction from a normal distribution with sigma 0.5, truncated to [-1, 1]. */
void drawRecipe(Rng& r, float* m, int n)
{
    for (int i = 0; i < n; ++i) {
        double x;
        do { x = 0.5 * gaussian(r); } while (x < -1.0 || x > 1.0);
        m[i] = static_cast<float>(x);
    }
}

/** @brief Index from cumulative weights. */
int pick(Rng& r, const double* weights, int n)
{
    double total = 0.0;
    for (int i = 0; i < n; ++i) total += weights[i];
    double x = static_cast<double>(r.uniform()) * total;
    for (int i = 0; i < n; ++i) { if (x < weights[i]) return i; x -= weights[i]; }
    return n - 1;
}

/**
 * @brief Longest gate that still lets the lowest note's release finish before the next slot.
 * @param slotBeats the shortest gap from a note to whatever ends it, in beats
 */
float gateLimitSlot(double slotBeats, double bpm, int lowestNote, float releaseKnobMs)
{
    const double f0 = midiToHz(lowestNote);
    const double release = std::max(releaseKnobMs * 0.001, 0.5 / f0);   // the bass's release floor
    const double slot = slotBeats * 60.0 / bpm;
    return static_cast<float>(1.0 - (release + 0.004) / slot);
}

/** @brief The same limit for a pattern family. */
float gateLimit(int pattern, double bpm, int lowestNote, float releaseKnobMs)
{
    return gateLimitSlot(shortestBassSlot(pattern), bpm, lowestNote, releaseKnobMs);
}

/** @brief Length of the whole set in bars, from compose.set_minutes and the tempo: the arc's time base. */
double setLengthBars(const ParamStore& p)
{
    const int cb = p.base(ModuleE::COMPOSE);
    const double minutes = std::max(1.0, static_cast<double>(p.getInt(cb + compose::SET_MINUTES)));
    const double bpm = p.getBool(cb + compose::STYLE_TEMPO) ? styleProfile(styleOf(p)).m_bpmCentre
                                                           : std::max(20.0, static_cast<double>(p.get(cb + compose::BPM)));
    return std::max(32.0, minutes * bpm / BeatsPerBar);
}

/** @brief The uniform source sampleMasked() draws from. */
struct BassUniform {
    Rng* m_r;   ///< the stream
    /** @brief A uniform draw in [0, 1). */
    double operator()() const { return static_cast<double>(m_r->uniform()); }
};

/**
 * @brief Sixteenth step of slot @p s of @p beat in @p pat, inside a bar.
 *
 * The pattern slots are positions in beats, and three of the five families land on sixteenths
 * exactly. The Triplet family does not -- its 1/3 and 2/3 round to the first and the third sixteenth
 * -- so what the model is told about a triplet bass is the nearest sixteenth. That is a real
 * approximation and it is the only one available: the whole conditioning of docs/MODEL_FORMAT.md
 * section 3, and the corpus it was counted on, live on a sixteenth grid. It affects the `step`,
 * `bar`, `gap` and `kick` the model sees, never where the note is actually played.
 */
int bassSlotStep(const BassPatternDef& pat, int beat, int s)
{
    return beat * 4 + static_cast<int>(std::lround(pat.m_pos[s] * 4.0));
}

/**
 * @brief Draws one bass phrase (BassPhraseBars bars) for a pattern family from the learned role.
 *
 * The line is the whole phrase -- `BassPhraseBars` bars of `pat.count` notes a beat -- drawn in one
 * masked pass, because that is the unit the model was trained on: a bass loop, not a bar. It is a
 * function of the track's seed and of nothing else, so bar 3000 is bar 3000 whether the night was
 * played from the start or rendered in pieces (Composer.h, determinism).
 *
 * **The constraint set.** Scale tones only, and the register the pattern families already reach:
 * from the seventh below the root down an octave -- the note `makeTrack` computes its gate limit for
 * -- up to an octave above it. Held against the corpus, that window holds 90.2 % of real psytrance
 * bass notes (`Tools/train/bass_stats.py`); the 7.4 % below it and the 2.3 % above are given up on
 * purpose, because a lower note would invalidate the release floor the gate limit is computed from
 * and the bass would start cutting its own tail.
 *
 * @param steps the sixteenths of the phrase that carry a note, ascending, `bar * 16 + step`; with a
 *              pattern family those are its slots, with a drawn rhythm the onsets of its masks
 * @param slots one entry per step: where in `out` that note's interval goes
 * @param scale the track's mode
 * @param seed  the track's bass-line seed
 * @param out   BassPhraseSlots entries; slots the phrase does not use are left at 0
 * @return false when the model refuses a step or the masked draw fails; the caller then stays on the
 *         pattern generator for the whole track rather than mixing two sources within one phrase.
 */
bool drawBassPhrase(NeuralModel& model, const std::vector<int>& steps, const std::vector<int>& slots,
                    int scale, uint64_t seed, int8_t* out)
{
    const int lowRel = scaleDegree(scale, 6) - 12;     // the same note gateLimit() is computed for
    const int highRel = 12;
    std::vector<std::vector<uint8_t>> allowed;
    std::vector<NoteCond> cond;
    const size_t n = steps.size();
    if (n == 0 || slots.size() != n) return false;
    allowed.assign(n, std::vector<uint8_t>(static_cast<size_t>(CorpusAlphabet), 0));
    cond.resize(n);
    for (size_t i = 0; i < n; ++i) {
        for (int rel = lowRel; rel <= highRel; ++rel)
            if (inScale(scale, rel)) allowed[i][static_cast<size_t>(PitchModel::symbol(rel))] = 1;
        const int step = steps[i];
        cond[i].m_step = step % 16;
        cond[i].m_bar = (step / 16) % 8;
        cond[i].m_gap = noteGapCode(step, i + 1 < n ? steps[i + 1] : 0, i + 1 < n);
        cond[i].m_idx = noteIndexBucket(static_cast<int>(i));
        cond[i].m_kick = kickClass(step);
    }
    Rng r;
    r.seed(seed);
    NeuralStepper stepper{ &model, BassRole, 0, BassPhraseBars, &cond, 0 };
    std::vector<int> syms;
    if (!sampleMasked(stepper, allowed, PitchModel::symbol(0), PitchModel::symbol(0), 1.0,
                      BassUniform{ &r }, syms) || syms.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i) out[slots[i]] = static_cast<int8_t>(PitchModel::rel(syms[i]));
    return true;
}

/**
 * @brief Where the notes of one phrase sit: the steps the model is asked about and the slots they
 *        land in.
 *
 * One function for both rhythm sources, so the learned pitch phrase is laid out the same way whether
 * its rhythm comes from a family or from a drawn mask, and `composeBars` reads it with one index
 * either way. The slot is the note's **ordinal within its beat**, which is what the pattern path has
 * always written -- for the Rolling family the ordinal is the sixteenth, for Gallop it is not, and
 * the Triplet family has no sixteenth at all. The ordinal is also what the style profile's slot
 * envelope (PLAN 6.6) and the phrase figures are indexed by, so a drawn bar of three notes is the
 * same three slots a rolling bar is.
 *
 * @param masks BassPhraseBars onset masks, or null for the pattern family @p pat
 */
void phraseSlots(const BassPatternDef& pat, const uint16_t* masks, std::vector<int>& steps, std::vector<int>& slots)
{
    steps.clear();
    slots.clear();
    for (int bar = 0; bar < BassPhraseBars; ++bar)
        for (int beat = 0; beat < BeatsPerBar; ++beat) {
            if (masks == nullptr) {
                for (int s = 0; s < pat.m_count; ++s) {
                    steps.push_back(bar * 16 + bassSlotStep(pat, beat, s));
                    slots.push_back((bar * BeatsPerBar + beat) * 3 + s);
                }
            } else {
                int ordinal = 0;
                for (int k = 1; k < 4; ++k)
                    if ((masks[bar] >> (beat * 4 + k)) & 1u) {
                        steps.push_back(bar * 16 + beat * 4 + k);
                        slots.push_back((bar * BeatsPerBar + beat) * 3 + ordinal);
                        ++ordinal;
                    }
            }
        }
}

} // namespace

/**
 * @brief Fills a track's learned bass phrases, or leaves the plan on the pattern generator.
 *
 * Called once per track, on the composer's thread, after the track's two bass patterns are decided.
 * The model is loaded lazily on first use and never on the audio thread; a missing weight file
 * prints one line and leaves `bassNeural` false, which is exactly the behaviour the melodic side
 * has when `melody.phosmdl` is not there.
 */
void Composer::makeBassPhrases(const ParamStore& p, TrackPlan& t) const
{
    t.m_bassNeural = false;
    if (p.getInt(p.base(ModuleE::COMPOSE) + compose::BASS_MODEL) != 1) return;
    std::string note;
    NeuralModel* model = sharedBassModel(&note);
    if (model == nullptr) {
        if (!note.empty() && !m_bassModelReported) { std::fprintf(stderr, "%s\n", note.c_str()); m_bassModelReported = true; }
        return;
    }
    const uint64_t seed = mixSeed(trackSeed(t.m_index) ^ SaltBassLine, 0);
    const int patterns[2] = { t.m_primaryPattern, t.m_secondaryPattern };
    std::vector<int> steps, slots;
    for (int k = 0; k < 2; ++k) {
        // With a drawn rhythm the model is asked about the steps that will really sound, not about the
        // family's: `step`, `gap` and `kick` are three of its five conditioning inputs
        // (docs/MODEL_FORMAT.md 3), so telling it the family's slots and then playing other ones would
        // throw away exactly the part of the conditioning the rhythm round is about.
        phraseSlots(BassPatterns[patterns[k]], t.m_bassRhythm ? t.m_bassMask[k] : nullptr, steps, slots);
        if (!drawBassPhrase(*model, steps, slots, t.m_scale, mixSeed(seed, static_cast<uint64_t>(k)), t.m_bassRel[k]))
            return;
    }
    t.m_bassNeural = true;
}

/**
 * @brief Draws a track's two bass rhythm phrases, or leaves the plan on the pattern families.
 *
 * **The roll has to keep rolling.** The onset model of Corpus.h is a distribution over *bars*, and
 * drawing eight independent bars from it would be a bass that plays something else every bar -- which
 * is not what the corpus does and not what the genre does. The corpus was measured instead
 * (`Tools/corpus/bass_rhythm.py`, section 2): over its 1515 multi-bar bass lines a bar equals its
 * line's most common bar in **86.8 %** of the cases and the bar before it in 79.0 %, and the median
 * line has exactly **one** distinct bar pattern. A line is a figure, not a sequence of bars. So a
 * phrase here is a *home bar* plus rare departures, and both numbers come from the table:
 *
 * * the home bar is the track's pattern family with probability `1 - stray`, and a bar drawn from the
 *   model with probability `stray`. The families are the prior the round was told to keep, and this
 *   is where they are kept: with `stray` at zero the drawn rhythm *is* the pattern generator's.
 * * every bar of the phrase repeats the home bar unless a second coin -- `1 - CorpusBassHomePerMille`
 *   scaled the same way -- says it departs, and a departure draws its own bar from the model.
 *
 * **What decides how far a track strays.** `compose.bass_variation`, the knob that already decides
 * how often the bass leaves its primary pattern and how often it drops out, times the style profile's
 * `hatDensity` -- the one rhythmic-busyness multiplier a `StyleProfile` carries (Form.h). Progressive
 * (0.9) therefore strays least and Hi-Tech (1.2) most, which is the ordering the profiles already use
 * for percussion density and for squelch. A field of its own on `StyleProfile` would say this more
 * plainly.
 *
 * The phrase is a function of the track's seed and of nothing else, under a salt of its own, so
 * turning the knob on moves no other draw and bar N is bar N however the set is rendered.
 */
void Composer::makeBassRhythm(const ParamStore& p, TrackPlan& t) const
{
    t.m_bassRhythm = false;
    t.m_bassShortestSlot = 0.25f;
    const int cb = p.base(ModuleE::COMPOSE);
    if (p.getInt(cb + compose::BASS_RHYTHM) != 1) return;
    const StyleProfile& style = styleProfile(static_cast<StyleIdE>(t.m_style));
    const float amount = std::clamp(p.get(cb + compose::BASS_VARIATION) * style.m_hatDensity, 0.0f, 1.0f);
    const double stray = amount;
    const double depart = (1.0 - BassRhythm::homeShare()) * amount;
    const int patterns[2] = { t.m_primaryPattern, t.m_secondaryPattern };
    int shortest = 4;
    for (int k = 0; k < 2; ++k) {
        Rng r;
        r.seed(mixSeed(trackSeed(t.m_index) ^ SaltBassRhythm, static_cast<uint64_t>(k)));
        BassUniform u{ &r };
        const unsigned home = u() < stray ? BassRhythm::draw(u) : BassRhythm::familyMask(patterns[k]);
        for (int bar = 0; bar < BassPhraseBars; ++bar) {
            const unsigned mask = u() < depart ? BassRhythm::draw(u) : home;
            t.m_bassMask[k][bar] = static_cast<uint16_t>(mask);
            shortest = std::min(shortest, BassRhythm::shortestSpan(mask));
        }
    }
    // Every span is a whole number of sixteenths and at least one, so the shortest slot a drawn rhythm
    // can produce is a quarter beat -- exactly the Rolling family's. The gate limit and the release
    // floor of gateLimitSlot() can therefore only relax against the default, never tighten.
    t.m_bassShortestSlot = static_cast<float>(shortest) * 0.25f;
    t.m_bassRhythm = true;
}

/**
 * @brief Whether the floor under 140 Hz is free for the whole of a bar (Melody.h, the sub foundation).
 *
 * Since 19.09.2026 this is BarPlan::floorSilent rather than "this track's kick and bass rest": over the DJ
 * overlap the previous track's kick and bass still hold the floor, and a buildup's pre-drop break is a held
 * breath rather than a floor (Form.cpp).
 */
static bool foundationBar(const BarPlan& bp) { return bp.m_floorSilent; }

/** @name The pad's high pass while it lays the sub foundation (rule 20 of 18.09.2026).
 *  @{ */
constexpr float FoundationHpFloor = 40.0f;   ///< Hz: under the lowest sub root (D2, 73 Hz) by nearly an octave
constexpr float FoundationHpTrack = 0.5f;    ///< x f0: an octave under each voice, so a sub keeps its fundamental
/** @} */

/**
 * @brief Bars before the kick returns in which the drone's low octave is already over (19.09.2026).
 *
 * The drone's release is 2.5 s; two bars are 3.3 s at 145 BPM, after which a note released there has
 * fallen by about 60 dB (the pad's release was measured at 43 dB per bar for 1.8 s), so the band under
 * 140 Hz belongs to kick and bass again from their first beat. In those two bars the drone plays its
 * upper octave instead: the cross-fade from the floor into the section that follows.
 */
constexpr int DroneLowTail = 2;

/**
 * @brief One bar as the drone sees it: whether it plays, in which octave, and whether pad or acid
 *        share its band there (20.09.2026 -- a run breaks where that changes, because the chord the
 *        drone holds changes with it).
 */
struct DroneBar {
    bool m_on = false;       ///< the drone plays
    bool m_low = false;      ///< in the low octave
    bool m_shaded = false;   ///< pad or acid share its band
    int m_section = -1;      ///< the section it belongs to
};

/**
 * @brief The drone's state in bar @p inTrack: on where the form sets its bit, low where the floor is
 *        silent for this bar and the DroneLowTail bars after it (inside the track).
 */
static DroneBar droneBarAt(const TrackPlan& plan, const PartAvailability& a, int inTrack)
{
    DroneBar d;
    if (inTrack < 0 || inTrack >= plan.m_bars) return d;
    const BarPlan bp = planBar(plan.m_form, a, plan.m_sectionSeed, inTrack);
    d.m_section = bp.m_index;
    d.m_on = (bp.m_parts & partBit(MelodyPartE::DRONE)) != 0;
    if (!d.m_on) return d;
    d.m_low = bp.m_floorSilent;
    for (int k = 1; k <= DroneLowTail && d.m_low; ++k)
        d.m_low = inTrack + k < plan.m_bars && planBar(plan.m_form, a, plan.m_sectionSeed, inTrack + k).m_floorSilent;
    // Until 20.09.2026 the drone left a bar in which pad or acid sounded, which is why it was measured
    // absent (Form.cpp): nearly every bar of a track has one of the two. It stays now, in its raised
    // octave and reduced to its root where those two already own the fifth (composeMelodyBar), and the
    // masking it leaves is a measurement rather than a prohibition.
    d.m_shaded = !d.m_low && (bp.m_parts & (partBit(MelodyPartE::PAD) | partBit(MelodyPartE::ACID))) != 0;
    return d;
}

/**
 * @brief What a bar needs to know beyond its BarPlan (Melody.h, MelodyContext), from the whole form.
 *
 * - *material*: the cell set -- 0 up to the track's first breakdown, 1 after it, 0 again after a
 *   second one. New material arrives at a section boundary and nowhere else (rule 4), and the riff
 *   the track opened with comes back in its third part.
 * - *foundationBars*: at the start of a pad chord in a bar without kick and bass, how many bars the
 *   silence lasts from here, read from the form's own bar plans. The pad's sub root ends a bar before
 *   it does (composeMelodyBar), which is why the length has to be known when the chord starts. Where
 *   the drone lies on the floor in its low octave the pad lays no sub: the two would double it.
 * - *droneBars*, *droneLow* (19.09.2026): at the first bar of a drone run -- consecutive drone bars of
 *   one section with the same octave -- how long the run is and which octave it takes.
 *
 * Every field is a function of the plan and the bar's place in it, so a bar composed alone is the
 * bar composed in sequence.
 */
MelodyContext melodyContext(const TrackPlan& plan, const BarPlan& bp, int inTrack)
{
    MelodyContext c;
    const int index = std::clamp(bp.m_index, 0, std::max(0, plan.m_form.m_count - 1));
    int breaks = 0;
    for (int s = 0; s < index; ++s) breaks += plan.m_form.m_section[s].m_type == SectionTypeE::BREAK ? 1 : 0;
    c.m_material = breaks % MaterialSets;
    const PartAvailability a = availabilityOf(plan);
    const bool droneHere = (bp.m_parts & partBit(MelodyPartE::DRONE)) != 0 && plan.m_melody.m_present[mpIndex(MelodyPartE::DRONE)];
    DroneBar here;
    if (droneHere) here = droneBarAt(plan, a, inTrack);
    if ((bp.m_parts & partBit(MelodyPartE::PAD)) != 0 && plan.m_melody.m_present[mpIndex(MelodyPartE::PAD)] && foundationBar(bp)
        // A foundation starts where a chord block starts -- or where the floor falls silent inside one
        // (22.09.2026). Until then a block was two or four bars, so a breakdown's second block
        // always began on silent ground; a breakdown holds one chord for the whole of it now, its block
        // starts on the breakdown's first bar with the kick still on the downbeat, and without this
        // clause the sub never arrived at all (testFoundation.score: "0 of 0 chords in silent bars").
        && (padChordAt(plan.m_melody, bp, inTrack).m_barInBlock == 0 || inTrack == 0
            || !foundationBar(planBar(plan.m_form, a, plan.m_sectionSeed, inTrack - 1)))
        && !(droneHere && here.m_low)) {
        int k = 1;
        while (inTrack + k < plan.m_bars && k < 64 && foundationBar(planBar(plan.m_form, a, plan.m_sectionSeed, inTrack + k))) ++k;
        c.m_foundationBars = k;
    }
    if (droneHere && here.m_on) {
        const DroneBar before = droneBarAt(plan, a, inTrack - 1);
        // A run also starts at the hand-over of the DJ overlap (20.09.2026). Over the overlap the
        // *incoming* track's voices are written by transitionBar, and it writes pads and drone only
        // where the two keys are compatible -- so for an incompatible pair nothing of this track's
        // drone is emitted in its first sixteen bars, while droneBarAt still says "on" for them and
        // the run therefore looked as if it had begun there. The whole intro then stayed silent:
        // measured on the listening seed, the drone sounded in 0 of track 2's 32 intro bars. Starting
        // a run at the hand-over costs a consonant guest one re-trigger, which its 2.5 s release
        // turns into the cross-fade the drone uses at every other section boundary anyway.
        const bool start = !before.m_on || before.m_section != here.m_section || before.m_low != here.m_low
                        || (plan.m_form.m_handover > 0 && inTrack == plan.m_form.m_handover);
        if (start) {
            // The run is walked to its end rather than to a fixed 64 bars (20.09.2026): with the drone
            // under every bar a run is a whole section, and a cap inside a section would have ended the
            // held chord in the middle of one with nothing to follow it. A section can never be longer
            // than the track, so plan.bars is the bound.
            int k = 1;
            bool shaded = here.m_shaded;
            while (k < plan.m_bars) {
                const DroneBar next = droneBarAt(plan, a, inTrack + k);
                if (!next.m_on || next.m_section != here.m_section || next.m_low != here.m_low) break;
                shaded = shaded || next.m_shaded;
                ++k;
            }
            c.m_droneBars = k;
            c.m_droneLow = here.m_low;
            c.m_droneTail = 0;
            // What the run holds is decided once, here, and not bar by bar: the drone's attack is 1.5 s,
            // so a chord that changed whenever the pad came and went would pump instead of carrying. A
            // run in which pad or acid sound *anywhere* keeps the root alone; only a run with the band
            // to itself adds the fifth (composeMelodyBar).
            c.m_droneShaded = shaded;
        }
    }
    return c;
}

/** @brief Whether the drone plays bar @p inTrack in its low octave (the high pass is opened for it). */
bool droneLowAt(const TrackPlan& plan, int inTrack)
{
    if (!plan.m_melody.m_present[mpIndex(MelodyPartE::DRONE)]) return false;
    return droneBarAt(plan, availabilityOf(plan), inTrack).m_low;
}

PartAvailability availabilityOf(const TrackPlan& plan)
{
    PartAvailability a;
    for (int k = 0; k < MelodyParts; ++k) a.m_part[k] = plan.m_melody.m_present[k];
    for (int k = 0; k < MelodyParts; ++k) a.m_amount[k] = plan.m_melody.m_amount[k];
    a.m_leadLo = plan.m_melody.m_leadLo;
    a.m_leadHi = plan.m_melody.m_leadHi;
    a.m_arpLo = plan.m_melody.m_arpLo;
    a.m_arpHi = plan.m_melody.m_arpHi;
    a.m_percLayers = plan.m_perc.m_layers;
    a.m_hatLayers = plan.m_perc.m_hatLayers;
    return a;
}

void Composer::setLock(LockUnitE unit, int index, bool locked)
{
    if (locked) m_locked[static_cast<int>(unit)][index] = 1;
    else m_locked[static_cast<int>(unit)].erase(index);
    m_plans.clear();
    m_walk.clear();
    ++m_planGeneration;
}

bool Composer::isLocked(LockUnitE unit, int index) const
{
    const auto& m = m_locked[static_cast<int>(unit)];
    return m.find(index) != m.end();
}

void Composer::reroll(LockUnitE unit, int index)
{
    if (isLocked(unit, index)) return;
    ++m_variation[static_cast<int>(unit)][index];
    m_plans.clear();
    m_walk.clear();
    ++m_planGeneration;
}

uint32_t Composer::variation(LockUnitE unit, int index) const
{
    const auto& m = m_variation[static_cast<int>(unit)];
    const auto it = m.find(index);
    return it == m.end() ? 0u : it->second;
}

void Composer::setVariation(LockUnitE unit, int index, uint32_t value)
{
    if (value == 0) m_variation[static_cast<int>(unit)].erase(index);
    else m_variation[static_cast<int>(unit)][index] = value;
    m_plans.clear();
    m_walk.clear();
    ++m_planGeneration;
}

void Composer::clearLocks()
{
    for (int u = 0; u < NumLockUnits; ++u) { m_locked[u].clear(); m_variation[u].clear(); }
    m_plans.clear();
    m_walk.clear();
    ++m_planGeneration;
}

uint64_t Composer::setSeed() const
{
    const uint32_t v = variation(LockUnitE::SET, 0);
    return v == 0 ? m_seed : mixSeed(m_seed ^ SaltReroll, v);
}

uint64_t Composer::trackSeed(int index) const
{
    // A locked track is frozen at the seed it had before any reroll, of the set or of itself; an
    // unlocked one follows the set's rerolls and adds its own variation counter.
    if (isLocked(LockUnitE::TRACK, index)) return mixSeed(m_seed ^ SaltTrack, static_cast<uint64_t>(index));
    const uint64_t base = mixSeed(setSeed() ^ SaltTrack, static_cast<uint64_t>(index));
    const uint32_t v = variation(LockUnitE::TRACK, index);
    return v == 0 ? base : mixSeed(base ^ SaltReroll, v);
}

uint64_t Composer::sectionSeedOf(int track, int si) const
{
    const int idx = sectionUnitIndex(track, si);
    if (isLocked(LockUnitE::SECTION, idx))
        return mixSeed(mixSeed(m_seed ^ SaltTrack, static_cast<uint64_t>(track)) ^ SaltSectU, static_cast<uint64_t>(si));
    const uint64_t base = mixSeed(trackSeed(track) ^ SaltSectU, static_cast<uint64_t>(si));
    const uint32_t v = variation(LockUnitE::SECTION, idx);
    return v == 0 ? base : mixSeed(base ^ SaltReroll, v);
}

uint64_t Composer::laneSeedOf(int track, int lane) const
{
    const int idx = laneUnitIndex(track, lane);
    if (isLocked(LockUnitE::PATTERN_LANE, idx))
        return mixSeed(mixSeed(m_seed ^ SaltTrack, static_cast<uint64_t>(track)) ^ SaltLaneU, static_cast<uint64_t>(lane));
    const uint64_t base = mixSeed(trackSeed(track) ^ SaltLaneU, static_cast<uint64_t>(lane));
    const uint32_t v = variation(LockUnitE::PATTERN_LANE, idx);
    return v == 0 ? base : mixSeed(base ^ SaltReroll, v);
}

std::vector<int> Composer::planKnobIds(const ParamStore& p)
{
    std::vector<int> ids;
    const int cb = p.base(ModuleE::COMPOSE);
    for (int i = 0; i < compose::COUNT; ++i) ids.push_back(cb + i);
    ids.push_back(p.base(ModuleE::KICK) + kick::ENGINE);
    ids.push_back(p.base(ModuleE::KICK) + kick::CLIP);
    ids.push_back(p.base(ModuleE::BASS) + bass::AMP_RELEASE);
    for (int l = 0; l < PercLanes; ++l) {
        const int b = p.base(ModuleE::PERC, l);
        for (int k : { static_cast<int>(perc::ACTIVE), static_cast<int>(perc::ROLE), static_cast<int>(perc::DENSITY), static_cast<int>(perc::ENGINE) })
            ids.push_back(b + k);
    }
    return ids;
}

void Composer::validate(const ParamStore& p) const
{
    // The plans depend on these knobs; when any of them moves, the plans are made again. Called by Entry, once per
    // outermost call (Composer.h, "Knobs that move while the composer works").
    const std::vector<int> ids = planKnobIds(p);
    std::vector<float> knobs;
    knobs.reserve(ids.size() + 1);
    for (int id : ids) knobs.push_back(p.get(id));
    knobs.push_back(static_cast<float>(preferencesRevision()));   // new preferences: new plans (Preferences.h)
    if (knobs != m_planKnobs) { m_plans.clear(); m_walk.clear(); ++m_planGeneration; m_planKnobs = std::move(knobs); }
}

/**
 * @brief The set walk: length, key, mode, tempo and the two sound recipes of every track.
 *
 * These are the journey through the night, so they come from the set seed alone and not from any
 * track's own seed. Rerolling one track therefore leaves the walk -- and with it every other track --
 * untouched (PLAN 6.8); rerolling the set moves the whole walk.
 */
const TrackWalk& Composer::walkAt(const ParamStore& p, int index) const
{
    const int cb = p.base(ModuleE::COMPOSE);
    const float tv = p.get(cb + compose::TRACK_VARIATION);
    const StyleProfile& style = styleProfile(styleOf(p));
    const bool styleTempo = p.getBool(cb + compose::STYLE_TEMPO);
    const double baseBpm = styleTempo ? style.m_bpmCentre : p.get(cb + compose::BPM);
    const double range = styleTempo ? style.m_bpmRange : p.get(cb + compose::TEMPO_RANGE);
    // Track lengths are multiples of 16 bars (19.09.2026; until then 32, so that every track boundary fell
    // on a 32-bar grid of the set -- the DJ overlap of the arrangement round moves every later track by
    // sixteen bars against that grid anyway, and a track's own form is what its phrases count from).
    const int baseBars = std::clamp((p.getInt(cb + compose::TRACK_BARS) / TrackBarStep) * TrackBarStep, MinTrackBars, MaxTrackBars);

    while (static_cast<int>(m_walk.size()) <= index) {
        const int i = static_cast<int>(m_walk.size());
        TrackWalk w;
        if (i == 0) {
            w.m_bars = baseBars;
            w.m_key = p.getInt(cb + compose::KEY);
            w.m_scale = p.getInt(cb + compose::SCALE);
            w.m_bpm = styleTempo ? std::round(style.m_bpmCentre * 2.0) * 0.5 : baseBpm;
            w.m_style = static_cast<int>(styleOf(p));
            // 21.09.2026, at the user's decision: the knobs still decide the first track's key,
            // mode, tempo and length -- those are musical settings somebody typed in -- but its
            // *sound* is now drawn like every other track's. Until today the branch ended in a
            // `continue`, so track 1 had no kick recipe, no bass recipe, no acid voicing and no
            // voice recipes at all: it was the knobs exactly. A track is 256 bars, so every render
            // shorter than about seven minutes is track 1 alone -- which meant the whole palette of
            // 15 pad tables, four oscillators and four filter responses never sounded once in a
            // short render, whatever the seed. The knobs are the centre now, not the whole story.
        } else {
            const TrackWalk& prev = m_walk[static_cast<size_t>(i - 1)];
            Rng r;
            r.seed(mixSeed(setSeed() ^ SaltWalk, static_cast<uint64_t>(i)));

            // The style journey (22.09.2026). The user: "es ist langweilig, wenn immer alles im selben
            // Stil ist in einem Set". One style for the night was the knob; with compose.style_mix on
            // the style walks -- but as a journey, not a lottery: the five profiles stand in tempo order
            // (Progressive 136.5, Full-On 144, Goa 145, Dark Forest 151.5, Hi-Tech 158) and a step goes
            // to a *neighbour*, upward while the set's energy arc rises and downward while it falls,
            // now and then against it; so a DJ's tempo never jumps more than one profile at a time and
            // the night has a shape. A style holds for one to three tracks. Its own generator, so
            // nothing the older draws below decide moves because of it.
            w.m_style = prev.m_style;
            // Track Variation at 0 means every track plays the knobs, the style among them (testVariety.plans).
            if (p.getBool(cb + compose::STYLE_MIX) && tv > 0.0f) {
                static const StyleIdE ByTempo[NumStyles] = { StyleIdE::PROGRESSIVE, StyleIdE::FULL_ON, StyleIdE::GOA, StyleIdE::DARK_FOREST, StyleIdE::HI_TECH };
                Rng rs;
                rs.seed(mixSeed(setSeed() ^ SaltStyle, static_cast<uint64_t>(i)));
                int pos = 1;
                for (int k = 0; k < NumStyles; ++k) if (static_cast<int>(ByTempo[k]) == prev.m_style) pos = k;
                int run = 1;
                for (int k = i - 2; k >= 0 && m_walk[static_cast<size_t>(k)].m_style == prev.m_style; --k) ++run;
                double startBar = 0.0, prevStart = 0.0;
                for (int k = 0; k < i; ++k) { if (k == i - 1) prevStart = startBar; startBar += m_walk[static_cast<size_t>(k)].m_bars; }
                const double setBars = setLengthBars(p);
                const bool rising = arcEnergy(arcOf(p), startBar / setBars) >= arcEnergy(arcOf(p), prevStart / setBars);
                const bool move = rs.uniform() < 0.4f || run >= 3;
                const bool against = rs.uniform() < 0.25f;
                if (move) {
                    int dir = (rising != against) ? 1 : -1;
                    if (pos + dir < 0 || pos + dir >= NumStyles) dir = -dir;
                    pos = std::clamp(pos + dir, 0, NumStyles - 1);
                    w.m_style = static_cast<int>(ByTempo[pos]);
                }
            }
            // From here on the track's own profile: its modes, and its tempo centre when Style Tempo is on.
            const StyleProfile& ts = styleProfile(static_cast<StyleIdE>(w.m_style));
            const double trackBpm = styleTempo ? ts.m_bpmCentre : baseBpm;
            const double trackRange = styleTempo ? ts.m_bpmRange : range;

            // Length: the knob, give or take one 16-bar block -- at the default 256 bars that is 240 .. 272, inside
            // the user's 220 .. 280 (19.09.2026; until then two 32-bar blocks either way).
            w.m_bars = baseBars;
            if (r.uniform() < tv) w.m_bars = std::clamp(baseBars + TrackBarStep * (r.below(3) - 1), MinTrackBars, MaxTrackBars);

            // Key: by fifths and whole tones, a minor third now and then, rarely a semitone. 22.09.2026,
            // the user ("auch immer dieselbe Tonart"): the knob's chance is raised by a quarter, and after
            // two tracks in one key the third *must* move -- a key held for three tracks is over twenty
            // minutes of one tonic. The coin is still tossed once, so the draws after it keep their place.
            w.m_key = prev.m_key;
            const bool heldTwice = tv > 0.0f && i >= 2 && m_walk[static_cast<size_t>(i - 2)].m_key == prev.m_key;
            const float keyCoin = r.uniform();   // drawn whatever tv is, so the draws after it keep their place
            const bool moveKey = tv > 0.0f && keyCoin < std::min(1.0f, tv + 0.25f);
            if (moveKey || heldTwice) {
                static const int Moves[8] = { 7, 5, 2, -2, 3, -3, 1, -1 };
                static const double Weights[8] = { 0.28, 0.28, 0.12, 0.12, 0.06, 0.06, 0.04, 0.04 };
                w.m_key = ((prev.m_key + Moves[pick(r, Weights, 8)]) % 12 + 12) % 12;
            }
            // Mode: mostly kept; when it changes, the track's style profile's weights decide.
            w.m_scale = prev.m_scale;
            if (r.uniform() < 0.25f * tv) w.m_scale = pick(r, ts.m_scaleWeight, NumScales);
            // The family jam (02.10.2026, setJam): a follower holds its key and takes the leader's mode.
            if (m_jamHold) {
                w.m_key = prev.m_key;
                if (m_jamScale >= 0 && m_jamScale < NumScales) w.m_scale = m_jamScale;
            }
            // Tempo: mean-reverting walk around the track's centre, inside its range, on half-BPM steps.
            // The centre itself follows the set's energy arc (ArcTempoPerUnit): where the night rises the
            // tracks run a little faster, where it closes a little slower.
            double arcCentre = trackBpm;
            {
                double startBar = 0.0;
                for (int k = 0; k < i; ++k) startBar += m_walk[static_cast<size_t>(k)].m_bars;
                const ArcIdE arc = arcOf(p);
                arcCentre += ArcTempoPerUnit * (arcEnergy(arc, startBar / setLengthBars(p)) - arcEnergy(arc, 0.0));
            }
            const double walk = arcCentre + 0.6 * (prev.m_bpm - arcCentre) + (2.0 * r.uniform() - 1.0) * trackRange * tv;
            w.m_bpm = std::round(std::clamp(walk, arcCentre - trackRange, arcCentre + trackRange) * 2.0) * 0.5;

        }

        // The set's motif (Melody.h, SetMotif; 23.09.2026, round "Set-Kurve"). The first track draws it --
        // a lead cell in the band its style's lead vector puts it in, and an archetype -- and states it in
        // its first lead phrase. The track that carries the set's end (compose.set_minutes) recalls it in
        // its second phrase, and so does about one track in seven in between, never the second track (a
        // recall right after the statement is a repeat). Its own stream, after the walk's older draws and
        // before the recipes, which have streams of their own: nothing else moves. A set rendered past its
        // length has no further recall; the recall belongs to the end the arc knows.
        {
            Rng rm;
            rm.seed(mixSeed(setSeed() ^ SaltMotif, static_cast<uint64_t>(i)));
            if (i == 0) {
                const LeadStyle& ls = styleProfile(static_cast<StyleIdE>(w.m_style)).m_lead;
                w.m_motif.m_band = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(ls.m_density * 2.0 + (static_cast<double>(rm.uniform()) - 0.5))), 0, 2));
                w.m_motif.m_cell = drawMotifCell(mixSeed(setSeed() ^ SaltMotif, 0xC0FFEEull), w.m_motif.m_band);
                w.m_motif.m_archetype = static_cast<int8_t>(pick(rm, ls.m_archetype, NumLeadArchetypes));
                w.m_motif.m_phrase = 0;
            } else {
                w.m_motif = m_walk[0].m_motif;
                double startBar = 0.0;
                for (int k = 0; k < i; ++k) startBar += m_walk[static_cast<size_t>(k)].m_bars;
                const double setBars = setLengthBars(p);
                const bool carriesEnd = startBar < setBars && startBar + w.m_bars >= setBars;
                const bool recall = rm.uniform() < 0.15f;   // drawn whatever i is, so the stream is one per track
                w.m_motif.m_phrase = (carriesEnd || (i >= 2 && recall)) ? 1 : -1;
            }
        }

        // Recipes by best candidate against the last four tracks.
        Rng rr;
        rr.seed(mixSeed(setSeed() ^ SaltRecipe, static_cast<uint64_t>(i)));
        auto chooseRecipe = [&](float* out, int n, bool kickSide) {
            float best[NumKickMacros] = {};
            double bestScore = -1.0;
            for (int c = 0; c < 12; ++c) {
                float cand[NumKickMacros] = {};
                drawRecipe(rr, cand, n);
                double score = 1e9;
                for (int back = 1; back <= 4 && i - back >= 0; ++back) {
                    const TrackWalk& o = m_walk[static_cast<size_t>(i - back)];
                    const float* om = kickSide ? o.m_kickMacro : o.m_bassMacro;
                    double d = 0.0;
                    for (int k = 0; k < n; ++k) d += (cand[k] - om[k]) * (cand[k] - om[k]);
                    score = std::min(score, std::sqrt(d));
                }
                // The first track has nothing behind it, so every candidate scored 1e9 and the *first*
                // draw won -- an unspread random point (22.09.2026). That was harmless while the first
                // track played the knobs, and became the level of the whole night when it started
                // drawing a recipe like every other track (21.09.2026): the first track is the level
                // match's reference, so a foundation that happened to come out quiet pulls every later
                // track down with it. Measured on seed 31: its probe read -17.0 LUFS against -10.4,
                // -11.1 and -12.8 for the three after it, which corrected them by -6.6, -5.9 and
                // -4.3 dB; the set sat 3.5 dB under where the knobs put it, and the probe's own error
                // was 1.1 dB for that recipe against 0.2 for the others -- the synthetic two-bar loop
                // represents an ordinary foundation well and an extreme one less well.
                //
                // So the first track takes the *mildest* of its twelve candidates instead of the
                // first: the opening track of a set is a variation of the settings somebody typed in,
                // which is what it should be anyway, and the reference is then stable by construction
                // rather than by luck. All twelve are still drawn, so no later track moves.
                if (i == 0) {
                    double norm = 0.0;
                    for (int k = 0; k < n; ++k) norm += static_cast<double>(cand[k]) * cand[k];
                    score = -std::sqrt(norm);
                }
                if (score > bestScore) { bestScore = score; std::copy(cand, cand + n, best); }
            }
            std::copy(best, best + n, out);
        };
        chooseRecipe(w.m_kickMacro, NumKickMacros, true);
        chooseRecipe(w.m_bassMacro, NumBassMacros, false);

        // The acid voicing (19.09.2026): a point in the triangle of the three voicings. Candidates are
        // uniform on the triangle -- normalised exponential draws, the flat Dirichlet distribution -- and
        // the one farthest from the previous two tracks' points wins, the same best-candidate rule as
        // the recipes. Its own generator, so the kick and bass recipes above are the ones they were.
        Rng ra;
        ra.seed(mixSeed(setSeed() ^ SaltAcidVoice, static_cast<uint64_t>(i)));
        double bestAcid = -1.0;
        for (int c = 0; c < 12; ++c) {
            float cand[NumAcidVoicings];
            float sum = 0.0f;
            for (float& x : cand) { x = static_cast<float>(-std::log(1.0 - 0.999999 * static_cast<double>(ra.uniform()))); sum += x; }
            for (float& x : cand) x /= sum;
            double score = 1e9;
            for (int back = 1; back <= 2 && i - back >= 0; ++back) {
                const float* o = m_walk[static_cast<size_t>(i - back)].m_acidVoicing;
                double d = 0.0;
                for (int k = 0; k < NumAcidVoicings; ++k) d += (cand[k] - o[k]) * (cand[k] - o[k]);
                score = std::min(score, std::sqrt(d));
            }
            // The first track keeps the knobs' voicing exactly (TrackWalk's own default, the driven
            // corner): unlike the kick, the bass and the six voices, this is a three-way character
            // switch -- clean, driven, liquid -- and the opening track of a set playing the acid
            // character somebody set is right, while the difference between it and a point 0.95 of
            // the way there is inaudible. All twelve candidates are still drawn, so no later track
            // moves. testAcidVoicing.corners states this as a property; it went red when the first
            // track began drawing a recipe (21.09.2026) and took candidate 0 here too.
            if (i == 0) continue;
            if (score > bestAcid) { bestAcid = score; std::copy(cand, cand + NumAcidVoicings, w.m_acidVoicing); }
        }

        // The voice recipes (19.09.2026, Composer.h, VoiceRecipe): per voice twelve candidates -- an
        // oscillator, a table and a response from the voice's palette (VoicePalettes), weighted by the
        // palette, delay times, and five directions from the same truncated normal the kick
        // and bass recipes use -- and the candidate farthest from the same voice in the previous two
        // tracks wins, so that neighbouring tracks never share a voice's sound. A discrete choice that
        // differs counts as one unit of distance (a whole direction's range is two). Its own generator:
        // nothing drawn above moves.
        Rng rv;
        rv.seed(mixSeed(setSeed() ^ SaltVoice, static_cast<uint64_t>(i)));
        // The counter-lead against this track's lead (19.09.2026, the user: "Die Counter-Lead sollte
        // natuerlich einen anderen Sound haben als die Haupt-Lead"). Until then a voice's candidates were
        // scored only against the same voice in the two tracks before, and the counter's palette shared two
        // of the lead lane's tables and three of its oscillators, so a track could give both the same
        // oscillator and table an octave apart. Now the counter's palette is the vocal family alone (Formant
        // Saw, Vocal, Glass), a candidate with the lead's oscillator or table is never taken, a different
        // filter response counts, and the distance to the lead is part of the score.
        const int leadV = polyIndex(PolyInstanceE::LEAD), counterV = polyIndex(PolyInstanceE::COUNTER);
        static_assert(polyIndex(PolyInstanceE::LEAD) < polyIndex(PolyInstanceE::COUNTER), "the lead is drawn before the counter");
        for (int v = 0; v < PolyInstances; ++v) {
            const VoicePalette& pal = VoicePalettes[v];
            const int nTables = paletteTableCount(pal);
            double bestScore = -1.0;
            for (int c = 0; c < 12; ++c) {
                VoiceRecipe cand;
                cand.m_osc = pick(rv, pal.m_osc, static_cast<int>(PolyOscE::COUNT));
                cand.m_table = nTables > 0 ? paletteTableAt(pal, rv.below(nTables)) : -1;
                cand.m_filter = pick(rv, pal.m_filter, static_cast<int>(PolyFilterE::COUNT));
                // The echo is a property of the *role* since 20.09.2026 (round "dialogue"). The user's
                // rule: "Lead auf ein punktiertes Achtel (3/16), Counter auf ein schnelles 1/16
                // Ping-Pong". Until then every voice drew its two times from the same four, so a track
                // could give the lead a sixteenth and the counter a dotted eighth -- exactly the wrong
                // way round -- and the contrast the rule is about was a matter of luck. Now each voice
                // draws inside its own family (kVoiceDelayL / kVoiceDelayR), so a track still varies
                // its echoes but never trades the two leads' characters. Two draws either way, so no
                // other decision of this candidate moves.
                const VoiceDelayFamily& fam = VoiceDelay[v];
                cand.m_delayL = fam.m_left[rv.below(fam.m_leftCount)];
                cand.m_delayR = fam.m_right[rv.below(fam.m_rightCount)];
                // The second oscillator (22.09.2026): which one answers, and at which interval. Two
                // draws whatever the outcome, so "no second oscillator this track" costs the same
                // random numbers as any other result and no other decision of this candidate moves.
                cand.m_osc2 = pick(rv, pal.m_osc2, static_cast<int>(PolyOsc2E::COUNT));
                cand.m_osc2Semis = pick(rv, pal.m_interval, static_cast<int>(PolyOsc2IntervalE::COUNT));
                cand.m_hasOsc2 = true;
                drawRecipe(rv, cand.m_macro, NumVoiceMacros);
                double score = 1e9;
                for (int back = 1; back <= 2 && i - back >= 0; ++back) {
                    const VoiceRecipe& o = m_walk[static_cast<size_t>(i - back)].m_voice[v];
                    double d = 0.0;
                    for (int k = 0; k < NumVoiceMacros; ++k) d += (cand.m_macro[k] - o.m_macro[k]) * (cand.m_macro[k] - o.m_macro[k]);
                    d = std::sqrt(d) + (cand.m_osc != o.m_osc ? 1.0 : 0.0) + (cand.m_table != o.m_table ? 1.0 : 0.0)
                      + (cand.m_filter != o.m_filter ? 0.5 : 0.0)
                      // The second oscillator counts like the filter: half a unit for the kind and
                      // half for the interval. A track whose pad answers an octave lower where the
                      // last one answered at a fifth is a different pad, whatever else it drew.
                      + (cand.m_osc2 != o.m_osc2 ? 0.5 : 0.0) + (cand.m_osc2Semis != o.m_osc2Semis ? 0.5 : 0.0);
                    score = std::min(score, d);
                }
                if (v == counterV) {
                    const VoiceRecipe& lead = w.m_voice[leadV];
                    if (cand.m_osc == lead.m_osc || cand.m_table == lead.m_table) continue;   // never the lead's sound
                    double d = 0.0;
                    for (int k = 0; k < NumVoiceMacros; ++k) d += (cand.m_macro[k] - lead.m_macro[k]) * (cand.m_macro[k] - lead.m_macro[k]);
                    score = std::min(score, 1e8) + 0.5 * std::sqrt(d) + (cand.m_filter != lead.m_filter ? 0.5 : 0.0);
                }
                // The first track: the mildest macro vector, not the first draw (22.09.2026). Its
                // parts are the reference the level match brings every later track's to
                // (Composer.h, "Level match"), so an extreme first draw moves the whole night. The
                // discrete choices -- oscillator, table, filter, the two delays -- are left exactly
                // as drawn: those are what gives the first track a sound of its own, which is the
                // point of it having a recipe at all.
                if (i == 0) {
                    double norm = 0.0;
                    for (int k = 0; k < NumVoiceMacros; ++k) norm += static_cast<double>(cand.m_macro[k]) * cand.m_macro[k];
                    score = -std::sqrt(norm);
                }
                if (score > bestScore) { bestScore = score; w.m_voice[v] = cand; }
            }
            if (v == counterV) {
                // No candidate differed from the lead in both (possible when all twelve drew its
                // oscillator or its table -- more so now that the lead's own pool is much wider, round
                // "wavetable-selection": seed 77 hit this on a real track and exposed the bug below):
                // take the counter's likeliest other oscillator and the first palette table that is not
                // the lead's.
                //
                // `bestScore` staying at its initial -1.0 is the sign that every candidate was rejected,
                // so `w.voice[v]` was never written this loop and still holds VoiceRecipe's own defaults
                // (osc -1, table -1, "the knob"). Until 20.09.2026 the two fixes below fired only on
                // `c.osc == lead.osc` / `c.table == lead.table`, which is never true for -1 against a
                // real (>= 0) lead value -- so this exact case slipped through as "no collision, nothing
                // to fix" and left the counter to whatever the knob's own default oscillator and table
                // are, which can coincide with the lead's drawn ones by simple bad luck (seed 77,
                // testVoices.counterSound77: 1 of 24 tracks). `bestScore <= -1.0` is the direct test for
                // that case and triggers the same remediation the collision case already used.
                VoiceRecipe& c = w.m_voice[v];
                const VoiceRecipe& lead = w.m_voice[leadV];
                const bool noCandidateSurvived = bestScore <= -1.0;
                if (noCandidateSurvived || c.m_osc == lead.m_osc) {
                    int best = -1;
                    for (int o = 0; o < static_cast<int>(PolyOscE::COUNT); ++o)
                        if (o != lead.m_osc && pal.m_osc[o] > 0.0 && (best < 0 || pal.m_osc[o] > pal.m_osc[best])) best = o;
                    c.m_osc = best;
                }
                if (noCandidateSurvived || c.m_table == lead.m_table)
                    for (int k = 0; k < nTables; ++k) { const int t = paletteTableAt(pal, k); if (t != lead.m_table) { c.m_table = t; break; } }
            }
        }
        // The sound presets (26.09.2026; SoundPresets.h): for every synth a group of the bank weighted by the track's
        // style -- a group of the two tracks before it at a sixth of its weight, so neighbours differ -- and a cell of
        // its eight by eight grid. Their own stream, so no draw above moves.
        {
            Rng rp;
            rp.seed(mixSeed(setSeed() ^ SaltSoundPreset, static_cast<uint64_t>(i)));
            for (int k = 0; k < SoundSynths; ++k) {
                const ModuleE m = k == 0 ? ModuleE::KICK : k == 1 ? ModuleE::BASS : k == 2 ? ModuleE::ACID : ModuleE::POLY;
                const std::vector<SoundPreset>& bank = factoryPresets(m, k >= 3 ? k - 3 : 0);
                const int groups = static_cast<int>(bank.size() / 64);
                if (groups <= 0) continue;
                const int st = std::clamp(w.m_style, 0, 4);
                // The counter never plays the lead's sound (23.09.2026, round "Counter": another oscillator, and so
                // another table), with presets too: a group of the lead's oscillator is left out (26.09.2026 -- on the
                // listening seed both played Vocal Formant, and drop 2 lost its lift, testClimax).
                auto oscOf = [](const SoundPreset& sp) {
                    for (const auto& kv : sp.m_values) if (kv.first == poly::OSC) return static_cast<int>(std::lround(kv.second));
                    return -1;
                };
                const int leadOsc = k == 4 ? oscOf(factoryPresets(ModuleE::POLY, 0)[static_cast<size_t>(w.m_soundPreset[3])]) : -1;
                double weight[16] = {}, total = 0.0;
                for (int g = 0; g < groups && g < 16; ++g) {
                    double wg = static_cast<double>(bank[static_cast<size_t>(g * 64)].m_style[st]);
                    for (int back = 1; back <= 2 && i - back >= 0; ++back)
                        if (m_walk[static_cast<size_t>(i - back)].m_soundPreset[k] / 64 == g) wg /= 6.0;
                    if (leadOsc >= 0 && oscOf(bank[static_cast<size_t>(g * 64)]) == leadOsc) wg = 0.0;
                    weight[g] = wg;
                    total += wg;
                }
                double u = static_cast<double>(rp.uniform()) * total;
                int g = 0;
                while (g < groups - 1 && u >= weight[g]) { u -= weight[g]; ++g; }
                w.m_soundPreset[k] = g * 64 + rp.below(64);
            }
        }
        m_walk.push_back(w);
    }
    return m_walk[static_cast<size_t>(index)];
}

TrackPlan Composer::makeTrack(const ParamStore& p, int index) const
{
    const int cb = p.base(ModuleE::COMPOSE);
    const float tv = p.get(cb + compose::TRACK_VARIATION);
    const float sv = p.get(cb + compose::SOUND_VARIATION);
    const int reg = p.getInt(cb + compose::BASS_REGISTER);
    const float releaseKnob = p.get(p.base(ModuleE::BASS) + bass::AMP_RELEASE);
    const TrackWalk& w = walkAt(p, index);
    const StyleProfile& style = styleProfile(static_cast<StyleIdE>(w.m_style));   // the track's own (22.09.2026)

    TrackPlan t;
    t.m_style = w.m_style;
    t.m_index = index;
    // The DJ blend (Form.h, djOverlapBars; 23.09.2026): every track after the first starts its blend's length
    // before the one before it ends, its kick-free intro over the other's outro. The length is the incoming
    // style's, never longer than the outgoing outro; the incoming intro is checked once the form exists.
    int overlap = 0;
    if (index > 0) {
        const TrackPlan& prev = m_plans[static_cast<size_t>(index - 1)];
        overlap = std::min(djOverlapBars(static_cast<StyleIdE>(w.m_style)), prev.m_form.m_count > 0 ? prev.m_form.m_section[prev.m_form.m_count - 1].m_bars : DjOverlap);
        t.m_firstBar = prev.m_firstBar + prev.m_bars - overlap;
    }
    t.m_key = w.m_key;
    t.m_scale = w.m_scale;
    t.m_bpm = w.m_bpm;
    std::copy(w.m_kickMacro, w.m_kickMacro + NumKickMacros, t.m_kickMacro);
    std::copy(w.m_bassMacro, w.m_bassMacro + NumBassMacros, t.m_bassMacro);
    std::copy(w.m_acidVoicing, w.m_acidVoicing + NumAcidVoicings, t.m_acidVoicing);
    for (int v = 0; v < PolyInstances; ++v) t.m_voice[v] = w.m_voice[v];
    std::copy(w.m_soundPreset, w.m_soundPreset + SoundSynths, t.m_soundPreset);
    // The tables this track's voices will read, expanded now, on whichever thread planned the track
    // (21.09.2026). The pack is only indexed at load; a table costs its 1.9 MB of mip levels when a
    // track first asks for it and not before, which is what lets the library grow past a handful.
    // The audio thread never builds: it reads what is published or falls back (WaveTableFile.h).
    {
        int want[PolyInstances];
        int n = 0;
        for (int v = 0; v < PolyInstances; ++v)
            if (t.m_voice[v].m_table >= 0) want[n++] = t.m_voice[v].m_table;
        if (n > 0) ensureWaveTables(want, n);
    }

    // Where the track sits on the set's energy arc (Form.h).
    const double setBars = setLengthBars(p);
    const ArcIdE arc = arcOf(p);
    t.m_arcIn = static_cast<float>(arcEnergy(arc, t.m_firstBar / setBars));
    t.m_arcOut = static_cast<float>(arcEnergy(arc, (t.m_firstBar + w.m_bars) / setBars));

    // The form: a track's own decision, so it hangs off the track's seed and is rerollable. The
    // section seeds are made first, because since 16.09.2026 the form draws each section's borrowed
    // mode from the section's own seed (Form.h) -- a section is a lockable unit and its mode has to
    // move with it. They do not depend on the form, only on the track, so nothing is circular.
    for (int s = 0; s < MaxSections; ++s) t.m_sectionSeed[s] = sectionSeedOf(index, s);
    t.m_formSeed = mixSeed(trackSeed(index) ^ SaltFormU, 0);
    // Modal interchange off: a style profile whose chance is zero, which makes every section keep
    // the track's mode. That is the A/B the self test uses to prove the bass does not move.
    StyleProfile interchange = style;
    if (!p.getBool(cb + compose::MODAL_INTERCHANGE)) interchange.m_interchangeChance = 0.0f;
    t.m_form = makeFormPlan(interchange, t.m_formSeed, w.m_bars, t.m_arcIn, t.m_arcOut, t.m_scale, t.m_sectionSeed);
    if (index > 0) {
        // The blend can be no longer than this track's intro (a short track shrinks its intro to sixteen,
        // Form.cpp). A shorter blend moves firstBar by at most sixteen bars against the arc position the
        // form was made with; the arc's energy hardly moves over sixteen bars of a set, so the form stands.
        TrackPlan& prev = m_plans[static_cast<size_t>(index - 1)];
        overlap = std::max(DjOverlap, std::min(overlap, t.m_form.m_section[0].m_bars));
        t.m_firstBar = prev.m_firstBar + prev.m_bars - overlap;
        prev.m_form.m_overlapTail = overlap;   // the outgoing outro builds its blend from it (Form.cpp, planBar)
    }
    t.m_form.m_handover = index == 0 ? 0 : overlap;
    makeFormSfx(t.m_form, t.m_formSeed, p.get(cb + compose::SFX_AMOUNT), p.get(cb + compose::VOICE_DENSITY), p.get(cb + compose::BED_DENSITY));
    placeField(t.m_form, t.m_formSeed, t.m_style, p.get(cb + compose::FIELD_DENSITY), p.get(cb + compose::SFX_AMOUNT));   // 27.09.2026
    t.m_bars = t.m_form.m_bars;

    uint64_t laneSeeds[PercLanes];
    for (int l = 0; l < PercLanes; ++l) laneSeeds[l] = laneSeedOf(index, l);
    t.m_percSeed = mixSeed(trackSeed(index) ^ SaltPerc, 0);
    t.m_perc = makePercPlan(p, t.m_percSeed, index == 0, laneSeeds);
    t.m_melodySeed = mixSeed(trackSeed(index) ^ SaltMelody, 0);
    // The colour -- how many notes of acid, lead and arp are the flat second or the augmented second --
    // follows the style profile and the track's place on the energy arc (Farbood's dissonance). Since
    // 18.09.2026 it scales one calibrated target share per role (Melody.cpp, ColourShare) and is the
    // only thing that decides the colour; it no longer weights the sampler's sets.
    const float colour = std::clamp(style.m_colour * (0.4f + 0.6f * 0.5f * (t.m_arcIn + t.m_arcOut)), 0.0f, 1.0f);
    // The form's scale mask says which modes need recoloured material; with interchange off it holds
    // the track's mode alone and makeMelodyPlan does exactly what it did before.
    // The bass pattern before the melody (22.09.2026, round "Lead"): the lead's rhythm interlocks with
    // the bass, so the pattern has to exist when the melody is drawn. Its draws are the first ones of
    // the track's generator, as they were; only their place in this function moved, so every draw
    // after them keeps its value.
    Rng r;
    r.seed(trackSeed(index));
    t.m_primaryPattern = std::clamp(p.getInt(cb + compose::BASS_PATTERN), 0, NumBassPatterns - 1);
    t.m_secondaryPattern = t.m_primaryPattern == 0 ? 1 : 0;
    if (index > 0) {
        if (r.uniform() < tv) {
            static const double Weights[NumBassPatterns] = { 0.45, 0.20, 0.12, 0.08, 0.15 };
            t.m_primaryPattern = pick(r, Weights, NumBassPatterns);
        }
        double wp[NumBassPatterns] = { 0.40, 0.30, 0.20, 0.05, 0.05 };
        wp[t.m_primaryPattern] = 0.0;
        t.m_secondaryPattern = pick(r, wp, NumBassPatterns);
    }
    // The bass onsets the lead interlocks with: the primary pattern's family figure. A drawn bass
    // rhythm (compose.bass_rhythm = Corpus) strays from it now and then; the family is what the
    // track's groove is built on, and the mask is decided before the rhythm exists.
    t.m_melody = makeMelodyPlan(p, style, t.m_melodySeed, t.m_key, t.m_scale, index == 0, colour, t.m_form.m_scaleMask,
                              BassRhythm::familyMask(t.m_primaryPattern), w.m_motif);

    if (index == 0) {
        t.m_gate = p.get(cb + compose::BASS_GATE);
        // The rhythm before the pitches, because the learned phrase is drawn for the steps the rhythm
        // decides. The first track takes the gate knob as it is -- "the knobs, exactly" -- and a drawn
        // rhythm cannot make that worse: its shortest slot is a quarter beat, which is what the
        // default Rolling family already asks of the knob.
        makeBassRhythm(p, t);
        makeBassPhrases(p, t);
        measureTrack(p, t);
        return t;   // the first track is the knobs, exactly
    }

    // The patterns were drawn above, before the melody. The rhythm, after the patterns it strays from
    // and before the gate limit it decides.
    makeBassRhythm(p, t);

    // Gate, within what lets the lowest note (the seventh below the root) finish its release. With a
    // drawn rhythm the limit is computed from the shortest span the two phrases really contain
    // instead of from the families' -- which is the exact same computation on a measured number, and
    // is never tighter than a quarter beat because the grid has no finer gap (Composer.h).
    const int root = bassRootNote(t.m_key, reg);
    const int lowest = root + scaleDegree(t.m_scale, 6) - 12;
    const float gateMax = t.m_bassRhythm
        ? gateLimitSlot(t.m_bassShortestSlot, t.m_bpm, lowest, releaseKnob)
        : std::min(gateLimit(t.m_primaryPattern, t.m_bpm, lowest, releaseKnob),
                   gateLimit(t.m_secondaryPattern, t.m_bpm, lowest, releaseKnob));
    t.m_gate = std::clamp(p.get(cb + compose::BASS_GATE) + (2.0f * r.uniform() - 1.0f) * 0.12f * tv, 0.35f, std::max(0.35f, gateMax));

    // The learned bass phrases, after the patterns and the rhythm they fill and before any probe
    // render, so that the level match hears the track the way it will be played.
    makeBassPhrases(p, t);

    // Kick character switches.
    t.m_kickEngine = -1;
    if (r.uniform() < 0.15f * sv) t.m_kickEngine = 1 - p.getInt(p.base(ModuleE::KICK) + kick::ENGINE);
    t.m_kickClip = r.uniform() < 0.3f * sv ? 1 : -1;

    measureTrack(p, t);
    return t;
}

const TrackPlan& Composer::track(const ParamStore& p, int index) const
{
    const Entry entry(*this, p);
    while (static_cast<int>(m_plans.size()) <= index) m_plans.push_back(makeTrack(p, static_cast<int>(m_plans.size())));
    return m_plans[static_cast<size_t>(index)];
}

int Composer::trackOfBar(const ParamStore& p, int bar) const
{
    const Entry entry(*this, p);
    int i = 0;
    for (;;) {
        const TrackPlan& t = track(p, i);
        if (bar < t.m_firstBar + t.m_bars) return i;
        ++i;
    }
}

int Composer::incomingOfBar(const ParamStore& p, int bar) const
{
    const Entry entry(*this, p);
    const int ti = trackOfBar(p, bar);
    const TrackPlan& t = track(p, ti);
    if (bar < t.m_firstBar + t.m_bars - DjOverlapMax) return -1;
    const TrackPlan& next = track(p, ti + 1);
    return bar >= next.m_firstBar ? ti + 1 : -1;
}

TempoMap Composer::tempoMap(const ParamStore& p, int bars) const
{
    // Held per track from its hand-over, ramped over the DJ overlap into the next -- the same sixteen bars
    // at the end of every track as before 19.09.2026, now the bars in which the next track's intro already
    // sounds over this one's bare outro (Form.h, DjOverlap).
    const Entry entry(*this, p);   // one set of plans for the whole map
    TempoMap m;
    const TrackPlan& first = track(p, 0);
    m.setConstant(first.m_bpm);
    for (int i = 0;; ++i) {
        // References: inside one call planning the next track moves none of the cached ones (Composer.h).
        const TrackPlan& t = track(p, i);
        const double start = static_cast<double>(handoverBar(t)) * BeatsPerBar;
        if (i > 0) m.add(start, t.m_bpm, false);
        if (t.m_firstBar + t.m_bars >= bars) break;
        const TrackPlan& next = track(p, i + 1);
        if (next.m_bpm != t.m_bpm) {
            const double rampStart = static_cast<double>(next.m_firstBar) * BeatsPerBar;
            m.add(std::max(start, rampStart), t.m_bpm, true);
        }
    }
    return m;
}

std::vector<SectionMark> Composer::sections(const ParamStore& p, int bars) const
{
    const Entry entry(*this, p);   // one set of plans for every mark
    std::vector<SectionMark> out;
    for (int i = 0;; ++i) {
        const TrackPlan& t = track(p, i);
        for (int s = 0; s < t.m_form.m_count; ++s) {
            const Section& sec = t.m_form.m_section[s];
            const int startBar = t.m_firstBar + sec.m_startBar;
            if (startBar >= bars) break;
            SectionMark m;
            m.m_beat = static_cast<double>(startBar) * BeatsPerBar;
            m.m_type = sec.m_type;
            m.m_energy = sec.m_energy;
            m.m_track = i;
            // The cut sits at the head of the breakdown it belongs to, the pre-drop break in the last
            // bar of its buildup; both are sub-bar categories of Grosz et al. and get their own mark.
            if (sec.m_type == SectionTypeE::BREAK && sec.m_cutBeats > 0.0f) {
                SectionMark c = m;
                c.m_type = SectionTypeE::CUT;
                out.push_back(c);
            }
            out.push_back(m);
            if (sec.m_type == SectionTypeE::BUILD) {
                SectionMark d = m;
                d.m_type = SectionTypeE::PDB;
                d.m_beat = static_cast<double>(startBar + sec.m_bars - 1) * BeatsPerBar;
                if (startBar + sec.m_bars - 1 < bars) out.push_back(d);
            }
        }
        if (t.m_firstBar + t.m_bars >= bars) break;
    }
    std::stable_sort(out.begin(), out.end(), [](const SectionMark& a, const SectionMark& b) { return a.m_beat < b.m_beat; });
    return out;
}

/**
 * @brief The incoming track's share of a bar of the DJ overlap (19.09.2026, round "arrangement").
 *
 * All the tracks come from one hand, so the transition is written rather than mixed, but it is written
 * the way a DJ mixes two records (Form.h, DjOverlap): the incoming track's intro starts sixteen bars
 * before the outgoing track ends, over its bare outro -- kick, bass and one hat -- with what the intro's
 * kick-free half plays: the quiet sixteenth hat, the shaker, the pads, textures and voices. Its kick and
 * bass wait for its own bar 17, which is the outgoing track's end, so exactly one kick and one bass sound
 * at every moment and the kick-bass lock never has two partners.
 *
 * **Keys.** The incoming pads sound over the outgoing bass. They come in only where the two keys are close
 * enough for that to be consonant -- the same key, a fourth or a fifth apart -- the harmonic-mixing rule of
 * the DJ literature (Ishizaki, Hoashi and Takishima 2009, which this generator already followed for the
 * pads that used to hold over from the previous track). Where they are not, the incoming track's first
 * sixteen bars are its percussion and its effects alone; its pads enter with its kick and bass. The
 * effects are tuned to the key the engine plays in, which is the outgoing track's until the hand-over.
 * A fourth apart is consonant for the tonics but not for every colour: the incoming key's minor sixth is
 * then a semitone over the outgoing tonic, a b9 held over the outgoing bass, and the user's rule
 * (25.09.2026) allows none in a pad or a drone -- such a note is left out until the hand-over.
 *
 * **Controls.** The incoming track's voice recipes and levels at its first bar, its voices' section
 * controls, and per bar the pad's and the drone's high pass and the drone's evolution -- for this track,
 * whose voices are the only ones sounding there (the outgoing outro's bare bars have none). Its floor
 * follows at the hand-over (composeBars).
 *
 * The percussion shares the kit's lanes: where both tracks put a hit on the same lane at the same moment,
 * the outgoing track's stands and the incoming one is dropped.
 */
void Composer::transitionBar(const ParamStore& p, int gi, int bar, std::vector<NoteEvent>& out,
                             std::vector<ControlEvent>* controls) const
{
    const TrackPlan& guest = m_plans[static_cast<size_t>(gi)];
    const TrackPlan& owner = m_plans[static_cast<size_t>(gi - 1)];
    const int inTrack = bar - guest.m_firstBar;
    const double barBeat = static_cast<double>(bar) * BeatsPerBar;
    const BarPlan bp = planBar(guest.m_form, availabilityOf(guest), guest.m_sectionSeed, inTrack);
    const int move = ((guest.m_key - owner.m_key) % 12 + 12) % 12;
    const bool consonant = move == 0 || move == 5 || move == 7;

    if (controls != nullptr) {
        if (inTrack == 0) trackStartControls(p, guest, barBeat, *controls, ControlScopeE::VOICES);
        if (bp.m_barInSection == 0) sectionControls(p, guest, bp, barBeat, *controls, ControlScopeE::VOICES);
        leadArcControls(p, guest, bp, inTrack, barBeat, *controls);
        // The fader ride into the swap (23.09.2026, round "DJ"): eight bars before the hand-over the track
        // gain and the master offset start ramping from the outgoing track's values to the ones the incoming
        // floor will write at the hand-over -- the same numbers trackStartControls and sectionControls push
        // there, so the swap itself moves nothing. Without the ramp both jump at the hand-over, which is
        // heard as "ploetzlich wieder lauter".
        if (guest.m_form.m_handover >= 8 && inTrack == guest.m_form.m_handover - 8) {
            const int mb = p.base(ModuleE::MIX);
            const BarPlan at = planBar(guest.m_form, availabilityOf(guest), guest.m_sectionSeed, guest.m_form.m_handover);
            const Section& sec = guest.m_form.m_section[std::clamp(at.m_index, 0, MaxSections - 1)];
            const double u = sec.m_bars > 1 ? static_cast<double>(at.m_barInSection) / sec.m_bars : 0.0;
            const ParamDesc& g = p.desc(mb + mix::TRACK_GAIN);
            ControlEvent c;
            c.m_beat = barBeat;
            c.m_kind = ControlEvent::KindE::OFFSET;
            c.m_length = 8.0f * static_cast<float>(BeatsPerBar);
            c.m_param = static_cast<int16_t>(mb + mix::TRACK_GAIN);
            c.m_value = (guest.m_gainDb + energyGainDb(at.m_energy) + sectionTrimDb(guest.m_form, at.m_index, u)) / (g.m_maxValue - g.m_minValue);
            controls->push_back(c);
            if (!guest.m_masterDeferred) {
                const ParamDesc& mg = p.desc(p.base(ModuleE::MASTER) + master::GAIN);
                c.m_param = static_cast<int16_t>(p.base(ModuleE::MASTER) + master::GAIN);
                c.m_value = guest.m_masterGainDb / (mg.m_maxValue - mg.m_minValue);
                controls->push_back(c);
            }
        }
        const int pb = p.base(PolyInstanceE::PAD), db = p.base(PolyInstanceE::DRONE);
        auto closeHighPass = [&](int base) {
            // The floor is the outgoing track's here: the incoming pad and drone keep their knobs' high pass.
            ControlEvent h;
            h.m_beat = barBeat;
            h.m_kind = ControlEvent::KindE::OFFSET;
            h.m_value = 0.0f;
            h.m_param = static_cast<int16_t>(base + poly::HP_FLOOR);
            controls->push_back(h);
            h.m_param = static_cast<int16_t>(base + poly::HP_TRACK);
            controls->push_back(h);
        };
        if (guest.m_melody.m_present[mpIndex(MelodyPartE::PAD)]) closeHighPass(pb);
        if (guest.m_melody.m_present[mpIndex(MelodyPartE::DRONE)]) {
            closeHighPass(db);
            droneControls(p, guest, inTrack, barBeat, *controls);
        }
    }

    PercBarSpec spec;
    spec.m_layers = bp.m_percLayers;
    spec.m_fills = false;
    spec.m_quietHats = bp.m_quietHats;
    spec.m_shaker = bp.m_shaker;
    spec.m_offbeatHat = bp.m_offbeatHat;
    spec.m_hatLevel = bp.m_hatLevel;
    spec.m_cycleBar = bp.m_cycleBar;
    composePercBar(p, guest.m_perc, guest.m_percSeed, bar, inTrack, owner.m_bpm, guest.m_key, guest.m_scale, spec, out);
    if (consonant) {
        const size_t from = out.size();
        composeMelodyBar(p, guest.m_melody, bar, inTrack, guest.m_scale, bp, out, melodyContext(guest, bp, inTrack));
        const int flat9 = (owner.m_key + 1) % 12;
        out.erase(std::remove_if(out.begin() + static_cast<std::ptrdiff_t>(from), out.end(), [&](const NoteEvent& e) {
                      return (e.m_part == PartE::PAD || e.m_part == PartE::DRONE) && e.m_pitch % 12 == flat9;
                  }), out.end());
    }
    composeSfxBar(guest.m_form, static_cast<double>(guest.m_firstBar) * BeatsPerBar, inTrack, out);
}

/** @brief Whether the bass follows the chords in bar @p inTrack: where the track's own pad sounds (25.09.2026). */
static bool followsHere(const TrackPlan& plan, int inTrack)
{
    if (inTrack < 0 || inTrack >= plan.m_bars) return false;
    const BarPlan bp = planBar(plan.m_form, availabilityOf(plan), plan.m_sectionSeed, inTrack);
    return (bp.m_parts & partBit(MelodyPartE::PAD)) != 0 && plan.m_melody.m_present[mpIndex(MelodyPartE::PAD)];
}

/**
 * @brief With Bass Follows Chords on, the drone goes where the bass goes (25.09.2026).
 *
 * A drone left on the tonic while the bass moves to another root stands a step or a semitone against it -- the
 * literature the user brought: where the bass follows, "geht typischerweise alles mit". Each drone note that
 * composeMelodyBar placed from @p from on is cut at the chord blocks it spans (and where the bass stops or starts
 * following, followsHere), and every piece moves by the bass's shift there (bassChordShift), to the nearer octave
 * so the drone keeps its register. Pieces that would repeat the pitch before them are joined back.
 * @param plan     the track
 * @param baseRoot the bass's root note on the tonic, MIDI
 * @param out      the bar's notes; the drone's are replaced by their pieces
 * @param from     the first note composeMelodyBar added
 */
static void followWithDrone(const TrackPlan& plan, int baseRoot, std::vector<NoteEvent>& out, size_t from)
{
    std::vector<NoteEvent> pieces;
    for (size_t k = from; k < out.size(); ++k) {
        if (out[k].m_part != PartE::DRONE) continue;
        const NoteEvent whole = out[k];
        const double end = whole.m_beat + whole.m_length;
        double at = whole.m_beat;
        while (at < end - 1e-9) {
            const int bar = static_cast<int>(std::floor(at / BeatsPerBar + 1e-9));
            const int inTrack = bar - plan.m_firstBar;
            const PadChord pcd = padChordAt(plan.m_melody, plan.m_form, inTrack);
            // A piece per bar where the following switches on or off with the pad, else per chord block.
            const int blockEnd = bar - pcd.m_barInBlock + std::max(1, pcd.m_blockBars);
            int pieceEnd = bar + 1;
            const bool on = followsHere(plan, inTrack);
            while (pieceEnd < blockEnd && followsHere(plan, pieceEnd - plan.m_firstBar) == on) ++pieceEnd;
            const double cut = std::min(end, static_cast<double>(pieceEnd) * BeatsPerBar);
            int shift = on ? bassChordShift(pcd, plan.m_scale, baseRoot) : 0;
            shift = ((shift % 12) + 12) % 12;
            if (shift > 6) shift -= 12;
            NoteEvent piece = whole;
            piece.m_beat = at;
            piece.m_length = static_cast<float>(cut - at);
            piece.m_pitch = static_cast<uint8_t>(std::clamp(static_cast<int>(whole.m_pitch) + shift, 0, 127));
            NoteEvent* before = pieces.empty() ? nullptr : &pieces.back();
            if (before != nullptr && before->m_pitch == piece.m_pitch && std::fabs(before->m_beat + before->m_length - at) < 1e-9)
                before->m_length += piece.m_length;
            else
                pieces.push_back(piece);
            at = cut;
        }
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(k));
        --k;
    }
    out.insert(out.end(), pieces.begin(), pieces.end());
}

void Composer::composeBars(const ParamStore& p, int firstBar, int count, std::vector<NoteEvent>& out,
                           std::vector<ControlEvent>* controls) const
{
    // The knobs are checked here and nowhere else in this call (Composer.h, "Knobs that move while the composer
    // works"): the track index, the incoming track and the plans below all come from one set of plans.
    const Entry entry(*this, p);
    const int cb = p.base(ModuleE::COMPOSE);
    const int kickPattern = p.getInt(cb + compose::KICK_PATTERN);
    const float bassVar = p.get(cb + compose::BASS_VARIATION);
    const int reg = p.getInt(cb + compose::BASS_REGISTER);

    const size_t noteStart = out.size();
    const size_t ctlStart = controls ? controls->size() : 0;
    for (int bar = firstBar; bar < firstBar + count; ++bar) {
        const int ti = trackOfBar(p, bar);
        // The overlap window needs the next track's plan.
        const TrackPlan& plan = track(p, ti);
        if (bar - plan.m_firstBar >= plan.m_bars - DjOverlapMax) track(p, ti + 1);
        // The incoming track of the DJ overlap, if one sounds in this bar (Form.h, DjOverlap).
        int incoming = ti + 1 < static_cast<int>(m_plans.size()) && bar >= m_plans[static_cast<size_t>(ti + 1)].m_firstBar ? ti + 1 : -1;
        const size_t barNotes = out.size();
        // One track alone (setSoloTrack, 23.09.2026): its own bars without a guest, its intro as the guest alone,
        // and nothing of any other track.
        if (m_soloTrack >= 0) {
            if (ti == m_soloTrack) {
                incoming = -1;
            } else if (incoming == m_soloTrack) {
                const TrackPlan& solo = m_plans[static_cast<size_t>(incoming)];
                // Its floor settings at its first bar: in the set the outgoing track holds the floor until the
                // hand-over, alone there is nobody, and the engine would play the intro on the previous state.
                if (controls != nullptr && bar == solo.m_firstBar)
                    trackStartControls(p, solo, static_cast<double>(bar) * BeatsPerBar, *controls, ControlScopeE::FLOOR);
                std::vector<NoteEvent> guest;
                transitionBar(p, incoming, bar, guest, controls);
                out.insert(out.end(), guest.begin(), guest.end());
                continue;
            } else {
                continue;
            }
        }
        const int inTrack = bar - plan.m_firstBar;
        const float progress = static_cast<float>(inTrack) / static_cast<float>(plan.m_bars);
        const double barBeat = static_cast<double>(bar) * BeatsPerBar;
        const int baseRoot = bassRootNote(plan.m_key, reg);
        const bool follow = p.getBool(cb + compose::BASS_FOLLOWS_CHORDS);
        // Following, the bass takes the root the pad holds (bassChordShift), and its figures count from that
        // chord's degree -- the tonic's where the pad holds the tonic for a chord it may not (the bII). Only
        // where the pad sounds: in a bar without it there is no chord to follow, and the bass is the pedal
        // (so, too, under a DJ overlap, whose outgoing outro is bare).
        int shift = 0, chordDeg = 0;
        if (follow && followsHere(plan, inTrack)) {
            const PadChord pcd = padChordAt(plan.m_melody, plan.m_form, inTrack);
            shift = bassChordShift(pcd, plan.m_scale, baseRoot);
            chordDeg = shift == 0 ? 0 : pcd.m_degree;
        }
        const int root = baseRoot + shift;

        // Pattern of this bar: the secondary one for the last four bars of some 16-bar blocks.
        Rng block;
        block.seed(mixSeed(m_seed ^ SaltBlock, (static_cast<uint64_t>(ti) << 32) | static_cast<uint64_t>(inTrack / 16)));
        const bool secondaryBlock = inTrack >= 16 && block.uniform() < 0.35f * bassVar;
        const bool bassBreak = block.uniform() < 0.3f * bassVar;
        const bool secondary = secondaryBlock && inTrack % 16 >= 12;
        const int pattern = secondary ? plan.m_secondaryPattern : plan.m_primaryPattern;
        const BassPatternDef& pat = BassPatterns[pattern];
        // The bar's onset mask when the rhythm is drawn (Composer.h). The masks live on the learned
        // phrase's eight-bar cycle, the same one the pitches use.
        const unsigned mask = plan.m_bassRhythm ? plan.m_bassMask[secondary ? 1 : 0][inTrack % BassPhraseBars] : 0u;

        // What the form says plays in this bar (Form.h).
        BarPlan bp = planBar(plan.m_form, availabilityOf(plan), plan.m_sectionSeed, inTrack);

        if (controls != nullptr) {
            // A track's floor takes over at its hand-over: its first bar in the set's first track, its own
            // bar 17 -- the outgoing track's end -- in every later one, whose voices started at its first bar
            // (transitionBar). Sections that start later write everything at their first bar as before.
            const int handover = plan.m_form.m_handover;
            if (inTrack == handover) {
                trackStartControls(p, plan, barBeat, *controls, handover == 0 ? ControlScopeE::ALL : ControlScopeE::FLOOR);
                arcControls(p, plan, inTrack, barBeat, false, *controls);
                if (handover > 0 && bp.m_barInSection > 0) sectionControls(p, plan, bp, barBeat, *controls, ControlScopeE::FLOOR);
            } else if (inTrack % 32 == 0 && inTrack > handover) {
                arcControls(p, plan, inTrack, barBeat, true, *controls);
            }
            if (bp.m_barInSection == 0 && inTrack >= handover) sectionControls(p, plan, bp, barBeat, *controls);
            if (inTrack >= handover) leadArcControls(p, plan, bp, inTrack, barBeat, *controls);
            // The throw (25.09.2026, the mix guide): on the last beat before every eight-bar change of a groove or a
            // drop the effects go into the plate (sfx.plate_send +0.6; the hall, the far room, is muted there), back on
            // the next downbeat -- the zap that "splashes" to the back. Both events lie in their own bar.
            if (inTrack >= handover && (bp.m_type == SectionTypeE::GROOVE || bp.m_type == SectionTypeE::DROP) && !(plan.m_index == 0 && bp.m_index == 0)) {
                const int sb = p.base(ModuleE::SFX);
                if (inTrack % 8 == 7 || inTrack % 8 == 0) {
                    ControlEvent t;
                    t.m_beat = inTrack % 8 == 7 ? barBeat + 3.0 : barBeat;
                    t.m_param = static_cast<int16_t>(sb + sfx::PLATE_SEND);
                    t.m_kind = ControlEvent::KindE::OFFSET;
                    t.m_value = inTrack % 8 == 7 ? 0.6f : 0.0f;
                    t.m_length = 0.0f;
                    controls->push_back(t);
                }
            }
            // The pad's high pass for the sub foundation (rule 20): opened to FoundationHpFloor and
            // an octave under each voice wherever the form silences kick and bass, the knobs
            // everywhere else. Every bar carries its state, so a bar composed alone carries it too;
            // a pad voice reads its high pass once, at its note-on (Poly.cpp), so the switch never
            // moves a filter under a sounding note and cannot click.
            // The same for the drone's low octave (19.09.2026): open where it lies on the silent floor
            // (droneLowAt), closed -- its knobs, 140 Hz -- everywhere else.
            auto highPass = [&](PolyInstanceE inst, bool open) {
                const int pb = p.base(inst);
                ControlEvent h;
                h.m_beat = barBeat;
                h.m_kind = ControlEvent::KindE::OFFSET;
                h.m_param = static_cast<int16_t>(pb + poly::HP_FLOOR);
                h.m_value = open ? p.toNormalised(pb + poly::HP_FLOOR, FoundationHpFloor) - p.toNormalised(pb + poly::HP_FLOOR, p.get(pb + poly::HP_FLOOR)) : 0.0f;
                controls->push_back(h);
                h.m_param = static_cast<int16_t>(pb + poly::HP_TRACK);
                h.m_value = open ? p.toNormalised(pb + poly::HP_TRACK, FoundationHpTrack) - p.toNormalised(pb + poly::HP_TRACK, p.get(pb + poly::HP_TRACK)) : 0.0f;
                controls->push_back(h);
            };
            // Over the DJ overlap the incoming track's pad and drone are the ones sounding, and it writes
            // their controls (transitionBar); the outgoing outro's bare bars have neither.
            if (incoming < 0 && plan.m_melody.m_present[mpIndex(MelodyPartE::PAD)]) highPass(PolyInstanceE::PAD, foundationBar(bp));
            if (incoming < 0 && plan.m_melody.m_present[mpIndex(MelodyPartE::DRONE)]) {
                highPass(PolyInstanceE::DRONE, droneLowAt(plan, inTrack));
                droneControls(p, plan, inTrack, barBeat, *controls);
            }
            ControlEvent c;
            c.m_beat = barBeat;
            c.m_param = static_cast<int16_t>(cb + compose::BASS_PATTERN);
            c.m_kind = ControlEvent::KindE::OVERRIDE;
            c.m_value = static_cast<float>(pattern);
            controls->push_back(c);
            if (!plan.m_bassRhythm) {
                // One clearing event a bar while the rhythm comes from a family. Without it a switch
                // of compose.bass_rhythm from Corpus back to Pattern *during* playback would leave
                // `Engine::m_slotBeats` holding the last drawn slot for ever: the composer would stop
                // sending, and the engine has no way of telling "nothing sent yet" from "nothing sent
                // any more". A value <= 0 hands the engine back its own derivation (Score.h), so a
                // Pattern render sounds exactly as it did before 16.09.2026 -- the extra event only
                // recomputes, at a beat that already carries the Override above, the same values
                // `applyParams` has just written.
                c.m_kind = ControlEvent::KindE::BASS_SLOT;
                c.m_value = -1.0f;
                controls->push_back(c);
            }
        }

        // Phrase figures, likelier as the track goes on.
        Rng phrase;
        phrase.seed(mixSeed(m_seed ^ SaltPhrase, (static_cast<uint64_t>(ti) << 32) | static_cast<uint64_t>(inTrack / 4)));
        const float figureChance = bassVar * (0.6f + 0.4f * progress);
        const bool varyBar2 = phrase.uniform() < figureChance * 0.6f;
        const bool varyBar4 = phrase.uniform() < figureChance;
        static const int FigureDegrees[7][3] = { { 0, 0, 0 }, { 0, 0, 4 }, { 0, 4, 0 }, { 0, 1, 1 }, { 6, 0, 0 }, { 0, 2, 0 }, { 4, 0, 0 } };
        static const int FigureOctaves[7][3] = { { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 }, { -1, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 } };
        const int figure2 = phrase.below(7), figure4 = phrase.below(7);
        const int barInPhrase = inTrack % 4;
        // The learned bass has a phrase of its own length (BassPhraseBars, eight bars) and reads the
        // bar of the track with it; the figures above keep their four-bar phrase, so switching the
        // knob cannot move a single note the pattern generator places.
        const int barInBassPhrase = inTrack % BassPhraseBars;

        const StyleProfile& style = styleProfile(static_cast<StyleIdE>(plan.m_style));
        for (int beat = 0; beat < BeatsPerBar; ++beat) {
            const double b = barBeat + beat;
            const bool fillGap = kickPattern == 1 && inTrack % 8 == 7 && beat == 3;
            // The form decides per beat whether kick and bass play: a breakdown removes both (Solberg
            // and Dibben 2019), a pre-drop break removes them for part or all of its bar.
            if (kickPattern != 2 && !fillGap && ((bp.m_kickBeats >> beat) & 1) != 0 && static_cast<double>(beat) >= bp.m_cutBeats) {
                // The kick's roll (Form.h, BarPlan::kickRoll; 23.09.2026): in the bar the form marks, the kick
                // doubles to eighths, and in the roll's second half to sixteenths where the form asks for it,
                // the repeats a little softer than the beat so the pulse still reads.
                const int sub = bp.m_kickRoll >= 2 && beat >= 2 ? 4 : (bp.m_kickRoll >= 1 ? 2 : 1);
                for (int hit = 0; hit < sub; ++hit) {
                    NoteEvent k;
                    k.m_beat = b + static_cast<double>(hit) / sub;
                    k.m_length = 0.25f;
                    k.m_part = PartE::KICK;
                    k.m_pitch = 36;
                    k.m_velocity = static_cast<uint8_t>(hit == 0 ? 127 : (sub == 4 ? 104 : 112));
                    out.push_back(k);
                }
            }
            const bool bassBeat = ((bp.m_bassBeats >> beat) & 1) != 0 && static_cast<double>(beat) >= bp.m_cutBeats
                               && !(bassBreak && inTrack % 16 == 15 && beat >= 2);
            // How many notes this beat carries and where the first one is. With a pattern family the
            // answer is the family's; with a drawn rhythm it is the mask's, and it is a different
            // answer on every beat -- which is the whole reason the first slot has to travel to the
            // engine as an event of its own (Score.h, ControlEvent::Kind::BassSlot).
            int onsets = pat.m_count;
            int step[3] = { 0, 0, 0 };
            if (plan.m_bassRhythm) {
                onsets = 0;
                for (int k = 1; k < 4; ++k) if ((mask >> (beat * 4 + k)) & 1u) step[onsets++] = k;
            }
            if (plan.m_bassRhythm && controls != nullptr) {
                // One event per beat, at the beat, whether or not the beat plays. Unconditional,
                // because a bar composed on its own has to carry everything the engine needs for it:
                // an event pushed only on a change would leave a window that starts mid-track with the
                // slot of whatever was rendered before it. A beat with no bass note reports a quarter
                // beat -- the tightest slot the sixteenth grid can produce -- so that a resting bass
                // never lets the kick ring longer than it does in the beats around it, and so that the
                // limit is never looser than the Rolling family's.
                ControlEvent c;
                c.m_beat = b;
                c.m_kind = ControlEvent::KindE::BASS_SLOT;
                c.m_value = bassBeat && onsets > 0 ? 0.25f * static_cast<float>(step[0]) : 0.25f;
                controls->push_back(c);
            }
            if (!bassBeat) continue;

            int figure = -1;
            // With the learned bass the phrase figures are gone: they are the pattern generator's one
            // source of pitch variation, and the model is there to replace exactly that. The *group*
            // figure stays in both modes, because it is not a pitch idea but a form rule -- it is what
            // makes two consecutive eight-bar groups of a core differ (Form.cpp, GroupFigures), and
            // the learned phrase repeats every BassPhraseBars bars and could not do that by itself.
            if (!plan.m_bassNeural && beat == 3 && barInPhrase == 1 && varyBar2) figure = figure2;
            if (!plan.m_bassNeural && beat == 3 && barInPhrase == 3 && varyBar4) figure = figure4;
            // The group's own change: every eight-bar group of a core ends on a figure that differs
            // from the previous group's, so two consecutive groups can never be the same bars.
            if (beat == 1 && bp.m_groupFigure >= 0) figure = bp.m_groupFigure;
            for (int s = 0; s < onsets; ++s) {
                // Where the note starts and what ends it. The pattern families read their own table;
                // a drawn bar reads its mask, and its note ends at the next onset or at the next kick,
                // whichever comes first (Corpus.h, noteSpan) -- the same rule the families follow,
                // where the last note of a beat ends at 1.0.
                const double pos = plan.m_bassRhythm ? 0.25 * step[s] : pat.m_pos[s];
                const double next = plan.m_bassRhythm
                    ? 0.25 * (step[s] + BassRhythm::noteSpan(mask, beat * 4 + step[s]))
                    : (s + 1 < pat.m_count ? pat.m_pos[s + 1] : 1.0);
                int pitch = root;
                // The learned phrase (Composer.h): the interval of this slot from the bass root. It is
                // read against `baseRoot` and not against `root`, because the alphabet of the model is
                // intervals to the *tonic* -- adding a chord degree on top would carry a learned
                // interval out of the scale. compose.bass_follows_chords therefore has no effect on a
                // learned bass, which the self test checks and docs/PLAN.md 6.9 records as a limit.
                if (plan.m_bassNeural)
                    pitch = baseRoot + plan.m_bassRel[secondary ? 1 : 0]
                                             [(barInBassPhrase * BeatsPerBar + beat) * 3 + s];
                if (figure >= 0) {
                    const int idx = 3 - onsets + s;
                    // Figure degrees count from the chord's degree, so they stay in the scale when the bass follows the chords.
                    pitch = root + scaleDegree(plan.m_scale, chordDeg + FigureDegrees[figure][idx]) - scaleDegree(plan.m_scale, chordDeg)
                          + 12 * FigureOctaves[figure][idx];
                }
                // A b2 or a leading tone is a pickup (25.09.2026): at most two sixteenths, in the bar's last two,
                // into the next downbeat -- the variation at a phrase end the literature knows. Anywhere else,
                // under the drone's tonic, it is a minor ninth in the low register, and the figure takes the
                // fifth instead (the fifth below for the leading tone under the root).
                {
                    const int iv = ((pitch - root) % 12 + 12) % 12;
                    const bool pickup = beat == BeatsPerBar - 1 && pos >= 0.5 - 1e-9
                                        && (next - pos) * plan.m_gate * style.m_slotGate[std::min(s, BassSlots - 1)] <= 0.5 + 1e-9;
                    if (rubsTonic(iv) && !pickup) pitch += iv == 1 ? 6 : -4;
                }
                // The bass slot envelope of the style profile (PLAN 6.6): flat by default, because nine
                // reference tracks play three equally loud notes within +-1.2 dB.
                const int slot = std::min(s, BassSlots - 1);
                NoteEvent n;
                n.m_beat = b + pos;
                n.m_length = static_cast<float>((next - pos) * plan.m_gate * style.m_slotGate[slot]);
                n.m_part = PartE::BASS;
                n.m_pitch = static_cast<uint8_t>(std::clamp(pitch, 0, 127));
                n.m_velocity = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(110.0f * style.m_slotVel[slot])), 1, 127));
                out.push_back(n);
            }
        }
        PercBarSpec spec;
        spec.m_layers = bp.m_percLayers;
        spec.m_fills = bp.m_fills;
        spec.m_hatsDense = bp.m_hatsDense;
        spec.m_rollBar = bp.m_rollBar;
        spec.m_rollBars = bp.m_rollBars;
        spec.m_pdb = bp.m_pdb;
        spec.m_cutBeats = bp.m_cutBeats;
        spec.m_crash = bp.m_crash || (bp.m_barInSection == 0 && bp.m_type == SectionTypeE::DROP);
        spec.m_quietHats = bp.m_quietHats;
        spec.m_shaker = bp.m_shaker;
        spec.m_offbeatHat = bp.m_offbeatHat;
        spec.m_hatLevel = bp.m_hatLevel;
        spec.m_openHats = bp.m_openHats;
        spec.m_ride = bp.m_ride;
        spec.m_cycleBar = bp.m_cycleBar;
        composePercBar(p, plan.m_perc, plan.m_percSeed, bar, inTrack, plan.m_bpm, plan.m_key, plan.m_scale, spec, out);
        const size_t melodyFrom = out.size();
        composeMelodyBar(p, plan.m_melody, bar, inTrack, plan.m_scale, bp, out, melodyContext(plan, bp, inTrack));
        if (follow) followWithDrone(plan, baseRoot, out, melodyFrom);
        composeSfxBar(plan.m_form, static_cast<double>(plan.m_firstBar) * BeatsPerBar, inTrack, out);
        if (incoming >= 0) {
            // The percussion shares the kit's lanes: where both tracks hit one lane at the same moment the
            // outgoing track's hit stands (transitionBar).
            std::vector<NoteEvent> guest;
            transitionBar(p, incoming, bar, guest, controls);
            for (const NoteEvent& n : guest) {
                bool taken = false;
                for (size_t k = barNotes; k < out.size() && !taken; ++k)
                    taken = out[k].m_part == PartE::PERC && n.m_part == PartE::PERC && out[k].m_lane == n.m_lane && std::fabs(out[k].m_beat - n.m_beat) < 1e-6;
                if (!taken) out.push_back(n);
            }
        }
    }
    std::stable_sort(out.begin() + static_cast<long>(noteStart), out.end(), noteLess);
    std::inplace_merge(out.begin(), out.begin() + static_cast<long>(noteStart), out.end(), noteLess);
    if (controls != nullptr) {
        // Sorting only this call's new slice is not enough: `controls` accumulates across repeated
        // composeBars calls (Conductor::pump chunks a render into several), and the caller consumes it
        // as one beat-ordered stream. 18.09.2026: sectionAutomation (Form.h) can append a ride keyframe
        // whose beat lies many bars ahead, inside the FIRST chunk that enters its section -- sorted
        // correctly within that chunk's own slice, but still ahead in the vector of the near-term
        // events a LATER chunk appends for the bars in between. Conductor::pump and the engine's control
        // queue both assume the stream they are handed is globally non-decreasing in beat, so that one
        // future-dated event stalls everything queued behind it until its own beat finally arrives --
        // measured as the kick losing its lock to a freely drawn bass by tens of degrees (testBassRhythm,
        // "the kick phase still meets the bass..."). The prefix before ctlStart is the invariant this
        // function maintains call to call; merging restores it in the one case that can break it,
        // without re-touching the notes this file already sorts and hands over per call the same way.
        auto beatLess = [](const ControlEvent& a, const ControlEvent& b) { return a.m_beat < b.m_beat; };
        std::stable_sort(controls->begin() + static_cast<long>(ctlStart), controls->end(), beatLess);
        std::inplace_merge(controls->begin(), controls->begin() + static_cast<long>(ctlStart), controls->end(), beatLess);
    }
}

Conductor::Conductor(Engine& engine, const Composer& composer) : m_engine(engine), m_composer(composer) {}

void Conductor::rewind()
{
    m_nextBar = 0;
    m_notes.clear();
    m_controls.clear();
    m_notePos = m_controlPos = 0;
}

void Conductor::pump(const ParamStore& params, double horizonBeats, std::vector<NoteEvent>* record)
{
    const double target = m_engine.beatPosition() + horizonBeats;
    for (;;) {
        // sectionAutomation (Form.h, 18.09.2026) can date a control event many bars into its section's
        // future, all pushed in the one bar that opens the section -- composeBars keeps everything it
        // has produced so far sorted (see the merge there), but a bar this Conductor has not composed
        // yet can still turn out to owe an event of its own an EARLIER beat than one already sitting in
        // the buffer from a section-opening bar behind it. The one point before which nothing still to
        // come can ever land is the start of that next, uncomposed bar, so only events strictly before
        // it may be handed to the engine: m_engine.pushControl feeds a queue the engine drains assuming a
        // non-decreasing stream, and one early arrival stalls everything queued behind it until its own
        // beat finally comes around -- measured as the kick losing its lock to a freely drawn bass by
        // tens of degrees (testBassRhythm, "the kick phase still meets the bass...").
        const double safeBeat = static_cast<double>(m_nextBar) * BeatsPerBar;
        while (m_controlPos < m_controls.size() && m_controls[m_controlPos].m_beat < safeBeat) {
            if (!m_engine.pushControl(m_controls[m_controlPos])) return;
            ++m_controlPos;
        }
        while (m_notePos < m_notes.size() && m_notes[m_notePos].m_beat < safeBeat) {
            if (!m_engine.pushEvent(m_notes[m_notePos])) return;
            if (record != nullptr) record->push_back(m_notes[m_notePos]);
            ++m_notePos;
        }
        // Reclaim the drained prefix so a long render does not grow these buffers without bound; the
        // threshold is well above a bar's usual handful of events, so this runs rarely.
        if (m_controlPos > 256) { m_controls.erase(m_controls.begin(), m_controls.begin() + static_cast<long>(m_controlPos)); m_controlPos = 0; }
        if (m_notePos > 256) { m_notes.erase(m_notes.begin(), m_notes.begin() + static_cast<long>(m_notePos)); m_notePos = 0; }
        if (safeBeat >= target) return;
        m_composer.composeBars(params, m_nextBar, 1, m_notes, &m_controls);
        ++m_nextBar;
    }
}

} // namespace phos
