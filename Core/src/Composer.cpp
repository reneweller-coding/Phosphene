/**
 * @file Composer.cpp
 * @brief Track plans and their walk, bass phrases, curation, the bars of a set, and the conductor
 *        (the control events are in ComposerControls.cpp, the measurements in ComposerLevels.cpp).
 */
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

const char* const kKickMacroNames[kNumKickMacros] = { "length", "punch", "body", "grit", "click" };
const char* const kBassMacroNames[kNumBassMacros] = { "brightness", "pluck", "squelch", "grit", "weight" };
const char* const kLockUnitNames[kNumLockUnits] = { "set", "track", "section", "lane" };
const char* const kAcidVoicingNames[kNumAcidVoicings] = { "clean", "driven", "liquid" };
const char* const kVoiceMacroNames[kNumVoiceMacros] = { "brightness", "softness", "thickness", "space", "motion" };

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
constexpr double kArcTempoPerUnit = 6.0;

/**
 * @brief The delay times a voice's role may draw (20.09.2026, round "dialogue").
 *
 * Indices into kDelayBeats (Params.h): 0 = 1/16 (0.25 beats), 1 = 1/8, 2 = a dotted eighth (0.75, "3/16"),
 * 3 = a quarter, 4 = a dotted quarter, 5 = a half. The user's rule gives each of the two leads an echo of
 * its own -- the lead a dotted eighth, the counter a fast sixteenth ping-pong -- and a per-track recipe
 * may vary only inside that family, so the two never swap characters. A ping-pong is two *different*
 * times on the two sides (TempoDelay); the counter's pair is therefore 1/16 against 1/8 or 1/16 against
 * a dotted eighth, both of which put its repeats between the lead's. Pad and drone send nothing into
 * their delay (delay_send 0), so their family exists only to keep the table complete.
 */
struct VoiceDelayFamily { int left[3]; int leftCount; int right[3]; int rightCount; };
const VoiceDelayFamily kVoiceDelay[kPolyInstances] = {
    /* lead    */ { { 2, 2, 2 }, 1, { 2, 4, 4 }, 2 },   // dotted eighth against itself or a dotted quarter
    /* counter */ { { 0, 0, 0 }, 1, { 1, 2, 2 }, 2 },   // 1/16 against 1/8 or a dotted eighth: the ping-pong
    /* arp     */ { { 2, 1, 1 }, 2, { 1, 3, 3 }, 2 },   // as before: the arp's own short echo
    /* stab    */ { { 2, 3, 3 }, 2, { 3, 1, 1 }, 2 },
    /* pad     */ { { 3, 4, 4 }, 2, { 4, 5, 5 }, 2 },
    /* drone   */ { { 4, 5, 5 }, 2, { 5, 4, 4 }, 2 },
};
static_assert(sizeof(kVoiceDelay) / sizeof(kVoiceDelay[0]) == kPolyInstances, "one delay family per polyphonic instance");

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
    const int cb = p.base(Module::Compose);
    const double minutes = std::max(1.0, static_cast<double>(p.getInt(cb + compose::SetMinutes)));
    const double bpm = p.getBool(cb + compose::StyleTempo) ? styleProfile(styleOf(p)).bpmCentre
                                                           : std::max(20.0, static_cast<double>(p.get(cb + compose::Bpm)));
    return std::max(32.0, minutes * bpm / kBeatsPerBar);
}

/** @brief The uniform source sampleMasked() draws from. */
struct BassUniform {
    Rng* r;
    double operator()() const { return static_cast<double>(r->uniform()); }
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
    return beat * 4 + static_cast<int>(std::lround(pat.pos[s] * 4.0));
}

/**
 * @brief Draws one bass phrase (kBassPhraseBars bars) for a pattern family from the learned role.
 *
 * The line is the whole phrase -- `kBassPhraseBars` bars of `pat.count` notes a beat -- drawn in one
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
 * @param out   kBassPhraseSlots entries; slots the phrase does not use are left at 0
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
    allowed.assign(n, std::vector<uint8_t>(static_cast<size_t>(kCorpusAlphabet), 0));
    cond.resize(n);
    for (size_t i = 0; i < n; ++i) {
        for (int rel = lowRel; rel <= highRel; ++rel)
            if (inScale(scale, rel)) allowed[i][static_cast<size_t>(PitchModel::symbol(rel))] = 1;
        const int step = steps[i];
        cond[i].step = step % 16;
        cond[i].bar = (step / 16) % 8;
        cond[i].gap = noteGapCode(step, i + 1 < n ? steps[i + 1] : 0, i + 1 < n);
        cond[i].idx = noteIndexBucket(static_cast<int>(i));
        cond[i].kick = kickClass(step);
    }
    Rng r;
    r.seed(seed);
    NeuralStepper stepper{ &model, kBassRole, 0, kBassPhraseBars, &cond, 0 };
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
 * @param masks kBassPhraseBars onset masks, or null for the pattern family @p pat
 */
