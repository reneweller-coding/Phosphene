/**
 * @file Composer.cpp
 * @brief Track plans, sound recipes, bars of kick and bass, and the conductor.
 */
#include "phos/Composer.h"
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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace phos {

const char* const kKickMacroNames[kNumKickMacros] = { "length", "punch", "body", "grit", "click" };
const char* const kBassMacroNames[kNumBassMacros] = { "brightness", "pluck", "squelch", "grit", "weight" };
const char* const kLockUnitNames[kNumLockUnits] = { "set", "track", "section", "lane" };
const char* const kAcidVoicingNames[kNumAcidVoicings] = { "clean", "driven", "liquid" };
const char* const kVoiceMacroNames[kNumVoiceMacros] = { "brightness", "softness", "thickness", "space", "motion" };

namespace {

constexpr uint64_t kSaltTrack  = 0x545241434B000001ull;
constexpr uint64_t kSaltRecipe = 0x5245434950450002ull;
constexpr uint64_t kSaltBlock  = 0x424C4F434B000003ull;
constexpr uint64_t kSaltPhrase = 0x5048524153450004ull;
constexpr uint64_t kSaltArc    = 0x4152430000000005ull;
constexpr uint64_t kSaltPerc   = 0x5045524300000006ull;
constexpr uint64_t kSaltMelody = 0x4D454C4F44590007ull;
constexpr uint64_t kSaltWalk   = 0x57414C4B00000008ull;
constexpr uint64_t kSaltFormU  = 0x464F524D55000009ull;
constexpr uint64_t kSaltSectU  = 0x5345435455000010ull;
constexpr uint64_t kSaltLaneU  = 0x4C414E4555000011ull;
constexpr uint64_t kSaltReroll = 0x5245524F4C4C0012ull;

/** @brief One move of a perceptual direction: parameter (module table index), direction, weight. */
struct Loading { int param; int macro; float weight; };

// Kick: length, punch, body, grit, click. Weights in normalised knob units at full variation.
//
// Widened 19.09.2026 where the reference kicks spread (Tools/ref_kick.py, 24 recordings): the click
// band against the body has quartiles -31 .. -23 dB, eight decibels, where the old click weights moved
// it by about two; the length direction is *narrower* than before, because the reference kicks hardly
// spread there (body 101 .. 108 ms between the quartiles) and a short recipe took the track's low end
// with it (track 2 of the listening seed: 55 ms of body, the drop 3 dB lighter under its lead and arp);
// and the body direction now moves the end
// pitch, whose reference quartiles are 54 .. 71 Hz -- with Tune = Key the end pitch still lands on the
// key's root or fifth, so a recipe changes *which* of the two, never the tuning.
const Loading kKickLoadings[] = {
    { kick::AmpHold,    0,  0.10f }, { kick::AmpDecay,   0,  0.15f }, { kick::PitchDecay, 0,  0.10f },
    { kick::Punch,      1,  0.40f }, { kick::PunchDecay, 1, -0.25f }, { kick::PitchStart, 1,  0.25f },
    { kick::PitchDecay, 2,  0.30f }, { kick::Tone,       2,  0.15f }, { kick::PitchStart, 2, -0.10f }, { kick::PitchEnd, 2, 0.25f },
    { kick::Drive,      3,  0.45f }, { kick::ClickLevel, 3,  0.10f }, { kick::Level,      3, -0.05f },
    { kick::ClickLevel, 4,  0.75f }, { kick::ClickTone,  4,  0.40f }, { kick::ClickDecay, 4,  0.20f },
};
// Bass: brightness, pluck, squelch, grit, weight -- since 19.09.2026 the four bass characters of the
// round's brief as directions of one space rather than four presets: "clean sub + bite" is the centre
// and positive weight, "gritty/overdriven" is grit (both drives and the pulse), "rubbery/resonant" is
// squelch (the ladder's and the bite's resonance, a longer filter decay), "plucky/short" is pluck (every
// decay shorter, less sustain). The weights are three to four times the old ones: at the default Sound
// Variation of 0.5 and a typical draw of 0.4 the old table moved no knob by more than 0.06 of its range,
// which renders a different number and the same sound.
const Loading kBassLoadings[] = {
    { bass::Cutoff,      0,  0.25f }, { bass::EnvAmount,   0,  0.20f }, { bass::BiteCutoff, 0,  0.60f }, { bass::BiteEnv, 0, 0.35f },
    { bass::FilterDecay, 1, -0.40f }, { bass::AmpDecay,    1, -0.50f }, { bass::AmpSustain, 1, -0.60f }, { bass::BiteDecay, 1, -0.70f },
    { bass::Resonance,   2,  0.70f }, { bass::BiteResonance, 2, 0.90f }, { bass::FilterDecay, 2, 0.15f },
    { bass::Drive,       3,  0.60f }, { bass::BiteDrive,   3,  0.80f }, { bass::Wave,       3,  0.40f }, { bass::PulseWidth, 3, 0.10f },
    { bass::Bite,        3,  0.30f }, { bass::Level,       3, -0.03f },
    { bass::Sub,         4,  0.30f }, { bass::SubOctave,   4,  0.25f }, { bass::Bite,       4, -0.40f }, { bass::SplitRatio, 4, 0.15f },
    { bass::KeyTrack,    4, -0.10f },
};

/** @brief Salt of the acid voicing draw: its own, so no other draw of the walk moves. */
constexpr uint64_t kSaltAcidVoice = 0x4143494456434500ull;

/**
 * @brief One parameter of the acid voicings: its value in the clean and the liquid voicing.
 *
 * The driven voicing is the parameter table's default (Params.cpp, 18.09.2026), so it needs no column.
 * The values are the candidates the user heard on 18.09.2026 (docs/PLAN.md, "Fundament und Mix",
 * `A_acid_1_clean303` and `A_acid_3_liquid`), minus what a track cannot own:
 *  - **Resonance and Decay** are ridden by the section (Form.cpp, `sectionAutomation`). Until 19.09.2026
 *    the ride's targets were relative to the knob and a voicing could not own them; since then the ride
 *    is an excursion around the voiced value, which sectionControls hands it, so they are in the table.
 *  - **Level** is left to the level match, which probes each part alone with the track's sound and
 *    corrects it against the first track (Composer.h, "Level match").
 *  - **Hall Send** is the section's (the breakdown opens it).
 * Cutoff and Env Amount are also written per section (`sectionControls`); there the voicing's offset is
 * added to the section's own base, so it survives.
 */
struct AcidVoicingParam { int param; float clean, liquid; };
const AcidVoicingParam kAcidVoicingTable[] = {
    { acid::Wave,          0.0f,   0.5f },
    { acid::Cutoff,      600.0f, 450.0f },
    // 19.09.2026 (round "voices"): resonance and decay joined, now that the section ride swings around
    // the voiced value instead of the knob (Form.cpp, sectionAutomation). clean303 was rendered at a
    // resonance of 0.8, liquid at 0.88 with a 500 ms decay (docs/PLAN.md, "Fundament und Mix").
    { acid::Resonance,     0.80f,  0.88f },
    { acid::Decay,       220.0f, 500.0f },
    { acid::EnvAmount,     4.0f,   5.0f },
    { acid::Accent,        0.8f,   0.6f },
    { acid::Drive,         0.1f,   0.4f },
    { acid::LowCut,      150.0f, 150.0f },
    { acid::DelaySend,     0.3f,  0.35f },
    { acid::DelayFeedback, 0.45f, 0.55f },
    { acid::DisperseFreq, 1250.0f, 1500.0f },
};
/** @brief Disperser stages of the liquid voicing (a discrete parameter, written as an override). */
constexpr float kLiquidDisperse = 4.0f;

/** @brief Salt of the voice recipes (19.09.2026): their own generator, so no other draw of the walk moves. */
constexpr uint64_t kSaltVoice = 0x564F494345520014ull;
/** @brief Salt of the drone's slow evolution (19.09.2026). */
constexpr uint64_t kSaltDroneRide = 0x44524944450015ull;

/**
 * @brief What a polyphonic voice may become in a track: its oscillators, wavetables and filter responses,
 *        each with a weight (19.09.2026, round "voices").
 *
 * The tables are indices of the `table` choice (Params.cpp): 0 Classic, 1 Vocal, 2 Glass, 3 PWM, 4 Sync,
 * 5 Formant Saw -- the built-ins -- and from 6 the library tables of `Core/data/library.phoswt`, chosen by
 * `Tools/wt_select.py` per lane from the 2191 licence-clean tables of the Noctuary library
 * (`Core/data/CREDITS-wavetables.md`): 6 .. 10 the pad lane (Hyperbol, Sampled 210, Sohler52, Organ 034,
 * Otmorph 069), 11 .. 14 the lead lane (Hienharm, Junox_ho, Euclidea, Sohler49), 15 .. 17 the arp lane
 * (Consonant 129, AKWF hollow, Pd104). Each voice draws from the lane that was measured for its role
 * plus the built-ins whose character fits it: the counter-lead the vocal and formant tables the user's
 * inventory names, the drone the organ and the measured, slow tables. The high pass is never a
 * response a recipe picks: every voice has its tracking high pass already.
 */
struct VoicePalette {
    double osc[static_cast<int>(PolyOsc::Count)];      ///< weight per PolyOsc
    int    tables[8];                                  ///< candidate tables (-1 ends the list)
    double filter[static_cast<int>(PolyFilter::Count)];///< weight per PolyFilter
    float  scale[kNumVoiceMacros];                     ///< how far each direction reaches for this voice
};
//                                     Supersaw VA   FM   WT      tables                                    LP    BP    HP   Notch   bright soft thick space motion
const VoicePalette kVoicePalette[kPolyInstances] = {
    /* lead    */ { { 0.45, 0.15, 0.15, 0.25 }, { 11, 12, 13, 14, 4, 5, -1, -1 },  { 0.80, 0.10, 0.0, 0.10 }, { 1.0f, 0.6f, 1.0f, 1.0f, 0.8f } },
    /* counter */ { { 0.00, 0.20, 0.20, 0.60 }, { 5, 1, 2, 11, 13, 15, -1, -1 },   { 0.50, 0.35, 0.0, 0.15 }, { 1.0f, 0.8f, 0.8f, 1.0f, 1.0f } },
    /* arp     */ { { 0.35, 0.30, 0.10, 0.25 }, { 15, 16, 17, 2, 3, -1, -1, -1 },  { 0.75, 0.25, 0.0, 0.00 }, { 1.0f, 0.0f, 0.8f, 1.0f, 0.6f } },
    /* stab    */ { { 0.45, 0.25, 0.00, 0.30 }, { 3, 4, 6, 8, 15, 16, -1, -1 },    { 0.70, 0.30, 0.0, 0.00 }, { 1.0f, 0.0f, 1.0f, 1.0f, 0.5f } },
    /* pad     */ { { 0.25, 0.00, 0.00, 0.75 }, { 1, 6, 7, 8, 9, 10, 2, -1 },      { 0.85, 0.00, 0.0, 0.15 }, { 0.8f, 1.0f, 1.0f, 1.0f, 1.0f } },
    /* drone   */ { { 0.00, 0.30, 0.00, 0.70 }, { 9, 7, 10, 6, 1, -1, -1, -1 },    { 0.90, 0.10, 0.0, 0.00 }, { 0.7f, 0.6f, 1.0f, 0.8f, 1.0f } },
};

/**
 * @brief How the five directions of a voice recipe move the knobs: parameter, direction, weight in
 *        normalised knob units at full variation (Composer.h, VoiceRecipe).
 *
 * brightness opens the filter, its envelope and the table position; softness lengthens the attack, the
 * filter's decay and the release (the arp and the stab take none of it: their shortness is rule 15 and
 * the stab's whole point); thickness widens the unison and the stereo image; space sends more into the
 * delay and its feedback (the hall is the section's, see sectionControls); motion deepens the table LFO,
 * the position envelope and the thermal drift. The weights are design values: large enough that the
 * spread over twenty tracks is measurable (self test, testVoiceSpread), small enough that a voice at
 * the end of a direction is still the voice it was; the listening excerpts are where they get judged.
 */
const Loading kVoiceLoadings[] = {
    { poly::Cutoff,      0, 0.18f }, { poly::EnvAmount,   0, 0.10f }, { poly::Resonance, 0, 0.08f }, { poly::Position, 0, 0.20f },
    { poly::AmpAttack,   1, 0.15f }, { poly::FilterDecay, 1, 0.15f }, { poly::AmpRelease, 1, 0.10f },
    { poly::Detune,      2, 0.25f }, { poly::Mix,         2, 0.15f }, { poly::Width,    2, 0.20f },
    { poly::DelaySend,   3, 0.20f }, { poly::DelayFeedback, 3, 0.10f },
    { poly::PosLfoDepth, 4, 0.30f }, { poly::PosEnv,      4, 0.20f }, { poly::Drift,    4, 0.15f },
};
/** @brief The hall-send share of the space direction, folded into the section's hall ride (sectionControls). */
constexpr float kVoiceHallWeight = 0.12f;

/** @brief Bass parameters that move in slow arcs within a track, with their arc size at full variation. */
struct Arc { int param; float size; };
const Arc kBassArcs[] = { { bass::Cutoff, 0.08f }, { bass::EnvAmount, 0.06f }, { bass::Resonance, 0.06f } };

/** @brief A standard normal draw (Box-Muller). */
double normal(Rng& r)
{
    const double u1 = 1.0 - static_cast<double>(r.uniform());
    const double u2 = static_cast<double>(r.uniform());
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.141592653589793 * u2);
}

/** @brief Draws a recipe: each direction from a normal distribution with sigma 0.5, truncated to [-1, 1]. */
void drawRecipe(Rng& r, float* m, int n)
{
    for (int i = 0; i < n; ++i) {
        double x;
        do { x = 0.5 * normal(r); } while (x < -1.0 || x > 1.0);
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

/**
 * @brief The loudness side of Farbood's tension model: what a section's energy does to the track gain.
 *
 * At most +-2 dB around the level the track was matched to, so that the energy is audible without
 * undoing the level match between tracks.
 */
float energyGainDb(float energy)
{
    return std::clamp((energy - 0.7f) * 5.0f, -2.0f, 2.0f);
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

/** @brief Salt of the learned bass phrase: its own, so it does not move any other draw. */
constexpr uint64_t kSaltBassLine = 0x424153534C4E0013ull;

/** @brief Salt of the drawn bass rhythm: its own, for the same reason. */
constexpr uint64_t kSaltBassRhythm = 0x424153535248546Dull;

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
 * plainly; Form.h belongs to another agent this round, and the report says so.
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
    const StyleProfile& style = styleProfile(styleOf(p));
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

/** @brief Whether the form silences kick and bass for the whole of a bar (Melody.h, the sub foundation). */
static bool foundationBar(const BarPlan& bp) { return bp.kickBeats == 0 && bp.bassBeats == 0; }

/** @name The pad's high pass while it lays the sub foundation (rule 20 of 18.09.2026). @{ */
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

static PartAvailability availabilityOf(const TrackPlan& plan);

/** @brief One bar as the drone sees it: whether it plays and whether in its low octave. */
struct DroneBar { bool on = false, low = false; int section = -1; };

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
    // In the tail of a silent floor the drone would move to its upper octave -- the pad's root and fifth,
    // the acid's octave. Where either of them plays it leaves instead (Form.cpp gives the upper octave only
    // to bars without them), and its low note's release is the fade.
    if (!d.low && (bp.parts & (partBit(MelodyPart::Pad) | partBit(MelodyPart::Acid))) != 0) d.on = false;
    // The same for the first sixteen bars of a track after the first: the previous track's pads may still
    // hold there (transitionBar), in the octave the upper drone would take.
    if (!d.low && plan.index > 0 && inTrack < 16) d.on = false;
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
static MelodyContext melodyContext(const TrackPlan& plan, const BarPlan& bp, int inTrack)
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
        && inTrack % plan.melody.chordBars == 0 && !(droneHere && here.low)) {
        int k = 1;
        while (inTrack + k < plan.bars && k < 64 && foundationBar(planBar(plan.form, a, plan.sectionSeed, inTrack + k))) ++k;
        c.foundationBars = k;
    }
    if (droneHere && here.on) {
        const DroneBar before = droneBarAt(plan, a, inTrack - 1);
        const bool start = !before.on || before.section != here.section || before.low != here.low;
        if (start) {
            int k = 1;
            while (k < 64) {
                const DroneBar next = droneBarAt(plan, a, inTrack + k);
                if (!next.on || next.section != here.section || next.low != here.low) break;
                ++k;
            }
            c.droneBars = k;
            c.droneLow = here.low;
            c.droneTail = 0;
        }
    }
    return c;
}

/** @brief Whether the drone plays bar @p inTrack in its low octave (the high pass is opened for it). */
static bool droneLowAt(const TrackPlan& plan, int inTrack)
{
    if (!plan.melody.present[mpIndex(MelodyPart::Drone)]) return false;
    return droneBarAt(plan, availabilityOf(plan), inTrack).low;
}

static PartAvailability availabilityOf(const TrackPlan& plan)
{
    PartAvailability a;
    for (int k = 0; k < kMelodyParts; ++k) a.part[k] = plan.melody.present[k];
    a.leadLo = plan.melody.leadLo;
    a.leadHi = plan.melody.leadHi;
    a.arpLo = plan.melody.arpLo;
    a.arpHi = plan.melody.arpHi;
    a.percLayers = plan.perc.layers;
    return a;
}

void Composer::setLock(LockUnit unit, int index, bool locked)
{
    if (locked) locked_[static_cast<int>(unit)][index] = 1;
    else locked_[static_cast<int>(unit)].erase(index);
    plans_.clear();
    walk_.clear();
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
}

void Composer::clearLocks()
{
    for (int u = 0; u < kNumLockUnits; ++u) { locked_[u].clear(); variation_[u].clear(); }
    plans_.clear();
    walk_.clear();
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

void Composer::recipeOffsets(bool kickModule, const float* macros, float amount, float* out)
{
    const int n = ParamStore::moduleCount(kickModule ? Module::Kick : Module::Bass);
    std::fill(out, out + n, 0.0f);
    const Loading* table = kickModule ? kKickLoadings : kBassLoadings;
    const size_t count = kickModule ? sizeof(kKickLoadings) / sizeof(Loading) : sizeof(kBassLoadings) / sizeof(Loading);
    for (size_t i = 0; i < count; ++i) out[table[i].param] += amount * table[i].weight * macros[table[i].macro];
}

void Composer::acidVoicingOffsets(const ParamStore& p, const float* weights, float amount, float* out, int& disperse)
{
    const int ab = p.base(Module::Acid);
    std::fill(out, out + acid::Count, 0.0f);
    disperse = -1;
    const float reach = std::clamp(2.0f * amount, 0.0f, 1.0f);
    if (reach <= 0.0f) return;
    for (const AcidVoicingParam& v : kAcidVoicingTable) {
        const int id = ab + v.param;
        const float d = p.toNormalised(id, p.defaultValue(id));
        out[v.param] = reach * (weights[0] * (p.toNormalised(id, v.clean) - d) + weights[2] * (p.toNormalised(id, v.liquid) - d));
    }
    // The disperser is a number of stages: the liquid share of the stages, rounded, on top of the knob.
    const int knob = static_cast<int>(std::lround(p.get(ab + acid::Disperse)));
    const int stages = std::min(static_cast<int>(kDisperseStages), knob + static_cast<int>(std::lround(reach * weights[2] * kLiquidDisperse)));
    disperse = stages == knob ? -1 : stages;   // no liquid share: the knob, as an override of -1 says
}

void Composer::voiceRecipeOffsets(PolyInstance voice, const VoiceRecipe& r, float amount, float* out)
{
    std::fill(out, out + poly::Count, 0.0f);
    const VoicePalette& pal = kVoicePalette[polyIndex(voice)];
    for (const Loading& l : kVoiceLoadings) out[l.param] += amount * l.weight * pal.scale[l.macro] * r.macro[l.macro];
}

void Composer::matchMaster(const ParamStore& p, TrackPlan& t) const
{
    const int ms = p.base(Module::Master);
    t.masterGainDb = 0.0f;
    if (!p.getBool(ms + master::AutoGain)) return;
    const double target = p.get(ms + master::TargetLufs);
    // Measure, correct, measure again, and take a secant step between the two readings.
    const double l0 = probeLoudness(p, t, -2, 0.0f);
    t.mixLoudness = l0;
    if (l0 < -60.0) return;
    const double g1 = std::clamp(target - l0, -18.0, 18.0);
    const double l1 = probeLoudness(p, t, -2, static_cast<float>(g1));
    const double slope = std::fabs(g1) > 0.05 ? (l1 - l0) / g1 : 1.0;
    const double g2 = g1 + (target - l1) / std::clamp(slope, 0.2, 1.0);
    t.masterGainDb = static_cast<float>(std::clamp(g2, -18.0, 18.0));
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
    for (int l = 0; l < kPercLanes; ++l) {
        const int b = p.base(Module::Perc, l);
        for (int k : { static_cast<int>(perc::Active), static_cast<int>(perc::Role), static_cast<int>(perc::Density), static_cast<int>(perc::Engine) })
            knobs.push_back(p.get(b + k));
    }
    if (knobs != planKnobs_) { plans_.clear(); walk_.clear(); planKnobs_ = std::move(knobs); }
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
    // Track lengths are multiples of 32 bars, so that every track boundary -- the bass swap of a
    // transition -- falls on a 32-bar grid (PLAN 6.7).
    const int baseBars = std::clamp((p.getInt(cb + compose::TrackBars) / 32) * 32, kMinTrackBars, kMaxTrackBars);

    while (static_cast<int>(walk_.size()) <= index) {
        const int i = static_cast<int>(walk_.size());
        TrackWalk w;
        if (i == 0) {
            w.bars = baseBars;
            w.key = p.getInt(cb + compose::Key);
            w.scale = p.getInt(cb + compose::Scale);
            w.bpm = styleTempo ? std::round(style.bpmCentre * 2.0) * 0.5 : baseBpm;
            walk_.push_back(w);
            continue;   // the first track is the knobs, exactly
        }
        const TrackWalk& prev = walk_[static_cast<size_t>(i - 1)];
        Rng r;
        r.seed(mixSeed(setSeed() ^ kSaltWalk, static_cast<uint64_t>(i)));

        // Length: the knob, give or take up to two 32-bar blocks.
        w.bars = baseBars;
        if (r.uniform() < tv) w.bars = std::clamp(baseBars + 32 * (r.below(3) - 1), kMinTrackBars, kMaxTrackBars);

        // Key: by fifths and whole tones, now and then a semitone.
        w.key = prev.key;
        if (r.uniform() < tv) {
            static const int kMoves[6] = { 7, 5, 2, -2, 1, -1 };
            static const double kWeights[6] = { 0.3, 0.3, 0.15, 0.15, 0.05, 0.05 };
            w.key = ((prev.key + kMoves[pick(r, kWeights, 6)]) % 12 + 12) % 12;
        }
        // Mode: mostly kept; when it changes, the style profile's weights decide.
        w.scale = prev.scale;
        if (r.uniform() < 0.25f * tv) w.scale = pick(r, style.scaleWeight, kNumScales);
        // Tempo: mean-reverting walk around the centre, inside the range, on half-BPM steps.
        const double walk = baseBpm + 0.6 * (prev.bpm - baseBpm) + (2.0 * r.uniform() - 1.0) * range * tv;
        w.bpm = std::round(std::clamp(walk, baseBpm - range, baseBpm + range) * 2.0) * 0.5;

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
        for (int v = 0; v < kPolyInstances; ++v) {
            const VoicePalette& pal = kVoicePalette[v];
            int nTables = 0;
            while (nTables < 8 && pal.tables[nTables] >= 0) ++nTables;
            double bestScore = -1.0;
            for (int c = 0; c < 12; ++c) {
                VoiceRecipe cand;
                cand.osc = pick(rv, pal.osc, static_cast<int>(PolyOsc::Count));
                cand.table = nTables > 0 ? pal.tables[rv.below(nTables)] : -1;
                cand.filter = pick(rv, pal.filter, static_cast<int>(PolyFilter::Count));
                static const int kLeft[4] = { 1, 2, 3, 4 }, kRight[4] = { 3, 2, 1, 4 };
                cand.delayL = kLeft[rv.below(4)];
                cand.delayR = kRight[rv.below(4)];
                drawRecipe(rv, cand.macro, kNumVoiceMacros);
                double score = 1e9;
                for (int back = 1; back <= 2 && i - back >= 0; ++back) {
                    const VoiceRecipe& o = walk_[static_cast<size_t>(i - back)].voice[v];
                    double d = 0.0;
                    for (int k = 0; k < kNumVoiceMacros; ++k) d += (cand.macro[k] - o.macro[k]) * (cand.macro[k] - o.macro[k]);
                    d = std::sqrt(d) + (cand.osc != o.osc ? 1.0 : 0.0) + (cand.table != o.table ? 1.0 : 0.0) + (cand.filter != o.filter ? 0.5 : 0.0);
                    score = std::min(score, d);
                }
                if (score > bestScore) { bestScore = score; w.voice[v] = cand; }
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
    const StyleProfile& style = styleProfile(styleOf(p));
    const TrackWalk& w = walkAt(p, index);

    TrackPlan t;
    t.index = index;
    t.firstBar = index == 0 ? 0 : plans_[static_cast<size_t>(index - 1)].firstBar + plans_[static_cast<size_t>(index - 1)].bars;
    t.key = w.key;
    t.scale = w.scale;
    t.bpm = w.bpm;
    std::copy(w.kickMacro, w.kickMacro + kNumKickMacros, t.kickMacro);
    std::copy(w.bassMacro, w.bassMacro + kNumBassMacros, t.bassMacro);
    std::copy(w.acidVoicing, w.acidVoicing + kNumAcidVoicings, t.acidVoicing);
    for (int v = 0; v < kPolyInstances; ++v) t.voice[v] = w.voice[v];

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
    t.melody = makeMelodyPlan(p, style, t.melodySeed, t.key, t.scale, index == 0, colour, t.form.scaleMask);

    if (index == 0) {
        t.primaryPattern = std::clamp(p.getInt(cb + compose::BassPattern), 0, kNumBassPatterns - 1);
        t.secondaryPattern = t.primaryPattern == 0 ? 1 : 0;
        t.gate = p.get(cb + compose::BassGate);
        // The rhythm before the pitches, because the learned phrase is drawn for the steps the rhythm
        // decides. The first track takes the gate knob as it is -- "the knobs, exactly" -- and a drawn
        // rhythm cannot make that worse: its shortest slot is a quarter beat, which is what the
        // default Rolling family already asks of the knob.
        makeBassRhythm(p, t);
        makeBassPhrases(p, t);
        if (p.getBool(cb + compose::LevelMatch)) {
            t.loudness = probeLoudness(p, t);
            for (int k = 0; k < kMelodyParts; ++k) t.partLoudness[k] = probeLoudness(p, t, k);
        }
        matchMaster(p, t);
        return t;   // the first track is the knobs, exactly
    }

    Rng r;
    r.seed(trackSeed(index));

    // Patterns.
    t.primaryPattern = std::clamp(p.getInt(cb + compose::BassPattern), 0, kNumBassPatterns - 1);
    if (r.uniform() < tv) {
        static const double kWeights[kNumBassPatterns] = { 0.45, 0.20, 0.12, 0.08, 0.15 };
        t.primaryPattern = pick(r, kWeights, kNumBassPatterns);
    }
    {
        double wp[kNumBassPatterns] = { 0.40, 0.30, 0.20, 0.05, 0.05 };
        wp[t.primaryPattern] = 0.0;
        t.secondaryPattern = pick(r, wp, kNumBassPatterns);
    }

    // The rhythm, after the patterns it strays from and before the gate limit it decides.
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

    if (p.getBool(cb + compose::LevelMatch)) {
        t.loudness = probeLoudness(p, t);
        const double reference = plans_[0].loudness;
        t.gainDb = static_cast<float>(std::clamp(reference - t.loudness, -9.0, 9.0));
        // Each melodic part against the same part in the first track; the track gain applies to it as
        // well, so it is taken back out.
        for (int k = 0; k < kMelodyParts; ++k) {
            // A part the track does not use needs no correction (the first track measures all of them:
            // it is the reference for every later one).
            if (!t.melody.present[k]) { t.partLoudness[k] = -120.0; t.partGainDb[k] = 0.0f; continue; }
            t.partLoudness[k] = probeLoudness(p, t, k);
            const bool measurable = plans_[0].partLoudness[k] > -60.0 && t.partLoudness[k] > -60.0;
            t.partGainDb[k] = measurable ? static_cast<float>(std::clamp(plans_[0].partLoudness[k] - t.partLoudness[k] - t.gainDb, -12.0, 12.0)) : 0.0f;
        }
    }
    matchMaster(p, t);
    return t;
}

void Composer::trackStartControls(const ParamStore& p, const TrackPlan& plan, double beat, std::vector<ControlEvent>& out) const
{
    const int cb = p.base(Module::Compose), kb = p.base(Module::Kick), bb = p.base(Module::Bass), mb = p.base(Module::Mix);
    const float sv = p.get(cb + compose::SoundVariation);
    auto push = [&](int id, ControlEvent::Kind kind, float value) {
        ControlEvent c;
        c.beat = beat;
        c.param = static_cast<int16_t>(id);
        c.kind = kind;
        c.value = value;
        out.push_back(c);
    };
    push(cb + compose::Key, ControlEvent::Kind::Override, static_cast<float>(plan.key));
    push(cb + compose::Scale, ControlEvent::Kind::Override, static_cast<float>(plan.scale));
    push(cb + compose::BassPattern, ControlEvent::Kind::Override, static_cast<float>(plan.primaryPattern));
    push(kb + kick::Engine, ControlEvent::Kind::Override, static_cast<float>(plan.kickEngine));
    push(kb + kick::Clip, ControlEvent::Kind::Override, static_cast<float>(plan.kickClip));
    float kickOff[64] = {};
    recipeOffsets(true, plan.kickMacro, sv, kickOff);
    for (const Loading& l : kKickLoadings) push(kb + l.param, ControlEvent::Kind::Offset, kickOff[l.param]);
    // The track's level correction, in the normalised domain of a linear 24 dB range.
    const ParamDesc& g = p.desc(mb + mix::TrackGain);
    push(mb + mix::TrackGain, ControlEvent::Kind::Offset, plan.gainDb / (g.maxValue - g.minValue));
    // Percussion: one recipe for the whole kit, so the lanes keep sounding like one kit, and the
    // track's engine and mode-set switches.
    float percOff[perc::Count] = {};
    percRecipeOffsets(plan.perc.macro, sv, percOff);
    for (int l = 0; l < kPercLanes; ++l) {
        const int pb = p.base(Module::Perc, l);
        for (int k : { static_cast<int>(perc::Cutoff), static_cast<int>(perc::MetalScale), static_cast<int>(perc::Decay),
                       static_cast<int>(perc::NoiseDecay), static_cast<int>(perc::BurstSpacing), static_cast<int>(perc::Drive),
                       static_cast<int>(perc::Resonance) })
            push(pb + k, ControlEvent::Kind::Offset, percOff[k]);
        push(pb + perc::Engine, ControlEvent::Kind::Override, static_cast<float>(plan.perc.engineOverride[l]));
        push(pb + perc::ModeSet, ControlEvent::Kind::Override, static_cast<float>(plan.perc.modeSetOverride[l]));
    }
    (void)bb;

    // Melodic parts: switches of the track, delay times, level corrections and sound directions.
    const MelodyPlan& m = plan.melody;
    const int ab = p.base(Module::Acid);
    push(ab + acid::Squelch, ControlEvent::Kind::Override, static_cast<float>(m.acidSquelch));
    push(ab + acid::DelayLeft, ControlEvent::Kind::Override, static_cast<float>(m.delay[mpIndex(MelodyPart::Acid)][0]));
    push(ab + acid::DelayRight, ControlEvent::Kind::Override, static_cast<float>(m.delay[mpIndex(MelodyPart::Acid)][1]));
    for (int k = 0; k < kMelodyParts; ++k) {
        const MelodyPart part = static_cast<MelodyPart>(k);
        const int level = mb + (part == MelodyPart::Acid ? static_cast<int>(mix::AcidLevel) : mix::polyLevel(melodyPoly(part)));
        const ParamDesc& d = p.desc(level);
        push(level, ControlEvent::Kind::Offset, plan.partGainDb[k] / (d.maxValue - d.minValue));
    }
    // The track's acid voicing (Composer.h, kNumAcidVoicings). Cutoff and Env Amount are written again
    // by every section with the voicing folded into the section's base (sectionControls); they are
    // written here as well so that the level match's part probe, which plays the track start alone,
    // hears the voicing it has to match.
    {
        float acidOff[acid::Count] = {};
        int disperse = -1;
        acidVoicingOffsets(p, plan.acidVoicing, sv, acidOff, disperse);
        for (const AcidVoicingParam& v : kAcidVoicingTable) push(ab + v.param, ControlEvent::Kind::Offset, acidOff[v.param]);
        push(ab + acid::Disperse, ControlEvent::Kind::Override, static_cast<float>(disperse));
    }
    // Decay and resonance: the voicing's offset (pushed above) plus the old per-track direction, as one
    // value -- a strand holds an offset, it does not add one; the section ride swings around this sum.
    {
        float acidOff[acid::Count] = {};
        int unused = -1;
        acidVoicingOffsets(p, plan.acidVoicing, sv, acidOff, unused);
        push(ab + acid::Decay, ControlEvent::Kind::Offset, acidOff[acid::Decay] + 0.12f * sv * m.recipe[mpIndex(MelodyPart::Acid)]);
        push(ab + acid::Resonance, ControlEvent::Kind::Offset, acidOff[acid::Resonance] + 0.08f * sv * m.recipe[mpIndex(MelodyPart::Acid)]);
    }
    // Every polyphonic voice's own sound (Composer.h, VoiceRecipe; 19.09.2026). The discrete choices
    // only where Sound Variation is on at all; the directions scaled by it. Three of the parameters are
    // ridden again later with this offset as their base -- the lead's cutoff and the pad's table
    // position by every section (sectionControls), the drone's cutoff, position and detune by its slow
    // evolution (droneControls) -- because a ride replaces an offset rather than adding to it.
    for (int v = 0; v < kPolyInstances; ++v) {
        const PolyInstance inst = static_cast<PolyInstance>(v);
        const VoiceRecipe& rc = plan.voice[v];
        const int vb = p.base(inst);
        const bool vary = sv > 0.0f;
        push(vb + poly::Osc, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.osc : -1));
        push(vb + poly::Table, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.table : -1));
        push(vb + poly::FilterType, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.filter : -1));
        push(vb + poly::DelayLeft, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.delayL : -1));
        push(vb + poly::DelayRight, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.delayR : -1));
        float off[poly::Count] = {};
        voiceRecipeOffsets(inst, rc, sv, off);
        for (const Loading& l : kVoiceLoadings) push(vb + l.param, ControlEvent::Kind::Offset, off[l.param]);
    }
    push(p.base(PolyInstance::Pad) + poly::GatePattern, ControlEvent::Kind::Override, static_cast<float>(m.padGatePattern));    // The loudness offset of Auto Gain, in the normalised domain of master.gain's 36 dB range.
    const ParamDesc& mg = p.desc(p.base(Module::Master) + master::Gain);
    push(p.base(Module::Master) + master::Gain, ControlEvent::Kind::Offset, plan.masterGainDb / (mg.maxValue - mg.minValue));
}

/**
 * @brief The control events of a section: timbre as narrative, and the energy's three audible sides.
 *
 * Farrell's thesis (Sussex 2019) reads psychedelic music as a narrative told with timbre; Farbood's
 * tension model (2012) names the quantities a listener reads as tension. What the section can change
 * without touching a note is therefore: the filter openings of acid and lead, the pad's table position
 * and the hall sends (timbre), and the track gain (loudness, at most +-2 dB so the level match stays
 * intact). Every one of them is written as a ramp over the section, so nothing steps.
 *
 * The energy of a buildup runs from its own value to the drop's, which is exactly the "filter arcs
 * opening" the plan asks for; a breakdown opens the hall and closes the filters.
 */
void Composer::sectionControls(const ParamStore& p, const TrackPlan& plan, const BarPlan& bar, double beat,
                               std::vector<ControlEvent>& out) const
{
    const int cb = p.base(Module::Compose), ab = p.base(Module::Acid), mb = p.base(Module::Mix);
    const int lb = p.base(PolyInstance::Lead), pb = p.base(PolyInstance::Pad);
    const float sv = p.get(cb + compose::SoundVariation), mv = p.get(cb + compose::MelodyVariation);
    const MelodyPlan& m = plan.melody;
    const Section& s = plan.form.section[bar.index];
    const float length = static_cast<float>(s.bars) * kBeatsPerBar;
    auto push = [&](int id, float value, float len) {
        ControlEvent c;
        c.beat = beat;
        c.param = static_cast<int16_t>(id);
        c.kind = ControlEvent::Kind::Offset;
        c.value = value;
        c.length = len;
        out.push_back(c);
    };
    auto pushNow = [&](int id, ControlEvent::Kind kind, float value) {
        ControlEvent c;
        c.beat = beat;
        c.param = static_cast<int16_t>(id);
        c.kind = kind;
        c.value = value;
        out.push_back(c);
    };

    // A cutoff arc per section: the recipe's base plus a target drawn from the section's own seed and
    // lifted by the energy. The first section of the first track starts from the knobs exactly.
    Rng r;
    r.seed(mixSeed(plan.sectionSeed[std::clamp(bar.index, 0, kMaxSections - 1)] ^ kSaltArc, 0));
    const float wobble = (2.0f * r.uniform() - 1.0f);
    // The track's acid voicing moves the cutoff and the envelope depth the section's arc and ride are
    // centred on (19.09.2026): as a base, not as a separate event, because the arc and the ride write
    // the same parameters and each event replaces the offset before it.
    float voicing[acid::Count] = {};
    int disperseUnused = -1;
    acidVoicingOffsets(p, plan.acidVoicing, sv, voicing, disperseUnused);
    const float acidBase = 0.10f * sv * m.recipe[mpIndex(MelodyPart::Acid)] + voicing[acid::Cutoff];
    // The lead's cutoff and the pad's table position ride on their voice recipe's offset (19.09.2026).
    float leadOff[poly::Count] = {}, padOff[poly::Count] = {};
    voiceRecipeOffsets(PolyInstance::Lead, plan.voice[polyIndex(PolyInstance::Lead)], sv, leadOff);
    voiceRecipeOffsets(PolyInstance::Pad, plan.voice[polyIndex(PolyInstance::Pad)], sv, padOff);
    const float leadBase = 0.10f * sv * m.recipe[mpIndex(MelodyPart::Lead)] + leadOff[poly::Cutoff];
    const float padBase = 0.25f * sv * m.recipe[mpIndex(MelodyPart::Pad)] + padOff[poly::Position];
    const bool knobs = plan.index == 0 && bar.index == 0;
    const float e0 = s.energy, e1 = s.energyTo;
    auto cutoffAt = [&](float base, float scale, float energy) {
        return knobs ? 0.0f : base + scale * (0.35f * (energy - 0.7f) + 0.12f * mv * wobble);
    };
    if (bar.index == 0) {
        push(ab + acid::Cutoff, cutoffAt(acidBase, 1.0f, e0), 0.0f);
        push(lb + poly::Cutoff, cutoffAt(leadBase, 0.8f, e0), 0.0f);
        push(pb + poly::Position, cutoffAt(padBase, 0.6f, e0), 0.0f);
    }
    push(ab + acid::Cutoff, cutoffAt(acidBase, 1.0f, e1), length);
    push(ab + acid::EnvAmount, 0.5f * (cutoffAt(acidBase, 1.0f, e1) - acidBase) + voicing[acid::EnvAmount], length);
    push(lb + poly::Cutoff, cutoffAt(leadBase, 0.8f, e1), length);
    push(pb + poly::Position, cutoffAt(padBase, 0.6f, e1), length);

    // The hall opens where the floor empties: a breakdown is the wettest part of a track.
    const float wet = s.type == SectionType::Break ? 0.22f : (s.type == SectionType::Intro || s.type == SectionType::Outro ? 0.10f : 0.0f);
    const ParamDesc& hs = p.desc(pb + poly::HallSend);
    const float wetNorm = wet / (hs.maxValue - hs.minValue);
    push(ab + acid::HallSend, knobs ? 0.0f : wetNorm, length);
    // Every polyphonic voice, with the hall share of its recipe's space direction folded in (19.09.2026).
    for (int v = 0; v < kPolyInstances; ++v) {
        const float space = kVoiceHallWeight * sv * kVoicePalette[v].scale[3] * plan.voice[v].macro[3];
        push(p.base(static_cast<PolyInstance>(v)) + poly::HallSend, knobs ? 0.0f : wetNorm + space, length);
    }

    // Loudness (Farbood): at most +-2 dB around the section's energy, on top of the track's level match.
    const ParamDesc& g = p.desc(mb + mix::TrackGain);
    const float span = g.maxValue - g.minValue;
    push(mb + mix::TrackGain, (plan.gainDb + (knobs ? 0.0f : energyGainDb(e0))) / span, 0.0f);
    // A buildup's energy runs from the section before it to the drop, so the gain ramps with it; every
    // other section holds one value (e0 == e1 there).
    if (e1 != e0) push(mb + mix::TrackGain, (plan.gainDb + (knobs ? 0.0f : energyGainDb(e1))) / span, length);

    // The macro ride of this section and the buildup's hall send (Form.h, 16.09.2026). The only line
    // of this file the arrangement-dynamics round of 16.09.2026 added: everything it writes is made
    // in Form.cpp out of the section, its seed and the arc values computed just above.
    // Since 19.09.2026 the ride of resonance and decay swings around the track's voiced values (Form.cpp):
    // the voicing's offset and the old per-track direction, the same sum trackStartControls writes.
    const float acidRecipe = m.recipe[mpIndex(MelodyPart::Acid)];
    sectionAutomation(p, s, plan.sectionSeed[std::clamp(bar.index, 0, kMaxSections - 1)], beat,
                      cutoffAt(acidBase, 1.0f, e0), cutoffAt(acidBase, 1.0f, e1), knobs, out,
                      voicing[acid::Resonance] + 0.08f * sv * acidRecipe, voicing[acid::Decay] + 0.12f * sv * acidRecipe);

    // The pad's trance gate is a property of the section, not of a 16-bar block.
    pushNow(pb + poly::Gate, ControlEvent::Kind::Override, bar.padGate ? 1.0f : 0.0f);
}

void Composer::droneControls(const ParamStore& p, const TrackPlan& plan, int inTrack, double beat, std::vector<ControlEvent>& out) const
{
    const int period = std::max(1, plan.melody.droneEvolveBars);
    if (inTrack % period != 0) return;
    const float sv = p.get(p.base(Module::Compose) + compose::SoundVariation);
    float base[poly::Count] = {};
    voiceRecipeOffsets(PolyInstance::Drone, plan.voice[polyIndex(PolyInstance::Drone)], sv, base);
    Rng r;
    r.seed(mixSeed(plan.melodySeed ^ kSaltDroneRide, static_cast<uint64_t>(inTrack / period)));
    const int db = p.base(PolyInstance::Drone);
    // How far the drone wanders, in normalised knob units: the filter by up to 1.4 octaves of its
    // 6.5-octave range, the table position by a quarter, the unison detune by a fifth -- the "slow
    // evolution (filter, wavetable position, detune drift)" of the brief, one target per period.
    struct Move { int param; float reach; };
    static const Move kMoves[3] = { { poly::Cutoff, 0.22f }, { poly::Position, 0.25f }, { poly::Detune, 0.20f } };
    for (const Move& m : kMoves) {
        ControlEvent c;
        c.beat = beat;
        c.kind = ControlEvent::Kind::Offset;
        c.param = static_cast<int16_t>(db + m.param);
        c.value = base[m.param] + m.reach * (2.0f * r.uniform() - 1.0f);
        c.length = static_cast<float>(period * kBeatsPerBar);
        out.push_back(c);
    }
}

void Composer::arcControls(const ParamStore& p, const TrackPlan& plan, int inTrack, double beat, bool ramp, std::vector<ControlEvent>& out) const
{
    const int cb = p.base(Module::Compose), bb = p.base(Module::Bass);
    const float sv = p.get(cb + compose::SoundVariation);
    float bassOff[64] = {};
    recipeOffsets(false, plan.bassMacro, sv, bassOff);
    Rng arc;
    arc.seed(mixSeed(seed_ ^ kSaltArc, (static_cast<uint64_t>(plan.index) << 32) | static_cast<uint64_t>(inTrack / 32)));
    const bool arcsOn = plan.index > 0 || inTrack > 0;
    for (const Arc& a : kBassArcs) bassOff[a.param] += arcsOn ? (2.0f * arc.uniform() - 1.0f) * a.size * sv : 0.0f;
    auto push = [&](int local, float length) {
        ControlEvent c;
        c.beat = beat;
        c.param = static_cast<int16_t>(bb + local);
        c.kind = ControlEvent::Kind::Offset;
        c.value = bassOff[local];
        c.length = length;
        out.push_back(c);
    };
    if (!ramp) {
        for (const Loading& l : kBassLoadings) push(l.param, 0.0f);
    } else {
        // A slow arc: the bass's brightness and squelch travel over the next 32 bars.
        for (const Arc& a : kBassArcs) push(a.param, 32.0f * kBeatsPerBar);
    }
}

double Composer::probeLoudness(const ParamStore& p, const TrackPlan& plan, int part, float masterGainDb) const
{
    constexpr double sr = 48000.0;
    // Two bars for the foundation and the parts. For the whole mix eight bars spread evenly over the track,
    // as its blocks really play -- intro and outro included, since the gated integrated loudness counts
    // them: the densest block alone over-read a track by 0.8 LU, four bars from its middle by 0.6 LU.
    const int bars = part == -2 ? 16 : 2;
    auto engine = std::make_unique<Engine>();
    engine->prepare(sr, 512);
    engine->params().copyValuesFrom(p);
    const int mb = p.base(Module::Mix);
    engine->params().set(mb + mix::KickMute, 0.0f);
    engine->params().set(mb + mix::BassMute, 0.0f);
    engine->params().set(mb + mix::TrackGain, 0.0f);
    engine->params().set(mb + mix::PercMute, 0.0f);
    // The foundation (part -1): kick, bass and percussion. A melodic part: that part alone. The whole mix
    // (part -2): everything with its corrections, through the master. The first two are measured before
    // the master's dynamics, which would bend the relation between gain and loudness.
    for (int k = 0; k < kMelodyParts; ++k) {
        const MelodyPart mp = static_cast<MelodyPart>(k);
        const int mute = mp == MelodyPart::Acid ? static_cast<int>(mix::AcidMute) : mix::polyMute(melodyPoly(mp));
        engine->params().set(mb + mute, (k == part || part == -2) ? 0.0f : 1.0f);
    }
    engine->params().set(mb + mix::SfxMute, 1.0f);
    const int ms = p.base(Module::Master);
    if (part != -2) {
        engine->params().set(ms + master::CompRatio, 1.0f);
        engine->params().set(ms + master::Limiter, 0.0f);
        engine->params().set(ms + master::Clip, 0.0f);
        engine->params().set(ms + master::Clipper, 0.0f);
    }
    if (part >= 0) {
        engine->params().set(mb + mix::KickMute, 1.0f);
        engine->params().set(mb + mix::BassMute, 1.0f);
        engine->params().set(mb + mix::PercMute, 1.0f);
    }
    TempoMap tm;
    tm.setConstant(plan.bpm);
    engine->setTempoMap(tm);

    std::vector<ControlEvent> controls;
    TrackPlan neutral = plan;
    neutral.masterGainDb = masterGainDb;
    if (part != -2) {
        neutral.gainDb = 0.0f;
        for (float& g : neutral.partGainDb) g = 0.0f;
    }
    trackStartControls(p, neutral, 0.0, controls);
    arcControls(p, neutral, 0, 0.0, false, controls);
    for (const ControlEvent& c : controls) engine->pushControl(c);

    const int root = bassRootNote(plan.key, p.getInt(p.base(Module::Compose) + compose::BassRegister));
    const BassPatternDef& pat = kBassPatterns[plan.primaryPattern];
    std::vector<NoteEvent> notes;
    for (int beat = 0; beat < bars * kBeatsPerBar; ++beat) {
        NoteEvent k;
        k.beat = beat;
        k.part = Part::Kick;
        k.velocity = 127;
        notes.push_back(k);
        for (int s = 0; s < pat.count; ++s) {
            const double next = s + 1 < pat.count ? pat.pos[s + 1] : 1.0;
            NoteEvent n;
            n.beat = beat + pat.pos[s];
            n.length = static_cast<float>((next - pat.pos[s]) * plan.gate);
            n.part = Part::Bass;
            // The probe plays the track's own bass line, because a learned phrase is not the root
            // over and over: a bar of octaves is measurably quieter through the same ladder than a
            // bar of roots, and the level match between tracks would inherit that error. With the
            // pattern generator the phrase is absent and this is the root, as it always was.
            const int rel = plan.bassNeural
                                ? plan.bassRel[0][((beat / kBeatsPerBar % kBassPhraseBars) * kBeatsPerBar
                                                   + beat % kBeatsPerBar) * 3 + s]
                                : 0;   // beat runs over the whole probe, so beat/4 is its bar
            n.pitch = static_cast<uint8_t>(std::clamp(root + rel, 0, 127));
            n.velocity = 110;
            notes.push_back(n);
        }
    }
    // Which bar of the track each probe bar is: 49 and 50 (the percussion's layers are in) for the foundation
    // and the parts, the middle bar of the block at each fifth of the track for the whole mix.
    // Four contiguous windows of four bars, spread evenly over the track. Contiguous, because a pad
    // holds its chord over several bars and a reverb tail runs on: single bars cut from the middle of a
    // breakdown read far quieter than the same bar does in the track.
    auto sourceBar = [&](int b) {
        if (part != -2) return 49 + b;
        // The first window starts on bar 0 and the last ends on the last bar, so that the intro and
        // the outro count: they are a tenth of the track and quieter than everything else, and a probe
        // that skipped them aimed about 1 LU too high.
        const int window = ((b / 4) * (plan.bars - 4) / 3) / 4 * 4;
        return window + b % 4;
    };
    // The bar plan of a probe bar: the real instrumentation matrix for the whole mix, everything the
    // track has for a single part or the foundation (those measure a sound, not an arrangement).
    auto probeBar = [&](int source) {
        if (part != -2) {
            BarPlan bp = allPartsBar(plan.melody);
            bp.percLayers = plan.perc.layers;
            // A part's own probe plays that part whether or not the track uses it (19.09.2026): every
            // track has the material of every part, and the first track is the reference the later
            // ones are matched against -- a first track without a stab left every later stab unmatched.
            if (part >= 0) bp.parts = bp.partsNext = partBit(static_cast<MelodyPart>(part));
            return bp;
        }
        return planBar(plan.form, availabilityOf(plan), plan.sectionSeed, source);
    };
    // The foundation probe plays as many percussion layers as the track's first core does (19.09.2026).
    // It used to play all the track has; a core often plays fewer, and the probe then over-read the
    // track by up to 0.9 LU -- by different amounts from track to track, so the level match matched
    // the percussion plan instead of the sound. Measured in testVariety's level-match check: 0.81 LU of
    // spread with all layers, 0.40 with the core's (docs/PLAN.md, 19.09.2026). The round of 18.09.2026
    // had found the same limit from the other side: louder toms and congas widened the spread because
    // "the probe does not see how much of a track toms and congas play".
    //
    // 19.09.2026 (round "voices"): the first *drop*, where the track has one, and the first core only
    // where it has none. A track whose first core is a groove (the Full-On body) measured the groove's
    // thinner kit -- two layers plus one per eight-bar group -- and was lifted for it: on the listening
    // seed track 2 by +2.0 dB where its drop, kick, bass and kit alone, is only 0.3 LU quieter than
    // track 1's (rendered, master dynamics off: -11.8 against -11.5 LUFS unmatched, -9.8 matched). The
    // gain acts on kick, bass and percussion only, so the drop stood 1.7 dB hot and the level match of
    // its melodic parts, which takes the track gain back out, could not see it.
    int coreLayers = plan.perc.layers;
    int coreAt = -1;
    for (int pass = 0; pass < 2 && coreAt < 0; ++pass)
        for (int i = 0; i < plan.form.count && coreAt < 0; ++i) {
            const SectionType st = plan.form.section[i].type;
            if (st == SectionType::Drop || (pass == 1 && st == SectionType::Groove)) coreAt = i;
        }
    if (coreAt >= 0) {
        const Section& core = plan.form.section[coreAt];
        coreLayers = planBar(plan.form, availabilityOf(plan), plan.sectionSeed, core.startBar + std::min(4, core.bars - 1)).percLayers;
    }
    for (int b = 0; b < bars; ++b) {
        BarPlan bp = probeBar(sourceBar(b));
        if (part == -1) bp.percLayers = std::min(bp.percLayers, coreLayers);
        // The whole-mix probe has to hear the loudness side of the energy arc as well, or Auto Gain
        // would aim at a track that is louder than the one the form really plays (measured: 1.3 LU).
        if (part == -2) {
            const Section& sec = plan.form.section[std::clamp(bp.index, 0, kMaxSections - 1)];
            const ParamDesc& g = p.desc(mb + mix::TrackGain);
            ControlEvent c;
            c.beat = static_cast<double>(b) * kBeatsPerBar;
            c.param = static_cast<int16_t>(mb + mix::TrackGain);
            c.kind = ControlEvent::Kind::Offset;
            c.value = (plan.gainDb + energyGainDb(0.5f * (sec.energy + sec.energyTo))) / (g.maxValue - g.minValue);
            engine->pushControl(c);
        }
        PercBarSpec spec;
        spec.layers = bp.percLayers;
        spec.fills = false;
        spec.hatsDense = bp.hatsDense;
        spec.rollBar = bp.rollBar;
        spec.pdb = bp.pdb;
        spec.cutBeats = bp.cutBeats;
        composePercBar(p, plan.perc, plan.percSeed, b, sourceBar(b), plan.bpm, plan.key, plan.scale, spec, notes);
    }
    // The melodic parts: everything the track has for their own measurement, as the form plays them
    // for the whole mix.
    if (part >= 0 || part == -2) {
        std::vector<NoteEvent> mel;
        for (int b = 0; b < bars; ++b) {
            const BarPlan bp = probeBar(sourceBar(b));
            // The drone plays one held chord per run (MelodyContext::droneBars), which a probe bar
            // taken out of the middle of a run would never start: the part probe holds it over its two
            // bars in the upper octave, the mix probe over each four-bar window in the octave the form has
            // there (19.09.2026).
            MelodyContext ctx;
            if (part == -2) {
                ctx = melodyContext(plan, bp, sourceBar(b));
                if (b % 4 == 0 && ctx.droneBars == 0 && (bp.parts & partBit(MelodyPart::Drone)) != 0) {
                    ctx.droneBars = 4;
                    ctx.droneLow = droneLowAt(plan, sourceBar(b));
                }
            } else if (b == 0) {
                ctx.droneBars = bars;
            }
            composeMelodyBar(p, plan.melody, b, part == -2 ? sourceBar(b) : 48 + b, plan.scale, bp, mel, ctx);
        }
        for (const NoteEvent& n : mel) if (part == -2 || n.part == melodyScorePart(static_cast<MelodyPart>(part))) notes.push_back(n);
    }
    // One ring, so one order: the engine plays from the head of the ring.
    std::stable_sort(notes.begin(), notes.end(), noteLess);
    for (const NoteEvent& n : notes) engine->pushEvent(n);
    const int total = static_cast<int>(tm.secondsAt(bars * kBeatsPerBar) * sr);
    LoudnessMeter meter;
    meter.prepare(sr);
    std::vector<float> L(512), R(512);
    for (int done = 0; done < total; done += 512) {
        const int n = std::min(512, total - done);
        engine->process(L.data(), R.data(), n);
        meter.process(L.data(), R.data(), n);
    }
    return meter.read().integrated;
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

TempoMap Composer::tempoMap(const ParamStore& p, int bars) const
{
    TempoMap m;
    const TrackPlan& first = track(p, 0);
    m.setConstant(first.bpm);
    for (int i = 0;; ++i) {
        // By value: computing the next plan may move the cached ones.
        const TrackPlan t = track(p, i);
        const double start = static_cast<double>(t.firstBar) * kBeatsPerBar;
        if (i > 0) m.add(start, t.bpm, false);
        if (t.firstBar + t.bars >= bars) break;
        const TrackPlan next = track(p, i + 1);
        if (next.bpm != t.bpm) {
            const double rampStart = static_cast<double>(next.firstBar - 16) * kBeatsPerBar;
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
 * @brief The compositional transition between two tracks (PLAN 6.7).
 *
 * All the tracks come from one hand, so a transition is written rather than mixed: the hats of the
 * next track come in sixteen bars early, over the outro of this one; the pads of the previous track
 * hold on sixteen bars into this one's intro; and the key change at the swap -- always a 32-bar
 * boundary, because every track is a multiple of 32 bars long -- is masked by the outro's sweep, which
 * ends exactly on it, and by an impact on the downbeat.
 */
void Composer::transitionBar(const ParamStore& p, int ti, int inTrack, int bar, std::vector<NoteEvent>& out) const
{
    const TrackPlan& plan = plans_[static_cast<size_t>(ti)];
    const double barBeat = static_cast<double>(bar) * kBeatsPerBar;
    constexpr int kOverlap = 16;

    if (inTrack >= plan.bars - kOverlap && ti + 1 < static_cast<int>(plans_.size())) {
        const TrackPlan& next = plans_[static_cast<size_t>(ti + 1)];
        const int bIn = inTrack - (plan.bars - kOverlap);
        std::vector<NoteEvent> tmp;
        PercBarSpec spec;
        spec.layers = 1;
        spec.fills = false;
        composePercBar(p, next.perc, next.percSeed, bar, bIn, plan.bpm, next.key, next.scale, spec, tmp);
        const int hatLane = next.perc.layerOrder[0];
        const float fade = static_cast<float>(bIn + 1) / (kOverlap + 1);   // the hats grow into the swap
        for (NoteEvent& n : tmp) {
            if (n.lane != static_cast<uint8_t>(hatLane)) continue;
            n.velocity = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * fade)), 1, 127));
            out.push_back(n);
        }
    }
    if (inTrack < kOverlap && ti > 0) {
        const TrackPlan& prev = plans_[static_cast<size_t>(ti - 1)];
        // The pads keep their own pitches -- that is what "the pads of A stay" means -- so they only
        // carry over when the two keys are close enough for that to be consonant: the same key, a
        // fourth or a fifth. That is the harmonic-mixing rule of the DJ literature (Ishizaki, Hoashi
        // and Takishima 2009), applied to a transition we write rather than mix.
        const int move = ((plan.key - prev.key) % 12 + 12) % 12;
        if (prev.melody.present[mpIndex(MelodyPart::Pad)] && (move == 0 || move == 5 || move == 7)) {
            std::vector<NoteEvent> tmp;
            BarPlan pad;
            pad.parts = partBit(MelodyPart::Pad);
            pad.type = SectionType::Outro;
            composeMelodyBar(p, prev.melody, bar, prev.bars + inTrack, prev.scale, pad, tmp);
            const float fade = static_cast<float>(kOverlap - inTrack) / (kOverlap + 1);   // and the pads fade out
            for (NoteEvent& n : tmp) {
                if (n.part != Part::Pad) continue;
                n.velocity = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(n.velocity * fade)), 1, 127));
                out.push_back(n);
            }
        }
    }
    if (inTrack == 0 && ti > 0) {
        // The impact on the key change, together with the sweep that ends on this beat.
        NoteEvent e;
        e.beat = barBeat;
        e.length = 4.0f;
        e.part = Part::Sfx;
        e.pitch = static_cast<uint8_t>(kSfxBaseNote + static_cast<int>(SfxType::Impact));
        e.velocity = 110;
        out.push_back(e);
        // And the sub drop under it, as under every impact the form places (Form.cpp, placePsychedelia;
        // 19.09.2026). The next track's intro has no kick in its first bars, and the kick's own ducker
        // would hold the drop under it anyway (Engine.h). Only where the effects are on at all.
        if (p.get(p.base(Module::Compose) + compose::SfxAmount) > 0.0f) {
            e.pitch = static_cast<uint8_t>(kSfxBaseNote + static_cast<int>(SfxType::SubDrop));
            out.push_back(e);
        }
    }
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
        { const TrackPlan& t0 = track(p, ti); if (bar - t0.firstBar >= t0.bars - 16) track(p, ti + 1); }
        const TrackPlan& plan = track(p, ti);
        const int inTrack = bar - plan.firstBar;
        const float progress = static_cast<float>(inTrack) / static_cast<float>(plan.bars);
        const double barBeat = static_cast<double>(bar) * kBeatsPerBar;
        const int baseRoot = bassRootNote(plan.key, reg);
        const bool follow = p.getBool(cb + compose::BassFollowsChords);
        const int root = follow ? baseRoot + bassChordShift(plan.melody, plan.scale, inTrack, baseRoot) : baseRoot;
        const int chordDeg = follow ? plan.melody.chordDegree[chordIndexAt(plan.melody, inTrack)] : 0;

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
            if (inTrack == 0) {
                trackStartControls(p, plan, barBeat, *controls);
                arcControls(p, plan, inTrack, barBeat, false, *controls);
            } else if (inTrack % 32 == 0) {
                arcControls(p, plan, inTrack, barBeat, true, *controls);
            }
            if (bp.barInSection == 0) sectionControls(p, plan, bp, barBeat, *controls);
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
            if (plan.melody.present[mpIndex(MelodyPart::Pad)]) highPass(PolyInstance::Pad, foundationBar(bp));
            if (plan.melody.present[mpIndex(MelodyPart::Drone)]) {
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

        const StyleProfile& style = styleProfile(styleOf(p));
        for (int beat = 0; beat < kBeatsPerBar; ++beat) {
            const double b = barBeat + beat;
            const bool fillGap = kickPattern == 1 && inTrack % 8 == 7 && beat == 3;
            // The form decides per beat whether kick and bass play: a breakdown removes both (Solberg
            // and Dibben 2019), a pre-drop break removes them for part or all of its bar.
            if (kickPattern != 2 && !fillGap && ((bp.kickBeats >> beat) & 1) != 0 && static_cast<double>(beat) >= bp.cutBeats) {
                NoteEvent k;
                k.beat = b;
                k.length = 0.25f;
                k.part = Part::Kick;
                k.pitch = 36;
                k.velocity = 127;
                out.push_back(k);
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
        spec.pdb = bp.pdb;
        spec.cutBeats = bp.cutBeats;
        spec.crash = bp.barInSection == 0 && bp.type == SectionType::Drop;
        composePercBar(p, plan.perc, plan.percSeed, bar, inTrack, plan.bpm, plan.key, plan.scale, spec, out);
        composeMelodyBar(p, plan.melody, bar, inTrack, plan.scale, bp, out, melodyContext(plan, bp, inTrack));
        composeSfxBar(plan.form, static_cast<double>(plan.firstBar) * kBeatsPerBar, inTrack, out);
        transitionBar(p, ti, inTrack, bar, out);
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