void phraseSlots(const BassPatternDef& pat, const uint16_t* masks, std::vector<int>& steps, std::vector<int>& slots)
{
    steps.clear();
    slots.clear();
    for (int bar = 0; bar < kBassPhraseBars; ++bar)
        for (int beat = 0; beat < kBeatsPerBar; ++beat) {
            if (masks == nullptr) {
                for (int s = 0; s < pat.count; ++s) {
                    steps.push_back(bar * 16 + bassSlotStep(pat, beat, s));
                    slots.push_back((bar * kBeatsPerBar + beat) * 3 + s);
                }
            } else {
                int ordinal = 0;
                for (int k = 1; k < 4; ++k)
                    if ((masks[bar] >> (beat * 4 + k)) & 1u) {
                        steps.push_back(bar * 16 + beat * 4 + k);
                        slots.push_back((bar * kBeatsPerBar + beat) * 3 + ordinal);
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
    t.bassNeural = false;
    if (p.getInt(p.base(Module::Compose) + compose::BassModel) != 1) return;
    std::string note;
    NeuralModel* model = sharedBassModel(&note);
    if (model == nullptr) {
        if (!note.empty() && !bassModelReported_) { std::fprintf(stderr, "%s\n", note.c_str()); bassModelReported_ = true; }
        return;
    }
    const uint64_t seed = mixSeed(trackSeed(t.index) ^ kSaltBassLine, 0);
    const int patterns[2] = { t.primaryPattern, t.secondaryPattern };
    std::vector<int> steps, slots;
    for (int k = 0; k < 2; ++k) {
        // With a drawn rhythm the model is asked about the steps that will really sound, not about the
        // family's: `step`, `gap` and `kick` are three of its five conditioning inputs
        // (docs/MODEL_FORMAT.md 3), so telling it the family's slots and then playing other ones would
        // throw away exactly the part of the conditioning the rhythm round is about.
        phraseSlots(kBassPatterns[patterns[k]], t.bassRhythm ? t.bassMask[k] : nullptr, steps, slots);
        if (!drawBassPhrase(*model, steps, slots, t.scale, mixSeed(seed, static_cast<uint64_t>(k)), t.bassRel[k]))
            return;
    }
    t.bassNeural = true;
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
 * * every bar of the phrase repeats the home bar unless a second coin -- `1 - kCorpusBassHomePerMille`
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
    t.bassRhythm = false;
    t.bassShortestSlot = 0.25f;
    const int cb = p.base(Module::Compose);
    if (p.getInt(cb + compose::BassRhythm) != 1) return;
    const StyleProfile& style = styleProfile(static_cast<StyleId>(t.style));
    const float amount = std::clamp(p.get(cb + compose::BassVariation) * style.hatDensity, 0.0f, 1.0f);
    const double stray = amount;
    const double depart = (1.0 - BassRhythm::homeShare()) * amount;
    const int patterns[2] = { t.primaryPattern, t.secondaryPattern };
    int shortest = 4;
    for (int k = 0; k < 2; ++k) {
        Rng r;
        r.seed(mixSeed(trackSeed(t.index) ^ kSaltBassRhythm, static_cast<uint64_t>(k)));
        BassUniform u{ &r };
        const unsigned home = u() < stray ? BassRhythm::draw(u) : BassRhythm::familyMask(patterns[k]);
        for (int bar = 0; bar < kBassPhraseBars; ++bar) {
            const unsigned mask = u() < depart ? BassRhythm::draw(u) : home;
            t.bassMask[k][bar] = static_cast<uint16_t>(mask);
            shortest = std::min(shortest, BassRhythm::shortestSpan(mask));
        }
    }
    // Every span is a whole number of sixteenths and at least one, so the shortest slot a drawn rhythm
    // can produce is a quarter beat -- exactly the Rolling family's. The gate limit and the release
    // floor of gateLimitSlot() can therefore only relax against the default, never tighten.
    t.bassShortestSlot = static_cast<float>(shortest) * 0.25f;
    t.bassRhythm = true;
}

/**
 * @brief Whether the floor under 140 Hz is free for the whole of a bar (Melody.h, the sub foundation).
 *
 * Since 19.09.2026 this is BarPlan::floorSilent rather than "this track's kick and bass rest": over the DJ
 * overlap the previous track's kick and bass still hold the floor, and a buildup's pre-drop break is a held
 * breath rather than a floor (Form.cpp).
 */
static bool foundationBar(const BarPlan& bp) { return bp.floorSilent; }

/** @name The pad's high pass while it lays the sub foundation (rule 20 of 18.09.2026).
 *  @{ */
constexpr float kFoundationHpFloor = 40.0f;   ///< Hz: under the lowest sub root (D2, 73 Hz) by nearly an octave
constexpr float kFoundationHpTrack = 0.5f;    ///< x f0: an octave under each voice, so a sub keeps its fundamental
/** @} */

/**
 * @brief Bars before the kick returns in which the drone's low octave is already over (19.09.2026).
 *
 * The drone's release is 2.5 s; two bars are 3.3 s at 145 BPM, after which a note released there has
 * fallen by about 60 dB (the pad's release was measured at 43 dB per bar for 1.8 s), so the band under
 * 140 Hz belongs to kick and bass again from their first beat. In those two bars the drone plays its
 * upper octave instead: the cross-fade from the floor into the section that follows.
 */
constexpr int kDroneLowTail = 2;

/**
 * @brief One bar as the drone sees it: whether it plays, in which octave, and whether pad or acid
 *        share its band there (20.09.2026 -- a run breaks where that changes, because the chord the
 *        drone holds changes with it).
 */
struct DroneBar { bool on = false, low = false, shaded = false; int section = -1; };

/**
 * @brief The drone's state in bar @p inTrack: on where the form sets its bit, low where the floor is
 *        silent for this bar and the kDroneLowTail bars after it (inside the track).
 */
static DroneBar droneBarAt(const TrackPlan& plan, const PartAvailability& a, int inTrack)
{
    DroneBar d;
    if (inTrack < 0 || inTrack >= plan.bars) return d;
    const BarPlan bp = planBar(plan.form, a, plan.sectionSeed, inTrack);
    d.section = bp.index;
    d.on = (bp.parts & partBit(MelodyPart::Drone)) != 0;
    if (!d.on) return d;
    d.low = bp.floorSilent;
    for (int k = 1; k <= kDroneLowTail && d.low; ++k)
        d.low = inTrack + k < plan.bars && planBar(plan.form, a, plan.sectionSeed, inTrack + k).floorSilent;
    // Until 20.09.2026 the drone left a bar in which pad or acid sounded, which is why it was measured
    // absent (Form.cpp): nearly every bar of a track has one of the two. It stays now, in its raised
    // octave and reduced to its root where those two already own the fifth (composeMelodyBar), and the
    // masking it leaves is a measurement rather than a prohibition.
    d.shaded = !d.low && (bp.parts & (partBit(MelodyPart::Pad) | partBit(MelodyPart::Acid))) != 0;
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
    const int index = std::clamp(bp.index, 0, std::max(0, plan.form.count - 1));
    int breaks = 0;
    for (int s = 0; s < index; ++s) breaks += plan.form.section[s].type == SectionType::Break ? 1 : 0;
    c.material = breaks % kMaterialSets;
    const PartAvailability a = availabilityOf(plan);
    const bool droneHere = (bp.parts & partBit(MelodyPart::Drone)) != 0 && plan.melody.present[mpIndex(MelodyPart::Drone)];
    DroneBar here;
    if (droneHere) here = droneBarAt(plan, a, inTrack);
    if ((bp.parts & partBit(MelodyPart::Pad)) != 0 && plan.melody.present[mpIndex(MelodyPart::Pad)] && foundationBar(bp)
        // A foundation starts where a chord block starts -- or where the floor falls silent inside one
        // (22.09.2026). Until then a block was two or four bars, so a breakdown's second block
        // always began on silent ground; a breakdown holds one chord for the whole of it now, its block
        // starts on the breakdown's first bar with the kick still on the downbeat, and without this
        // clause the sub never arrived at all (testFoundation.score: "0 of 0 chords in silent bars").
        && (padChordAt(plan.melody, bp, inTrack).barInBlock == 0 || inTrack == 0
            || !foundationBar(planBar(plan.form, a, plan.sectionSeed, inTrack - 1)))
        && !(droneHere && here.low)) {
        int k = 1;
        while (inTrack + k < plan.bars && k < 64 && foundationBar(planBar(plan.form, a, plan.sectionSeed, inTrack + k))) ++k;
        c.foundationBars = k;
    }
    if (droneHere && here.on) {
        const DroneBar before = droneBarAt(plan, a, inTrack - 1);
        // A run also starts at the hand-over of the DJ overlap (20.09.2026). Over the overlap the
        // *incoming* track's voices are written by transitionBar, and it writes pads and drone only
        // where the two keys are compatible -- so for an incompatible pair nothing of this track's
        // drone is emitted in its first sixteen bars, while droneBarAt still says "on" for them and
        // the run therefore looked as if it had begun there. The whole intro then stayed silent:
        // measured on the listening seed, the drone sounded in 0 of track 2's 32 intro bars. Starting
        // a run at the hand-over costs a consonant guest one re-trigger, which its 2.5 s release
        // turns into the cross-fade the drone uses at every other section boundary anyway.
        const bool start = !before.on || before.section != here.section || before.low != here.low
                        || (plan.form.handover > 0 && inTrack == plan.form.handover);
        if (start) {
            // The run is walked to its end rather than to a fixed 64 bars (20.09.2026): with the drone
            // under every bar a run is a whole section, and a cap inside a section would have ended the
            // held chord in the middle of one with nothing to follow it. A section can never be longer
            // than the track, so plan.bars is the bound.
            int k = 1;
            bool shaded = here.shaded;
            while (k < plan.bars) {
                const DroneBar next = droneBarAt(plan, a, inTrack + k);
                if (!next.on || next.section != here.section || next.low != here.low) break;
                shaded = shaded || next.shaded;
                ++k;
            }
            c.droneBars = k;
            c.droneLow = here.low;
            c.droneTail = 0;
            // What the run holds is decided once, here, and not bar by bar: the drone's attack is 1.5 s,
            // so a chord that changed whenever the pad came and went would pump instead of carrying. A
            // run in which pad or acid sound *anywhere* keeps the root alone; only a run with the band
            // to itself adds the fifth (composeMelodyBar).
            c.droneShaded = shaded;
        }
    }
    return c;
}

/** @brief Whether the drone plays bar @p inTrack in its low octave (the high pass is opened for it). */
bool droneLowAt(const TrackPlan& plan, int inTrack)
{
    if (!plan.melody.present[mpIndex(MelodyPart::Drone)]) return false;
    return droneBarAt(plan, availabilityOf(plan), inTrack).low;
}

PartAvailability availabilityOf(const TrackPlan& plan)
{
    PartAvailability a;
    for (int k = 0; k < kMelodyParts; ++k) a.part[k] = plan.melody.present[k];
    for (int k = 0; k < kMelodyParts; ++k) a.amount[k] = plan.melody.amount[k];
    a.leadLo = plan.melody.leadLo;
    a.leadHi = plan.melody.leadHi;
    a.arpLo = plan.melody.arpLo;
    a.arpHi = plan.melody.arpHi;
    a.percLayers = plan.perc.layers;
    a.hatLayers = plan.perc.hatLayers;
    return a;
}

void Composer::setLock(LockUnit unit, int index, bool locked)
{
    if (locked) locked_[static_cast<int>(unit)][index] = 1;
    else locked_[static_cast<int>(unit)].erase(index);
    plans_.clear();
    walk_.clear();
    ++planGeneration_;
}

bool Composer::isLocked(LockUnit unit, int index) const
{
    const auto& m = locked_[static_cast<int>(unit)];
    return m.find(index) != m.end();
}

void Composer::reroll(LockUnit unit, int index)
{
    if (isLocked(unit, index)) return;
    ++variation_[static_cast<int>(unit)][index];
    plans_.clear();
    walk_.clear();
    ++planGeneration_;
}

uint32_t Composer::variation(LockUnit unit, int index) const
{
    const auto& m = variation_[static_cast<int>(unit)];
    const auto it = m.find(index);
    return it == m.end() ? 0u : it->second;
}

void Composer::setVariation(LockUnit unit, int index, uint32_t value)
{
    if (value == 0) variation_[static_cast<int>(unit)].erase(index);
    else variation_[static_cast<int>(unit)][index] = value;
    plans_.clear();
    walk_.clear();
    ++planGeneration_;
}

void Composer::clearLocks()
{
    for (int u = 0; u < kNumLockUnits; ++u) { locked_[u].clear(); variation_[u].clear(); }
    plans_.clear();
    walk_.clear();
    ++planGeneration_;
}

uint64_t Composer::setSeed() const
{
    const uint32_t v = variation(LockUnit::Set, 0);
    return v == 0 ? seed_ : mixSeed(seed_ ^ kSaltReroll, v);
}

uint64_t Composer::trackSeed(int index) const
{
    // A locked track is frozen at the seed it had before any reroll, of the set or of itself; an
    // unlocked one follows the set's rerolls and adds its own variation counter.
    if (isLocked(LockUnit::Track, index)) return mixSeed(seed_ ^ kSaltTrack, static_cast<uint64_t>(index));
    const uint64_t base = mixSeed(setSeed() ^ kSaltTrack, static_cast<uint64_t>(index));
    const uint32_t v = variation(LockUnit::Track, index);
    return v == 0 ? base : mixSeed(base ^ kSaltReroll, v);
}

uint64_t Composer::sectionSeedOf(int track, int si) const
{
    const int idx = sectionUnitIndex(track, si);
    if (isLocked(LockUnit::Section, idx))
        return mixSeed(mixSeed(seed_ ^ kSaltTrack, static_cast<uint64_t>(track)) ^ kSaltSectU, static_cast<uint64_t>(si));
    const uint64_t base = mixSeed(trackSeed(track) ^ kSaltSectU, static_cast<uint64_t>(si));
    const uint32_t v = variation(LockUnit::Section, idx);
    return v == 0 ? base : mixSeed(base ^ kSaltReroll, v);
}

uint64_t Composer::laneSeedOf(int track, int lane) const
{
    const int idx = laneUnitIndex(track, lane);
    if (isLocked(LockUnit::PatternLane, idx))
        return mixSeed(mixSeed(seed_ ^ kSaltTrack, static_cast<uint64_t>(track)) ^ kSaltLaneU, static_cast<uint64_t>(lane));
    const uint64_t base = mixSeed(trackSeed(track) ^ kSaltLaneU, static_cast<uint64_t>(lane));
    const uint32_t v = variation(LockUnit::PatternLane, idx);
    return v == 0 ? base : mixSeed(base ^ kSaltReroll, v);
}

void Composer::validate(const ParamStore& p) const
{
    // The plans depend on these knobs; when any of them moves, the plans are made again.
    std::vector<float> knobs;
    const int cb = p.base(Module::Compose);
    for (int i = 0; i < compose::Count; ++i) knobs.push_back(p.get(cb + i));
    knobs.push_back(p.get(p.base(Module::Kick) + kick::Engine));
    knobs.push_back(p.get(p.base(Module::Kick) + kick::Clip));
    knobs.push_back(p.get(p.base(Module::Bass) + bass::AmpRelease));
    knobs.push_back(static_cast<float>(preferencesRevision()));   // new preferences: new plans (Preferences.h)
    for (int l = 0; l < kPercLanes; ++l) {
        const int b = p.base(Module::Perc, l);
        for (int k : { static_cast<int>(perc::Active), static_cast<int>(perc::Role), static_cast<int>(perc::Density), static_cast<int>(perc::Engine) })
            knobs.push_back(p.get(b + k));
    }
    if (knobs != planKnobs_) { plans_.clear(); walk_.clear(); ++planGeneration_; planKnobs_ = std::move(knobs); }
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
    const int cb = p.base(Module::Compose);
    const float tv = p.get(cb + compose::TrackVariation);
    const StyleProfile& style = styleProfile(styleOf(p));
    const bool styleTempo = p.getBool(cb + compose::StyleTempo);
    const double baseBpm = styleTempo ? style.bpmCentre : p.get(cb + compose::Bpm);
    const double range = styleTempo ? style.bpmRange : p.get(cb + compose::TempoRange);
    // Track lengths are multiples of 16 bars (19.09.2026; until then 32, so that every track boundary fell
    // on a 32-bar grid of the set -- the DJ overlap of the arrangement round moves every later track by
    // sixteen bars against that grid anyway, and a track's own form is what its phrases count from).
    const int baseBars = std::clamp((p.getInt(cb + compose::TrackBars) / kTrackBarStep) * kTrackBarStep, kMinTrackBars, kMaxTrackBars);

    while (static_cast<int>(walk_.size()) <= index) {
        const int i = static_cast<int>(walk_.size());
        TrackWalk w;
        if (i == 0) {
            w.bars = baseBars;
            w.key = p.getInt(cb + compose::Key);
            w.scale = p.getInt(cb + compose::Scale);
            w.bpm = styleTempo ? std::round(style.bpmCentre * 2.0) * 0.5 : baseBpm;
            w.style = static_cast<int>(styleOf(p));
            // 21.09.2026, at the user's decision: the knobs still decide the first track's key,
            // mode, tempo and length -- those are musical settings somebody typed in -- but its
            // *sound* is now drawn like every other track's. Until today the branch ended in a
            // `continue`, so track 1 had no kick recipe, no bass recipe, no acid voicing and no
            // voice recipes at all: it was the knobs exactly. A track is 256 bars, so every render
            // shorter than about seven minutes is track 1 alone -- which meant the whole palette of
            // 15 pad tables, four oscillators and four filter responses never sounded once in a
            // short render, whatever the seed. The knobs are the centre now, not the whole story.
        } else {
            const TrackWalk& prev = walk_[static_cast<size_t>(i - 1)];
            Rng r;
            r.seed(mixSeed(setSeed() ^ kSaltWalk, static_cast<uint64_t>(i)));

            // The style journey (22.09.2026). The user: "es ist langweilig, wenn immer alles im selben
            // Stil ist in einem Set". One style for the night was the knob; with compose.style_mix on
            // the style walks -- but as a journey, not a lottery: the five profiles stand in tempo order
            // (Progressive 136.5, Full-On 144, Goa 145, Dark Forest 151.5, Hi-Tech 158) and a step goes
            // to a *neighbour*, upward while the set's energy arc rises and downward while it falls,
            // now and then against it; so a DJ's tempo never jumps more than one profile at a time and
            // the night has a shape. A style holds for one to three tracks. Its own generator, so
            // nothing the older draws below decide moves because of it.
            w.style = prev.style;
            // Track Variation at 0 means every track plays the knobs, the style among them (testVariety.plans).
            if (p.getBool(cb + compose::StyleMix) && tv > 0.0f) {
                static const StyleId kByTempo[kNumStyles] = { StyleId::Progressive, StyleId::FullOn, StyleId::Goa, StyleId::DarkForest, StyleId::HiTech };
                Rng rs;
                rs.seed(mixSeed(setSeed() ^ kSaltStyle, static_cast<uint64_t>(i)));
                int pos = 1;
                for (int k = 0; k < kNumStyles; ++k) if (static_cast<int>(kByTempo[k]) == prev.style) pos = k;
                int run = 1;
                for (int k = i - 2; k >= 0 && walk_[static_cast<size_t>(k)].style == prev.style; --k) ++run;
                double startBar = 0.0, prevStart = 0.0;
                for (int k = 0; k < i; ++k) { if (k == i - 1) prevStart = startBar; startBar += walk_[static_cast<size_t>(k)].bars; }
                const double setBars = setLengthBars(p);
                const bool rising = arcEnergy(arcOf(p), startBar / setBars) >= arcEnergy(arcOf(p), prevStart / setBars);
                const bool move = rs.uniform() < 0.4f || run >= 3;
                const bool against = rs.uniform() < 0.25f;
                if (move) {
                    int dir = (rising != against) ? 1 : -1;
                    if (pos + dir < 0 || pos + dir >= kNumStyles) dir = -dir;
                    pos = std::clamp(pos + dir, 0, kNumStyles - 1);
                    w.style = static_cast<int>(kByTempo[pos]);
                }
            }
            // From here on the track's own profile: its modes, and its tempo centre when Style Tempo is on.
            const StyleProfile& ts = styleProfile(static_cast<StyleId>(w.style));
            const double trackBpm = styleTempo ? ts.bpmCentre : baseBpm;
            const double trackRange = styleTempo ? ts.bpmRange : range;

            // Length: the knob, give or take one 16-bar block -- at the default 256 bars that is 240 .. 272, inside
            // the user's 220 .. 280 (19.09.2026; until then two 32-bar blocks either way).
            w.bars = baseBars;
            if (r.uniform() < tv) w.bars = std::clamp(baseBars + kTrackBarStep * (r.below(3) - 1), kMinTrackBars, kMaxTrackBars);

            // Key: by fifths and whole tones, a minor third now and then, rarely a semitone. 22.09.2026,
            // the user ("auch immer dieselbe Tonart"): the knob's chance is raised by a quarter, and after
            // two tracks in one key the third *must* move -- a key held for three tracks is over twenty
            // minutes of one tonic. The coin is still tossed once, so the draws after it keep their place.
            w.key = prev.key;
            const bool heldTwice = tv > 0.0f && i >= 2 && walk_[static_cast<size_t>(i - 2)].key == prev.key;
            const float keyCoin = r.uniform();   // drawn whatever tv is, so the draws after it keep their place
            const bool moveKey = tv > 0.0f && keyCoin < std::min(1.0f, tv + 0.25f);
            if (moveKey || heldTwice) {
                static const int kMoves[8] = { 7, 5, 2, -2, 3, -3, 1, -1 };
                static const double kWeights[8] = { 0.28, 0.28, 0.12, 0.12, 0.06, 0.06, 0.04, 0.04 };
                w.key = ((prev.key + kMoves[pick(r, kWeights, 8)]) % 12 + 12) % 12;
            }
            // Mode: mostly kept; when it changes, the track's style profile's weights decide.
            w.scale = prev.scale;
            if (r.uniform() < 0.25f * tv) w.scale = pick(r, ts.scaleWeight, kNumScales);
            // Tempo: mean-reverting walk around the track's centre, inside its range, on half-BPM steps.
            // The centre itself follows the set's energy arc (kArcTempoPerUnit): where the night rises the
            // tracks run a little faster, where it closes a little slower.
            double arcCentre = trackBpm;
            {
                double startBar = 0.0;
                for (int k = 0; k < i; ++k) startBar += walk_[static_cast<size_t>(k)].bars;
                const ArcId arc = arcOf(p);
                arcCentre += kArcTempoPerUnit * (arcEnergy(arc, startBar / setLengthBars(p)) - arcEnergy(arc, 0.0));
            }
            const double walk = arcCentre + 0.6 * (prev.bpm - arcCentre) + (2.0 * r.uniform() - 1.0) * trackRange * tv;
            w.bpm = std::round(std::clamp(walk, arcCentre - trackRange, arcCentre + trackRange) * 2.0) * 0.5;

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
            rm.seed(mixSeed(setSeed() ^ kSaltMotif, static_cast<uint64_t>(i)));
            if (i == 0) {
                const LeadStyle& ls = styleProfile(static_cast<StyleId>(w.style)).lead;
                w.motif.band = static_cast<int8_t>(std::clamp(static_cast<int>(std::lround(ls.density * 2.0 + (static_cast<double>(rm.uniform()) - 0.5))), 0, 2));
                w.motif.cell = drawMotifCell(mixSeed(setSeed() ^ kSaltMotif, 0xC0FFEEull), w.motif.band);
                w.motif.archetype = static_cast<int8_t>(pick(rm, ls.archetype, kNumLeadArchetypes));
                w.motif.phrase = 0;
            } else {
                w.motif = walk_[0].motif;
                double startBar = 0.0;
                for (int k = 0; k < i; ++k) startBar += walk_[static_cast<size_t>(k)].bars;
                const double setBars = setLengthBars(p);
                const bool carriesEnd = startBar < setBars && startBar + w.bars >= setBars;
                const bool recall = rm.uniform() < 0.15f;   // drawn whatever i is, so the stream is one per track
                w.motif.phrase = (carriesEnd || (i >= 2 && recall)) ? 1 : -1;
            }
        }

        // Recipes by best candidate against the last four tracks.
        Rng rr;
        rr.seed(mixSeed(setSeed() ^ kSaltRecipe, static_cast<uint64_t>(i)));
        auto chooseRecipe = [&](float* out, int n, bool kickSide) {
            float best[kNumKickMacros] = {};
            double bestScore = -1.0;
            for (int c = 0; c < 12; ++c) {
                float cand[kNumKickMacros] = {};
                drawRecipe(rr, cand, n);
                double score = 1e9;
                for (int back = 1; back <= 4 && i - back >= 0; ++back) {
                    const TrackWalk& o = walk_[static_cast<size_t>(i - back)];
                    const float* om = kickSide ? o.kickMacro : o.bassMacro;
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
        chooseRecipe(w.kickMacro, kNumKickMacros, true);
        chooseRecipe(w.bassMacro, kNumBassMacros, false);

        // The acid voicing (19.09.2026): a point in the triangle of the three voicings. Candidates are
        // uniform on the triangle -- normalised exponential draws, the flat Dirichlet distribution -- and
        // the one farthest from the previous two tracks' points wins, the same best-candidate rule as
        // the recipes. Its own generator, so the kick and bass recipes above are the ones they were.
        Rng ra;
        ra.seed(mixSeed(setSeed() ^ kSaltAcidVoice, static_cast<uint64_t>(i)));
        double bestAcid = -1.0;
        for (int c = 0; c < 12; ++c) {
            float cand[kNumAcidVoicings];
            float sum = 0.0f;
            for (float& x : cand) { x = static_cast<float>(-std::log(1.0 - 0.999999 * static_cast<double>(ra.uniform()))); sum += x; }
            for (float& x : cand) x /= sum;
            double score = 1e9;
            for (int back = 1; back <= 2 && i - back >= 0; ++back) {
                const float* o = walk_[static_cast<size_t>(i - back)].acidVoicing;
                double d = 0.0;
                for (int k = 0; k < kNumAcidVoicings; ++k) d += (cand[k] - o[k]) * (cand[k] - o[k]);
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
            if (score > bestAcid) { bestAcid = score; std::copy(cand, cand + kNumAcidVoicings, w.acidVoicing); }
        }

        // The voice recipes (19.09.2026, Composer.h, VoiceRecipe): per voice twelve candidates -- an
        // oscillator, a table and a response from the voice's palette (kVoicePalette), weighted by the
        // palette, delay times, and five directions from the same truncated normal the kick
        // and bass recipes use -- and the candidate farthest from the same voice in the previous two
        // tracks wins, so that neighbouring tracks never share a voice's sound. A discrete choice that
        // differs counts as one unit of distance (a whole direction's range is two). Its own generator:
        // nothing drawn above moves.
        Rng rv;
        rv.seed(mixSeed(setSeed() ^ kSaltVoice, static_cast<uint64_t>(i)));
        // The counter-lead against this track's lead (19.09.2026, the user: "Die Counter-Lead sollte
        // natuerlich einen anderen Sound haben als die Haupt-Lead"). Until then a voice's candidates were
        // scored only against the same voice in the two tracks before, and the counter's palette shared two
        // of the lead lane's tables and three of its oscillators, so a track could give both the same
        // oscillator and table an octave apart. Now the counter's palette is the vocal family alone (Formant
        // Saw, Vocal, Glass), a candidate with the lead's oscillator or table is never taken, a different
        // filter response counts, and the distance to the lead is part of the score.
        const int leadV = polyIndex(PolyInstance::Lead), counterV = polyIndex(PolyInstance::Counter);
        static_assert(polyIndex(PolyInstance::Lead) < polyIndex(PolyInstance::Counter), "the lead is drawn before the counter");
        for (int v = 0; v < kPolyInstances; ++v) {
            const VoicePalette& pal = kVoicePalette[v];
            const int nTables = paletteTableCount(pal);
            double bestScore = -1.0;
            for (int c = 0; c < 12; ++c) {
                VoiceRecipe cand;
                cand.osc = pick(rv, pal.osc, static_cast<int>(PolyOsc::Count));
                cand.table = nTables > 0 ? paletteTableAt(pal, rv.below(nTables)) : -1;
                cand.filter = pick(rv, pal.filter, static_cast<int>(PolyFilter::Count));
                // The echo is a property of the *role* since 20.09.2026 (round "dialogue"). The user's
                // rule: "Lead auf ein punktiertes Achtel (3/16), Counter auf ein schnelles 1/16
                // Ping-Pong". Until then every voice drew its two times from the same four, so a track
                // could give the lead a sixteenth and the counter a dotted eighth -- exactly the wrong
                // way round -- and the contrast the rule is about was a matter of luck. Now each voice
                // draws inside its own family (kVoiceDelayL / kVoiceDelayR), so a track still varies
                // its echoes but never trades the two leads' characters. Two draws either way, so no
                // other decision of this candidate moves.
                const VoiceDelayFamily& fam = kVoiceDelay[v];
                cand.delayL = fam.left[rv.below(fam.leftCount)];
                cand.delayR = fam.right[rv.below(fam.rightCount)];
                // The second oscillator (22.09.2026): which one answers, and at which interval. Two
                // draws whatever the outcome, so "no second oscillator this track" costs the same
                // random numbers as any other result and no other decision of this candidate moves.
                cand.osc2 = pick(rv, pal.osc2, static_cast<int>(PolyOsc2::Count));
                cand.osc2Semis = pick(rv, pal.interval, static_cast<int>(PolyOsc2Interval::Count));
                cand.hasOsc2 = true;
                drawRecipe(rv, cand.macro, kNumVoiceMacros);
                double score = 1e9;
                for (int back = 1; back <= 2 && i - back >= 0; ++back) {
                    const VoiceRecipe& o = walk_[static_cast<size_t>(i - back)].voice[v];
                    double d = 0.0;
                    for (int k = 0; k < kNumVoiceMacros; ++k) d += (cand.macro[k] - o.macro[k]) * (cand.macro[k] - o.macro[k]);
                    d = std::sqrt(d) + (cand.osc != o.osc ? 1.0 : 0.0) + (cand.table != o.table ? 1.0 : 0.0)
                      + (cand.filter != o.filter ? 0.5 : 0.0)
                      // The second oscillator counts like the filter: half a unit for the kind and
                      // half for the interval. A track whose pad answers an octave lower where the
                      // last one answered at a fifth is a different pad, whatever else it drew.
                      + (cand.osc2 != o.osc2 ? 0.5 : 0.0) + (cand.osc2Semis != o.osc2Semis ? 0.5 : 0.0);
                    score = std::min(score, d);
                }
                if (v == counterV) {
                    const VoiceRecipe& lead = w.voice[leadV];
                    if (cand.osc == lead.osc || cand.table == lead.table) continue;   // never the lead's sound
                    double d = 0.0;
                    for (int k = 0; k < kNumVoiceMacros; ++k) d += (cand.macro[k] - lead.macro[k]) * (cand.macro[k] - lead.macro[k]);
                    score = std::min(score, 1e8) + 0.5 * std::sqrt(d) + (cand.filter != lead.filter ? 0.5 : 0.0);
                }
                // The first track: the mildest macro vector, not the first draw (22.09.2026). Its
                // parts are the reference the level match brings every later track's to
                // (Composer.h, "Level match"), so an extreme first draw moves the whole night. The
                // discrete choices -- oscillator, table, filter, the two delays -- are left exactly
                // as drawn: those are what gives the first track a sound of its own, which is the
                // point of it having a recipe at all.
                if (i == 0) {
                    double norm = 0.0;
                    for (int k = 0; k < kNumVoiceMacros; ++k) norm += static_cast<double>(cand.macro[k]) * cand.macro[k];
                    score = -std::sqrt(norm);
                }
                if (score > bestScore) { bestScore = score; w.voice[v] = cand; }
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
                VoiceRecipe& c = w.voice[v];
                const VoiceRecipe& lead = w.voice[leadV];
                const bool noCandidateSurvived = bestScore <= -1.0;
                if (noCandidateSurvived || c.osc == lead.osc) {
                    int best = -1;
                    for (int o = 0; o < static_cast<int>(PolyOsc::Count); ++o)
                        if (o != lead.osc && pal.osc[o] > 0.0 && (best < 0 || pal.osc[o] > pal.osc[best])) best = o;
                    c.osc = best;
                }
                if (noCandidateSurvived || c.table == lead.table)
                    for (int k = 0; k < nTables; ++k) { const int t = paletteTableAt(pal, k); if (t != lead.table) { c.table = t; break; } }
            }
        }
        walk_.push_back(w);
    }
    return walk_[static_cast<size_t>(index)];
}

TrackPlan Composer::makeTrack(const ParamStore& p, int index) const
{
    const int cb = p.base(Module::Compose);
    const float tv = p.get(cb + compose::TrackVariation);
    const float sv = p.get(cb + compose::SoundVariation);
    const int reg = p.getInt(cb + compose::BassRegister);
    const float releaseKnob = p.get(p.base(Module::Bass) + bass::AmpRelease);
    const TrackWalk& w = walkAt(p, index);
    const StyleProfile& style = styleProfile(static_cast<StyleId>(w.style));   // the track's own (22.09.2026)

    TrackPlan t;
    t.style = w.style;
    t.index = index;
    // The DJ blend (Form.h, djOverlapBars; 23.09.2026): every track after the first starts its blend's length
    // before the one before it ends, its kick-free intro over the other's outro. The length is the incoming
    // style's, never longer than the outgoing outro; the incoming intro is checked once the form exists.
    int overlap = 0;
    if (index > 0) {
        const TrackPlan& prev = plans_[static_cast<size_t>(index - 1)];
        overlap = std::min(djOverlapBars(static_cast<StyleId>(w.style)), prev.form.count > 0 ? prev.form.section[prev.form.count - 1].bars : kDjOverlap);
        t.firstBar = prev.firstBar + prev.bars - overlap;
    }
    t.key = w.key;
    t.scale = w.scale;
    t.bpm = w.bpm;
    std::copy(w.kickMacro, w.kickMacro + kNumKickMacros, t.kickMacro);
    std::copy(w.bassMacro, w.bassMacro + kNumBassMacros, t.bassMacro);
    std::copy(w.acidVoicing, w.acidVoicing + kNumAcidVoicings, t.acidVoicing);
    for (int v = 0; v < kPolyInstances; ++v) t.voice[v] = w.voice[v];
    // The tables this track's voices will read, expanded now, on whichever thread planned the track
    // (21.09.2026). The pack is only indexed at load; a table costs its 1.9 MB of mip levels when a
    // track first asks for it and not before, which is what lets the library grow past a handful.
    // The audio thread never builds: it reads what is published or falls back (WaveTableFile.h).
    {
        int want[kPolyInstances];
        int n = 0;
        for (int v = 0; v < kPolyInstances; ++v)
            if (t.voice[v].table >= 0) want[n++] = t.voice[v].table;
        if (n > 0) ensureWaveTables(want, n);
    }

    // Where the track sits on the set's energy arc (Form.h).
    const double setBars = setLengthBars(p);
    const ArcId arc = arcOf(p);
    t.arcIn = static_cast<float>(arcEnergy(arc, t.firstBar / setBars));
    t.arcOut = static_cast<float>(arcEnergy(arc, (t.firstBar + w.bars) / setBars));

    // The form: a track's own decision, so it hangs off the track's seed and is rerollable. The
    // section seeds are made first, because since 16.09.2026 the form draws each section's borrowed
    // mode from the section's own seed (Form.h) -- a section is a lockable unit and its mode has to
    // move with it. They do not depend on the form, only on the track, so nothing is circular.
    for (int s = 0; s < kMaxSections; ++s) t.sectionSeed[s] = sectionSeedOf(index, s);
    t.formSeed = mixSeed(trackSeed(index) ^ kSaltFormU, 0);
    // Modal interchange off: a style profile whose chance is zero, which makes every section keep
    // the track's mode. That is the A/B the self test uses to prove the bass does not move.
    StyleProfile interchange = style;
    if (!p.getBool(cb + compose::ModalInterchange)) interchange.interchangeChance = 0.0f;
    t.form = makeFormPlan(interchange, t.formSeed, w.bars, t.arcIn, t.arcOut, t.scale, t.sectionSeed);
    if (index > 0) {
        // The blend can be no longer than this track's intro (a short track shrinks its intro to sixteen,
        // Form.cpp). A shorter blend moves firstBar by at most sixteen bars against the arc position the
        // form was made with; the arc's energy hardly moves over sixteen bars of a set, so the form stands.
        TrackPlan& prev = plans_[static_cast<size_t>(index - 1)];
        overlap = std::max(kDjOverlap, std::min(overlap, t.form.section[0].bars));
        t.firstBar = prev.firstBar + prev.bars - overlap;
        prev.form.overlapTail = overlap;   // the outgoing outro builds its blend from it (Form.cpp, planBar)
    }
    t.form.handover = index == 0 ? 0 : overlap;
    makeFormSfx(t.form, t.formSeed, p.get(cb + compose::SfxAmount), p.get(cb + compose::VoiceDensity), p.get(cb + compose::BedDensity));
    t.bars = t.form.bars;

    uint64_t laneSeeds[kPercLanes];
    for (int l = 0; l < kPercLanes; ++l) laneSeeds[l] = laneSeedOf(index, l);
    t.percSeed = mixSeed(trackSeed(index) ^ kSaltPerc, 0);
    t.perc = makePercPlan(p, t.percSeed, index == 0, laneSeeds);
    t.melodySeed = mixSeed(trackSeed(index) ^ kSaltMelody, 0);
    // The colour -- how many notes of acid, lead and arp are the flat second or the augmented second --
    // follows the style profile and the track's place on the energy arc (Farbood's dissonance). Since
    // 18.09.2026 it scales one calibrated target share per role (Melody.cpp, kColourShare) and is the
    // only thing that decides the colour; it no longer weights the sampler's sets.
    const float colour = std::clamp(style.colour * (0.4f + 0.6f * 0.5f * (t.arcIn + t.arcOut)), 0.0f, 1.0f);
    // The form's scale mask says which modes need recoloured material; with interchange off it holds
    // the track's mode alone and makeMelodyPlan does exactly what it did before.
    // The bass pattern before the melody (22.09.2026, round "Lead"): the lead's rhythm interlocks with
    // the bass, so the pattern has to exist when the melody is drawn. Its draws are the first ones of
    // the track's generator, as they were; only their place in this function moved, so every draw
    // after them keeps its value.
    Rng r;
    r.seed(trackSeed(index));
    t.primaryPattern = std::clamp(p.getInt(cb + compose::BassPattern), 0, kNumBassPatterns - 1);
    t.secondaryPattern = t.primaryPattern == 0 ? 1 : 0;
    if (index > 0) {
        if (r.uniform() < tv) {
            static const double kWeights[kNumBassPatterns] = { 0.45, 0.20, 0.12, 0.08, 0.15 };
            t.primaryPattern = pick(r, kWeights, kNumBassPatterns);
        }
        double wp[kNumBassPatterns] = { 0.40, 0.30, 0.20, 0.05, 0.05 };
        wp[t.primaryPattern] = 0.0;
        t.secondaryPattern = pick(r, wp, kNumBassPatterns);
    }
    // The bass onsets the lead interlocks with: the primary pattern's family figure. A drawn bass
    // rhythm (compose.bass_rhythm = Corpus) strays from it now and then; the family is what the
    // track's groove is built on, and the mask is decided before the rhythm exists.
    t.melody = makeMelodyPlan(p, style, t.melodySeed, t.key, t.scale, index == 0, colour, t.form.scaleMask,
                              BassRhythm::familyMask(t.primaryPattern), w.motif);

    if (index == 0) {
        t.gate = p.get(cb + compose::BassGate);
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
    const int root = bassRootNote(t.key, reg);
    const int lowest = root + scaleDegree(t.scale, 6) - 12;
    const float gateMax = t.bassRhythm
        ? gateLimitSlot(t.bassShortestSlot, t.bpm, lowest, releaseKnob)
        : std::min(gateLimit(t.primaryPattern, t.bpm, lowest, releaseKnob),
                   gateLimit(t.secondaryPattern, t.bpm, lowest, releaseKnob));
    t.gate = std::clamp(p.get(cb + compose::BassGate) + (2.0f * r.uniform() - 1.0f) * 0.12f * tv, 0.35f, std::max(0.35f, gateMax));

    // The learned bass phrases, after the patterns and the rhythm they fill and before any probe
    // render, so that the level match hears the track the way it will be played.
    makeBassPhrases(p, t);

    // Kick character switches.
    t.kickEngine = -1;
    if (r.uniform() < 0.15f * sv) t.kickEngine = 1 - p.getInt(p.base(Module::Kick) + kick::Engine);
    t.kickClip = r.uniform() < 0.3f * sv ? 1 : -1;

    measureTrack(p, t);
    return t;
}

const TrackPlan& Composer::track(const ParamStore& p, int index) const
{
    validate(p);
    while (static_cast<int>(plans_.size()) <= index) plans_.push_back(makeTrack(p, static_cast<int>(plans_.size())));
    return plans_[static_cast<size_t>(index)];
}

int Composer::trackOfBar(const ParamStore& p, int bar) const
{
    validate(p);
    int i = 0;
    for (;;) {
        const TrackPlan& t = track(p, i);
        if (bar < t.firstBar + t.bars) return i;
        ++i;
    }
}

int Composer::incomingOfBar(const ParamStore& p, int bar) const
{
    const int ti = trackOfBar(p, bar);
    const TrackPlan& t = track(p, ti);
    if (bar < t.firstBar + t.bars - kDjOverlapMax) return -1;
    const TrackPlan& next = track(p, ti + 1);
    return bar >= next.firstBar ? ti + 1 : -1;
}

TempoMap Composer::tempoMap(const ParamStore& p, int bars) const
{
    // Held per track from its hand-over, ramped over the DJ overlap into the next -- the same sixteen bars
    // at the end of every track as before 19.09.2026, now the bars in which the next track's intro already
    // sounds over this one's bare outro (Form.h, kDjOverlap).
    TempoMap m;
    const TrackPlan& first = track(p, 0);
    m.setConstant(first.bpm);
    for (int i = 0;; ++i) {
        // By value: computing the next plan may move the cached ones.
        const TrackPlan t = track(p, i);
        const double start = static_cast<double>(handoverBar(t)) * kBeatsPerBar;
        if (i > 0) m.add(start, t.bpm, false);
        if (t.firstBar + t.bars >= bars) break;
        const TrackPlan next = track(p, i + 1);
        if (next.bpm != t.bpm) {
            const double rampStart = static_cast<double>(next.firstBar) * kBeatsPerBar;
            m.add(std::max(start, rampStart), t.bpm, true);
        }
    }
    return m;
}

std::vector<SectionMark> Composer::sections(const ParamStore& p, int bars) const
{
    std::vector<SectionMark> out;
    for (int i = 0;; ++i) {
        // By value: computing the next plan may move the cached ones.
        const TrackPlan t = track(p, i);
        for (int s = 0; s < t.form.count; ++s) {
            const Section& sec = t.form.section[s];
            const int startBar = t.firstBar + sec.startBar;
            if (startBar >= bars) break;
            SectionMark m;
            m.beat = static_cast<double>(startBar) * kBeatsPerBar;
            m.type = sec.type;
            m.energy = sec.energy;
            m.track = i;
            // The cut sits at the head of the breakdown it belongs to, the pre-drop break in the last
            // bar of its buildup; both are sub-bar categories of Grosz et al. and get their own mark.
            if (sec.type == SectionType::Break && sec.cutBeats > 0.0f) {
                SectionMark c = m;
                c.type = SectionType::Cut;
                out.push_back(c);
            }
            out.push_back(m);
            if (sec.type == SectionType::Build) {
                SectionMark d = m;
                d.type = SectionType::Pdb;
                d.beat = static_cast<double>(startBar + sec.bars - 1) * kBeatsPerBar;
                if (startBar + sec.bars - 1 < bars) out.push_back(d);
            }
        }
        if (t.firstBar + t.bars >= bars) break;
    }
    std::stable_sort(out.begin(), out.end(), [](const SectionMark& a, const SectionMark& b) { return a.beat < b.beat; });
    return out;
}

/**
 * @brief The incoming track's share of a bar of the DJ overlap (19.09.2026, round "arrangement").
 *
 * All the tracks come from one hand, so the transition is written rather than mixed, but it is written
 * the way a DJ mixes two records (Form.h, kDjOverlap): the incoming track's intro starts sixteen bars
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
    const TrackPlan& guest = plans_[static_cast<size_t>(gi)];
    const TrackPlan& owner = plans_[static_cast<size_t>(gi - 1)];
    const int inTrack = bar - guest.firstBar;
    const double barBeat = static_cast<double>(bar) * kBeatsPerBar;
    const BarPlan bp = planBar(guest.form, availabilityOf(guest), guest.sectionSeed, inTrack);
    const int move = ((guest.key - owner.key) % 12 + 12) % 12;
    const bool consonant = move == 0 || move == 5 || move == 7;

    if (controls != nullptr) {
        if (inTrack == 0) trackStartControls(p, guest, barBeat, *controls, ControlScope::Voices);
        if (bp.barInSection == 0) sectionControls(p, guest, bp, barBeat, *controls, ControlScope::Voices);
        leadArcControls(p, guest, bp, inTrack, barBeat, *controls);
        // The fader ride into the swap (23.09.2026, round "DJ"): eight bars before the hand-over the track
        // gain and the master offset start ramping from the outgoing track's values to the ones the incoming
        // floor will write at the hand-over -- the same numbers trackStartControls and sectionControls push
        // there, so the swap itself moves nothing. Without the ramp both jump at the hand-over, which is
        // heard as "ploetzlich wieder lauter".
        if (guest.form.handover >= 8 && inTrack == guest.form.handover - 8) {
            const int mb = p.base(Module::Mix);
            const BarPlan at = planBar(guest.form, availabilityOf(guest), guest.sectionSeed, guest.form.handover);
            const Section& sec = guest.form.section[std::clamp(at.index, 0, kMaxSections - 1)];
            const double u = sec.bars > 1 ? static_cast<double>(at.barInSection) / sec.bars : 0.0;
            const ParamDesc& g = p.desc(mb + mix::TrackGain);
            ControlEvent c;
            c.beat = barBeat;
            c.kind = ControlEvent::Kind::Offset;
            c.length = 8.0f * static_cast<float>(kBeatsPerBar);
            c.param = static_cast<int16_t>(mb + mix::TrackGain);
            c.value = (guest.gainDb + energyGainDb(at.energy) + sectionTrimDb(guest.form, at.index, u)) / (g.maxValue - g.minValue);
            controls->push_back(c);
            if (!guest.masterDeferred) {
                const ParamDesc& mg = p.desc(p.base(Module::Master) + master::Gain);
                c.param = static_cast<int16_t>(p.base(Module::Master) + master::Gain);
                c.value = guest.masterGainDb / (mg.maxValue - mg.minValue);
                controls->push_back(c);
            }
        }
        const int pb = p.base(PolyInstance::Pad), db = p.base(PolyInstance::Drone);
        auto closeHighPass = [&](int base) {
            // The floor is the outgoing track's here: the incoming pad and drone keep their knobs' high pass.
            ControlEvent h;
            h.beat = barBeat;
            h.kind = ControlEvent::Kind::Offset;
            h.value = 0.0f;
            h.param = static_cast<int16_t>(base + poly::HpFloor);
            controls->push_back(h);
            h.param = static_cast<int16_t>(base + poly::HpTrack);
            controls->push_back(h);
        };
        if (guest.melody.present[mpIndex(MelodyPart::Pad)]) closeHighPass(pb);
        if (guest.melody.present[mpIndex(MelodyPart::Drone)]) {
            closeHighPass(db);
            droneControls(p, guest, inTrack, barBeat, *controls);
        }
    }

    PercBarSpec spec;
    spec.layers = bp.percLayers;
    spec.fills = false;
    spec.quietHats = bp.quietHats;
    spec.shaker = bp.shaker;
    spec.offbeatHat = bp.offbeatHat;
    spec.hatLevel = bp.hatLevel;
    spec.cycleBar = bp.cycleBar;
    composePercBar(p, guest.perc, guest.percSeed, bar, inTrack, owner.bpm, guest.key, guest.scale, spec, out);
    if (consonant) {
        const size_t from = out.size();
        composeMelodyBar(p, guest.melody, bar, inTrack, guest.scale, bp, out, melodyContext(guest, bp, inTrack));
        const int flat9 = (owner.key + 1) % 12;
        out.erase(std::remove_if(out.begin() + static_cast<std::ptrdiff_t>(from), out.end(), [&](const NoteEvent& e) {
                      return (e.part == Part::Pad || e.part == Part::Drone) && e.pitch % 12 == flat9;
                  }), out.end());
    }
    composeSfxBar(guest.form, static_cast<double>(guest.firstBar) * kBeatsPerBar, inTrack, out);
}

void Composer::composeBars(const ParamStore& p, int firstBar, int count, std::vector<NoteEvent>& out,
                           std::vector<ControlEvent>* controls) const
{
    const int cb = p.base(Module::Compose);
    const int kickPattern = p.getInt(cb + compose::KickPattern);
    const float bassVar = p.get(cb + compose::BassVariation);
    const int reg = p.getInt(cb + compose::BassRegister);

    const size_t noteStart = out.size();
    const size_t ctlStart = controls ? controls->size() : 0;
    for (int bar = firstBar; bar < firstBar + count; ++bar) {
        const int ti = trackOfBar(p, bar);
        // The overlap window needs the next track's plan; making it may move the cache, so it is made
        // before any reference into the cache is taken.
        { const TrackPlan& t0 = track(p, ti); if (bar - t0.firstBar >= t0.bars - kDjOverlapMax) track(p, ti + 1); }
        const TrackPlan& plan = track(p, ti);
        // The incoming track of the DJ overlap, if one sounds in this bar (Form.h, kDjOverlap).
        int incoming = ti + 1 < static_cast<int>(plans_.size()) && bar >= plans_[static_cast<size_t>(ti + 1)].firstBar ? ti + 1 : -1;
        const size_t barNotes = out.size();
        // One track alone (setSoloTrack, 23.09.2026): its own bars without a guest, its intro as the guest alone,
        // and nothing of any other track.
        if (soloTrack_ >= 0) {
            if (ti == soloTrack_) {
                incoming = -1;
            } else if (incoming == soloTrack_) {
                const TrackPlan& solo = plans_[static_cast<size_t>(incoming)];
                // Its floor settings at its first bar: in the set the outgoing track holds the floor until the
                // hand-over, alone there is nobody, and the engine would play the intro on the previous state.
                if (controls != nullptr && bar == solo.firstBar)
                    trackStartControls(p, solo, static_cast<double>(bar) * kBeatsPerBar, *controls, ControlScope::Floor);
                std::vector<NoteEvent> guest;
                transitionBar(p, incoming, bar, guest, controls);
                out.insert(out.end(), guest.begin(), guest.end());
                continue;
            } else {
                continue;
            }
        }
        const int inTrack = bar - plan.firstBar;
        const float progress = static_cast<float>(inTrack) / static_cast<float>(plan.bars);
        const double barBeat = static_cast<double>(bar) * kBeatsPerBar;
        const int baseRoot = bassRootNote(plan.key, reg);
        const bool follow = p.getBool(cb + compose::BassFollowsChords);
        const int root = follow ? baseRoot + bassChordShift(plan.melody, plan.scale, inTrack, baseRoot) : baseRoot;
        const int chordDeg = follow ? padChordAt(plan.melody, plan.form, inTrack).degree : 0;   // the pad's chord, set 2 behind the main breakdown (23.09.2026)

        // Pattern of this bar: the secondary one for the last four bars of some 16-bar blocks.
        Rng block;
        block.seed(mixSeed(seed_ ^ kSaltBlock, (static_cast<uint64_t>(ti) << 32) | static_cast<uint64_t>(inTrack / 16)));
        const bool secondaryBlock = inTrack >= 16 && block.uniform() < 0.35f * bassVar;
        const bool bassBreak = block.uniform() < 0.3f * bassVar;
        const bool secondary = secondaryBlock && inTrack % 16 >= 12;
        const int pattern = secondary ? plan.secondaryPattern : plan.primaryPattern;
        const BassPatternDef& pat = kBassPatterns[pattern];
        // The bar's onset mask when the rhythm is drawn (Composer.h). The masks live on the learned
        // phrase's eight-bar cycle, the same one the pitches use.
        const unsigned mask = plan.bassRhythm ? plan.bassMask[secondary ? 1 : 0][inTrack % kBassPhraseBars] : 0u;

        // What the form says plays in this bar (Form.h).
        BarPlan bp = planBar(plan.form, availabilityOf(plan), plan.sectionSeed, inTrack);

        if (controls != nullptr) {
            // A track's floor takes over at its hand-over: its first bar in the set's first track, its own
            // bar 17 -- the outgoing track's end -- in every later one, whose voices started at its first bar
            // (transitionBar). Sections that start later write everything at their first bar as before.
            const int handover = plan.form.handover;
            if (inTrack == handover) {
                trackStartControls(p, plan, barBeat, *controls, handover == 0 ? ControlScope::All : ControlScope::Floor);
                arcControls(p, plan, inTrack, barBeat, false, *controls);
                if (handover > 0 && bp.barInSection > 0) sectionControls(p, plan, bp, barBeat, *controls, ControlScope::Floor);
            } else if (inTrack % 32 == 0 && inTrack > handover) {
                arcControls(p, plan, inTrack, barBeat, true, *controls);
            }
            if (bp.barInSection == 0 && inTrack >= handover) sectionControls(p, plan, bp, barBeat, *controls);
            if (inTrack >= handover) leadArcControls(p, plan, bp, inTrack, barBeat, *controls);
            // The pad's high pass for the sub foundation (rule 20): opened to kFoundationHpFloor and
            // an octave under each voice wherever the form silences kick and bass, the knobs
            // everywhere else. Every bar carries its state, so a bar composed alone carries it too;
            // a pad voice reads its high pass once, at its note-on (Poly.cpp), so the switch never
            // moves a filter under a sounding note and cannot click.
            // The same for the drone's low octave (19.09.2026): open where it lies on the silent floor
            // (droneLowAt), closed -- its knobs, 140 Hz -- everywhere else.
            auto highPass = [&](PolyInstance inst, bool open) {
                const int pb = p.base(inst);
                ControlEvent h;
                h.beat = barBeat;
                h.kind = ControlEvent::Kind::Offset;
                h.param = static_cast<int16_t>(pb + poly::HpFloor);
                h.value = open ? p.toNormalised(pb + poly::HpFloor, kFoundationHpFloor) - p.toNormalised(pb + poly::HpFloor, p.get(pb + poly::HpFloor)) : 0.0f;
                controls->push_back(h);
                h.param = static_cast<int16_t>(pb + poly::HpTrack);
                h.value = open ? p.toNormalised(pb + poly::HpTrack, kFoundationHpTrack) - p.toNormalised(pb + poly::HpTrack, p.get(pb + poly::HpTrack)) : 0.0f;
                controls->push_back(h);
            };
            // Over the DJ overlap the incoming track's pad and drone are the ones sounding, and it writes
            // their controls (transitionBar); the outgoing outro's bare bars have neither.
            if (incoming < 0 && plan.melody.present[mpIndex(MelodyPart::Pad)]) highPass(PolyInstance::Pad, foundationBar(bp));
            if (incoming < 0 && plan.melody.present[mpIndex(MelodyPart::Drone)]) {
                highPass(PolyInstance::Drone, droneLowAt(plan, inTrack));
                droneControls(p, plan, inTrack, barBeat, *controls);
            }
            ControlEvent c;
            c.beat = barBeat;
            c.param = static_cast<int16_t>(cb + compose::BassPattern);
            c.kind = ControlEvent::Kind::Override;
            c.value = static_cast<float>(pattern);
            controls->push_back(c);
            if (!plan.bassRhythm) {
                // One clearing event a bar while the rhythm comes from a family. Without it a switch
                // of compose.bass_rhythm from Corpus back to Pattern *during* playback would leave
                // `Engine::slotBeats_` holding the last drawn slot for ever: the composer would stop
                // sending, and the engine has no way of telling "nothing sent yet" from "nothing sent
                // any more". A value <= 0 hands the engine back its own derivation (Score.h), so a
                // Pattern render sounds exactly as it did before 16.09.2026 -- the extra event only
                // recomputes, at a beat that already carries the Override above, the same values
                // `applyParams` has just written.
                c.kind = ControlEvent::Kind::BassSlot;
                c.value = -1.0f;
                controls->push_back(c);
            }
        }

        // Phrase figures, likelier as the track goes on.
        Rng phrase;
        phrase.seed(mixSeed(seed_ ^ kSaltPhrase, (static_cast<uint64_t>(ti) << 32) | static_cast<uint64_t>(inTrack / 4)));
        const float figureChance = bassVar * (0.6f + 0.4f * progress);
        const bool varyBar2 = phrase.uniform() < figureChance * 0.6f;
        const bool varyBar4 = phrase.uniform() < figureChance;
        static const int kFigureDegrees[7][3] = { { 0, 0, 0 }, { 0, 0, 4 }, { 0, 4, 0 }, { 0, 1, 1 }, { 6, 0, 0 }, { 0, 2, 0 }, { 4, 0, 0 } };
        static const int kFigureOctaves[7][3] = { { 0, 0, 1 }, { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, 0 }, { -1, 0, 0 }, { 0, 0, 0 }, { 0, 1, 0 } };
        const int figure2 = phrase.below(7), figure4 = phrase.below(7);
        const int barInPhrase = inTrack % 4;
        // The learned bass has a phrase of its own length (kBassPhraseBars, eight bars) and reads the
        // bar of the track with it; the figures above keep their four-bar phrase, so switching the
        // knob cannot move a single note the pattern generator places.
        const int barInBassPhrase = inTrack % kBassPhraseBars;

        const StyleProfile& style = styleProfile(static_cast<StyleId>(plan.style));
        for (int beat = 0; beat < kBeatsPerBar; ++beat) {
            const double b = barBeat + beat;
            const bool fillGap = kickPattern == 1 && inTrack % 8 == 7 && beat == 3;
            // The form decides per beat whether kick and bass play: a breakdown removes both (Solberg
            // and Dibben 2019), a pre-drop break removes them for part or all of its bar.
            if (kickPattern != 2 && !fillGap && ((bp.kickBeats >> beat) & 1) != 0 && static_cast<double>(beat) >= bp.cutBeats) {
                // The kick's roll (Form.h, BarPlan::kickRoll; 23.09.2026): in the bar the form marks, the kick
                // doubles to eighths, and in the roll's second half to sixteenths where the form asks for it,
                // the repeats a little softer than the beat so the pulse still reads.
                const int sub = bp.kickRoll >= 2 && beat >= 2 ? 4 : (bp.kickRoll >= 1 ? 2 : 1);
                for (int hit = 0; hit < sub; ++hit) {
                    NoteEvent k;
                    k.beat = b + static_cast<double>(hit) / sub;
                    k.length = 0.25f;
                    k.part = Part::Kick;
                    k.pitch = 36;
                    k.velocity = static_cast<uint8_t>(hit == 0 ? 127 : (sub == 4 ? 104 : 112));
                    out.push_back(k);
                }
            }
            const bool bassBeat = ((bp.bassBeats >> beat) & 1) != 0 && static_cast<double>(beat) >= bp.cutBeats
                               && !(bassBreak && inTrack % 16 == 15 && beat >= 2);
            // How many notes this beat carries and where the first one is. With a pattern family the
            // answer is the family's; with a drawn rhythm it is the mask's, and it is a different
            // answer on every beat -- which is the whole reason the first slot has to travel to the
            // engine as an event of its own (Score.h, ControlEvent::Kind::BassSlot).
            int onsets = pat.count;
            int step[3] = { 0, 0, 0 };
            if (plan.bassRhythm) {
                onsets = 0;
                for (int k = 1; k < 4; ++k) if ((mask >> (beat * 4 + k)) & 1u) step[onsets++] = k;
            }
            if (plan.bassRhythm && controls != nullptr) {
                // One event per beat, at the beat, whether or not the beat plays. Unconditional,
                // because a bar composed on its own has to carry everything the engine needs for it:
                // an event pushed only on a change would leave a window that starts mid-track with the
                // slot of whatever was rendered before it. A beat with no bass note reports a quarter
                // beat -- the tightest slot the sixteenth grid can produce -- so that a resting bass
                // never lets the kick ring longer than it does in the beats around it, and so that the
                // limit is never looser than the Rolling family's.
                ControlEvent c;
                c.beat = b;
                c.kind = ControlEvent::Kind::BassSlot;
                c.value = bassBeat && onsets > 0 ? 0.25f * static_cast<float>(step[0]) : 0.25f;
                controls->push_back(c);
            }
            if (!bassBeat) continue;

            int figure = -1;
            // With the learned bass the phrase figures are gone: they are the pattern generator's one
            // source of pitch variation, and the model is there to replace exactly that. The *group*
            // figure stays in both modes, because it is not a pitch idea but a form rule -- it is what
            // makes two consecutive eight-bar groups of a core differ (Form.cpp, kGroupFigures), and
            // the learned phrase repeats every kBassPhraseBars bars and could not do that by itself.
            if (!plan.bassNeural && beat == 3 && barInPhrase == 1 && varyBar2) figure = figure2;
            if (!plan.bassNeural && beat == 3 && barInPhrase == 3 && varyBar4) figure = figure4;
            // The group's own change: every eight-bar group of a core ends on a figure that differs
            // from the previous group's, so two consecutive groups can never be the same bars.
            if (beat == 1 && bp.groupFigure >= 0) figure = bp.groupFigure;
            for (int s = 0; s < onsets; ++s) {
                // Where the note starts and what ends it. The pattern families read their own table;
                // a drawn bar reads its mask, and its note ends at the next onset or at the next kick,
                // whichever comes first (Corpus.h, noteSpan) -- the same rule the families follow,
                // where the last note of a beat ends at 1.0.
                const double pos = plan.bassRhythm ? 0.25 * step[s] : pat.pos[s];
                const double next = plan.bassRhythm
                    ? 0.25 * (step[s] + BassRhythm::noteSpan(mask, beat * 4 + step[s]))
                    : (s + 1 < pat.count ? pat.pos[s + 1] : 1.0);
                int pitch = root;
                // The learned phrase (Composer.h): the interval of this slot from the bass root. It is
                // read against `baseRoot` and not against `root`, because the alphabet of the model is
                // intervals to the *tonic* -- adding a chord degree on top would carry a learned
                // interval out of the scale. compose.bass_follows_chords therefore has no effect on a
                // learned bass, which the self test checks and docs/PLAN.md 6.9 records as a limit.
                if (plan.bassNeural)
                    pitch = baseRoot + plan.bassRel[secondary ? 1 : 0]
                                             [(barInBassPhrase * kBeatsPerBar + beat) * 3 + s];
                if (figure >= 0) {
                    const int idx = 3 - onsets + s;
                    // Figure degrees count from the chord's degree, so they stay in the scale when the bass follows the chords.
                    pitch = root + scaleDegree(plan.scale, chordDeg + kFigureDegrees[figure][idx]) - scaleDegree(plan.scale, chordDeg)
                          + 12 * kFigureOctaves[figure][idx];
                }
                // The bass slot envelope of the style profile (PLAN 6.6): flat by default, because nine
                // reference tracks play three equally loud notes within +-1.2 dB.
                const int slot = std::min(s, kBassSlots - 1);
                NoteEvent n;
                n.beat = b + pos;
                n.length = static_cast<float>((next - pos) * plan.gate * style.slotGate[slot]);
                n.part = Part::Bass;
                n.pitch = static_cast<uint8_t>(std::clamp(pitch, 0, 127));
                n.velocity = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(110.0f * style.slotVel[slot])), 1, 127));
                out.push_back(n);
            }
        }
        PercBarSpec spec;
        spec.layers = bp.percLayers;
        spec.fills = bp.fills;
        spec.hatsDense = bp.hatsDense;
        spec.rollBar = bp.rollBar;
        spec.rollBars = bp.rollBars;
        spec.pdb = bp.pdb;
        spec.cutBeats = bp.cutBeats;
        spec.crash = bp.crash || (bp.barInSection == 0 && bp.type == SectionType::Drop);
        spec.quietHats = bp.quietHats;
        spec.shaker = bp.shaker;
        spec.offbeatHat = bp.offbeatHat;
        spec.hatLevel = bp.hatLevel;
        spec.openHats = bp.openHats;
        spec.ride = bp.ride;
        spec.cycleBar = bp.cycleBar;
        composePercBar(p, plan.perc, plan.percSeed, bar, inTrack, plan.bpm, plan.key, plan.scale, spec, out);
        composeMelodyBar(p, plan.melody, bar, inTrack, plan.scale, bp, out, melodyContext(plan, bp, inTrack));
        composeSfxBar(plan.form, static_cast<double>(plan.firstBar) * kBeatsPerBar, inTrack, out);
        if (incoming >= 0) {
            // The percussion shares the kit's lanes: where both tracks hit one lane at the same moment the
            // outgoing track's hit stands (transitionBar).
            std::vector<NoteEvent> guest;
            transitionBar(p, incoming, bar, guest, controls);
            for (const NoteEvent& n : guest) {
                bool taken = false;
                for (size_t k = barNotes; k < out.size() && !taken; ++k)
                    taken = out[k].part == Part::Perc && n.part == Part::Perc && out[k].lane == n.lane && std::fabs(out[k].beat - n.beat) < 1e-6;
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
        auto beatLess = [](const ControlEvent& a, const ControlEvent& b) { return a.beat < b.beat; };
        std::stable_sort(controls->begin() + static_cast<long>(ctlStart), controls->end(), beatLess);
        std::inplace_merge(controls->begin(), controls->begin() + static_cast<long>(ctlStart), controls->end(), beatLess);
    }
}

Conductor::Conductor(Engine& engine, const Composer& composer) : engine_(engine), composer_(composer) {}

void Conductor::rewind()
{
    nextBar_ = 0;
    notes_.clear();
    controls_.clear();
    notePos_ = controlPos_ = 0;
}

void Conductor::pump(const ParamStore& params, double horizonBeats, std::vector<NoteEvent>* record)
{
    const double target = engine_.beatPosition() + horizonBeats;
    for (;;) {
        // sectionAutomation (Form.h, 18.09.2026) can date a control event many bars into its section's
        // future, all pushed in the one bar that opens the section -- composeBars keeps everything it
        // has produced so far sorted (see the merge there), but a bar this Conductor has not composed
        // yet can still turn out to owe an event of its own an EARLIER beat than one already sitting in
        // the buffer from a section-opening bar behind it. The one point before which nothing still to
        // come can ever land is the start of that next, uncomposed bar, so only events strictly before
        // it may be handed to the engine: engine_.pushControl feeds a queue the engine drains assuming a
        // non-decreasing stream, and one early arrival stalls everything queued behind it until its own
        // beat finally comes around -- measured as the kick losing its lock to a freely drawn bass by
        // tens of degrees (testBassRhythm, "the kick phase still meets the bass...").
        const double safeBeat = static_cast<double>(nextBar_) * kBeatsPerBar;
        while (controlPos_ < controls_.size() && controls_[controlPos_].beat < safeBeat) {
            if (!engine_.pushControl(controls_[controlPos_])) return;
            ++controlPos_;
        }
        while (notePos_ < notes_.size() && notes_[notePos_].beat < safeBeat) {
            if (!engine_.pushEvent(notes_[notePos_])) return;
            if (record != nullptr) record->push_back(notes_[notePos_]);
            ++notePos_;
        }
        // Reclaim the drained prefix so a long render does not grow these buffers without bound; the
        // threshold is well above a bar's usual handful of events, so this runs rarely.
        if (controlPos_ > 256) { controls_.erase(controls_.begin(), controls_.begin() + static_cast<long>(controlPos_)); controlPos_ = 0; }
        if (notePos_ > 256) { notes_.erase(notes_.begin(), notes_.begin() + static_cast<long>(notePos_)); notePos_ = 0; }
        if (safeBeat >= target) return;
        composer_.composeBars(params, nextBar_, 1, notes_, &controls_);
        ++nextBar_;
    }
}

} // namespace phos
