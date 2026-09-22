/**
 * @file Composer.cpp
 * @brief Track plans, sound recipes, bars of kick and bass, and the conductor.
 */
#include "phos/Composer.h"
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
#include "phos/WaveTableFile.h"   // waveTableLaneTables(), for kVoicePalette
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
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
constexpr uint64_t kSaltStyle  = 0x5354594C45000016ull;   ///< the style journey (22.09.2026): its own stream, so no older draw of the walk moves
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
/** @brief Salt of the section's choice between the track's two pad gate patterns (22.09.2026). */
constexpr uint64_t kSaltPadGate = 0x504144474154ull;
/** @brief Salt of the drone's slow evolution (19.09.2026). */
constexpr uint64_t kSaltDroneRide = 0x44524944450015ull;

/**
 * @brief What a polyphonic voice may become in a track: its oscillators, wavetables and filter responses,
 *        each with a weight (19.09.2026, round "voices"; rebuilt 22.09.2026, round "Klangfarben").
 *
 * ## Lanes, not table numbers
 *
 * Until this round a row named its tables by hand -- fifteen indices into the `table` parameter, with
 * a `static_assert` tying the array's size to the pack's 35 entries so that neither could move without
 * the other. That was workable while the pack held 35 tables and is the reason the pack *stayed* at 35:
 * every widening meant re-writing six rows of indices by hand. A row now names the **lanes**
 * `Tools/wt_select.py` measured the library into (WaveTableFile.h, WaveTableLane), plus whichever
 * built-ins fit the role, and `waveTableLaneTables()` turns that into the candidate list at run time.
 * Repack with a wider selection and the palettes widen with it.
 *
 * The pack this round ships holds 464 library tables against the last round's 35: 80 lead, 96 arp,
 * 128 pad, 96 counter and 64 drone, each chosen from the 2191 licence-clean tables of the Noctuary
 * library by the same farthest-point spreading rule as before (`Core/data/CREDITS-wavetables.md`).
 * They cost nothing until a track asks for one: the pack is only indexed at load and a table is
 * expanded into its mip levels when a voice's recipe actually draws it (WaveTableFile.h,
 * ensureWaveTables), which is what makes a library of this size possible at all -- expanded in full
 * it would be 900 MB.
 *
 * **The counter-lead has its own lane now.** The "voices" round gave it three built-ins, the
 * vocal/formant family, deliberately: it had to stay out of the lead's and the arp's table families so
 * that the two leads could never share a sound. That was the right call against a 35-table pack and it
 * is exactly why the counter was the one voice the user could not tell apart from track to track --
 * three tables is three colours. The lane (`counter` in wt_select.py) keeps the character the choice
 * was about, the vowel: a mid centroid that *moves* across the frames, with a fundamental still there.
 * The hard guard in the recipe draw below (`v == counterV`: never the lead's oscillator, never the
 * lead's table) is what keeps the two apart, and it does that far more comfortably against 96 tables
 * than against three.
 *
 * ## Oscillators
 *
 * Every lane can now draw every oscillator. The old rows had zeros in them -- the pad could never be
 * VA or FM, the drone never supersaw or FM, the stab never FM -- so a third of the recipe's reach was
 * dead for most voices, and the FM parameters the recipe learned to shape on 21.09.2026 were dead for
 * the pad entirely. The user's point ("die Mischung 0,25 und 0,75 zwischen FM und Wavetable ist ja
 * nicht gottgegeben"): the weights are a tilt towards the role, not a gate. The wavetable weight is
 * the largest everywhere, because that is the axis the 464 tables sit on; the rest say what the voice
 * leans towards when it is not a table -- the lead towards the supersaw, the drone and the counter
 * towards VA and FM, the pad towards a low-index FM (a classic pad, not a bell).
 */
struct VoicePalette {
    double osc[static_cast<int>(PolyOsc::Count)];       ///< weight per PolyOsc
    int    builtin[6];                                  ///< built-in candidates (-1 ends the list)
    int8_t lane[3];                                     ///< lanes drawn from (-1 ends the list)
    double filter[static_cast<int>(PolyFilter::Count)]; ///< weight per PolyFilter
    double osc2[static_cast<int>(PolyOsc2::Count)];     ///< weight per PolyOsc2, index 0 = no second oscillator
    double interval[static_cast<int>(PolyOsc2Interval::Count)];   ///< weight per PolyOsc2Interval
    float  scale[kNumVoiceMacros];                      ///< how far each direction reaches for this voice
};
constexpr int8_t kLanePad = static_cast<int8_t>(WaveTableLane::Pad);
constexpr int8_t kLaneLead = static_cast<int8_t>(WaveTableLane::Lead);
constexpr int8_t kLaneArp = static_cast<int8_t>(WaveTableLane::Arp);
constexpr int8_t kLaneDrone = static_cast<int8_t>(WaveTableLane::Drone);
constexpr int8_t kLaneCounter = static_cast<int8_t>(WaveTableLane::Counter);
// The drone's second oscillator never goes *below* it, which is the one row where that matters and
// where the first attempt at these weights was wrong. The drone is already the lowest voice, and
// rule 20 is explicit about the band under 140 Hz: it belongs to the kick and the bass from their
// first beat back, which is why the drone moves up an octave when they return (Melody.cpp,
// makeDrone). An octave-down partner put it straight back there -- testVoices.droneRender measured
// -5.9 dB in the two bars after the breakdown where it wants -18 dB or less. Unison, a fifth up or
// an octave up give it an organ-like body instead, in its own register.
//
// The pad never answers below its own note either, and the reason is sharper than the drone's. Its
// high pass tracks the note, so an octave-down partner is filtered away -- *except* in a breakdown,
// where rule 20 opens that high pass to 40 Hz on purpose so the pad can carry the floor kick and
// bass have left. So the one place the low partner is audible at all is the one place it does harm:
// bisected to this row, the breakdown's 40..140 Hz band went from 23.7 dB under the core to 18.1,
// against the 20 dB the section rule asks for (testSectionRules). Unison with a few cents of detune
// is what changes the pad's *colour* rather than its weight anyway -- with an octave-down partner on
// every pad the rendered spread of the pad's centroid across twenty tracks fell from 915 to 321
// cents, because a low partner pulls every track's centroid to the same place.
//
// Per row: the first oscillator's weights; the built-in candidate tables; the library lanes; the
// filter responses; the second oscillator's weights (index 0 = none, and it is the largest
// everywhere -- a second oscillator is a colour a track may draw, not a thing every track has); the
// interval it answers at (-2 Oct, -1 Oct, -5th, Unison, +5th, +1 Oct); and the reach of the five
// directions.
//                                     Supersaw VA   FM   WT      built-ins                  lanes                             LP    BP    HP   Notch     off   sup   va    fm    wt          -2oct -1oct -5th  uni   +5th  +1oct     bright soft thick space motion
const VoicePalette kVoicePalette[kPolyInstances] = {
    /* lead    */ { { 0.30, 0.12, 0.13, 0.45 }, { 4, 5, -1, -1, -1, -1 },  { kLaneLead, -1, -1 },        { 0.80, 0.10, 0.0, 0.10 }, { 0.45, 0.15, 0.15, 0.10, 0.15 }, { 0.05, 0.35, 0.10, 0.20, 0.05, 0.25 }, { 1.0f, 0.6f, 1.0f, 1.0f, 0.8f } },
    /* counter */ { { 0.08, 0.17, 0.20, 0.55 }, { 5, 1, 2, -1, -1, -1 },   { kLaneCounter, -1, -1 },     { 0.40, 0.40, 0.0, 0.20 }, { 0.45, 0.05, 0.20, 0.20, 0.10 }, { 0.05, 0.30, 0.10, 0.30, 0.05, 0.20 }, { 1.0f, 0.8f, 0.8f, 1.0f, 1.0f } },
    /* arp     */ { { 0.22, 0.20, 0.13, 0.45 }, { 2, 3, -1, -1, -1, -1 },  { kLaneArp, -1, -1 },         { 0.75, 0.25, 0.0, 0.00 }, { 0.50, 0.10, 0.15, 0.10, 0.15 }, { 0.02, 0.28, 0.05, 0.25, 0.05, 0.35 }, { 1.0f, 0.0f, 0.8f, 1.0f, 0.6f } },
    /* stab    */ { { 0.28, 0.18, 0.09, 0.45 }, { 3, 4, -1, -1, -1, -1 },  { kLaneArp, kLaneLead, -1 },  { 0.70, 0.30, 0.0, 0.00 }, { 0.45, 0.15, 0.15, 0.10, 0.15 }, { 0.05, 0.35, 0.10, 0.20, 0.05, 0.25 }, { 1.0f, 0.0f, 1.0f, 1.0f, 0.5f } },
    /* pad     */ { { 0.15, 0.08, 0.12, 0.65 }, { 1, 2, 5, -1, -1, -1 },   { kLanePad, -1, -1 },         { 0.85, 0.00, 0.0, 0.15 }, { 0.30, 0.10, 0.25, 0.15, 0.20 }, { 0.00, 0.00, 0.12, 0.45, 0.15, 0.28 }, { 0.8f, 1.0f, 1.0f, 1.0f, 1.0f } },
    /* drone   */ { { 0.06, 0.20, 0.09, 0.65 }, { 1, -1, -1, -1, -1, -1 }, { kLaneDrone, kLanePad, -1 }, { 0.90, 0.10, 0.0, 0.00 }, { 0.30, 0.05, 0.30, 0.10, 0.25 }, { 0.00, 0.00, 0.05, 0.45, 0.20, 0.30 }, { 0.7f, 0.6f, 1.0f, 0.8f, 1.0f } },
};

/**
 * @brief Real candidates in a palette's `tables[]`: entries before the first -1 (or the array's end).
 *
 * The one place this is counted -- the per-track recipe draw below calls it too, so
 * `Composer::voicePaletteTableCount()` (the test-facing wrapper, Composer.h) can never drift from what
 * a track actually draws from.
 */
int paletteTableCount(const VoicePalette& pal)
{
    int n = 0;
    for (int b : pal.builtin) if (b >= 0) ++n;
    for (int8_t l : pal.lane) if (l >= 0) n += static_cast<int>(waveTableLaneTables(static_cast<WaveTableLane>(l)).size());
    return n;
}

/** @brief Candidate @p k of a palette: the built-ins first, then each lane in turn. */
int paletteTableAt(const VoicePalette& pal, int k)
{
    for (int b : pal.builtin) {
        if (b < 0) break;
        if (k-- == 0) return b;
    }
    for (int8_t l : pal.lane) {
        if (l < 0) break;
        const std::vector<int>& lane = waveTableLaneTables(static_cast<WaveTableLane>(l));
        if (k < static_cast<int>(lane.size())) return lane[static_cast<size_t>(k)];
        k -= static_cast<int>(lane.size());
    }
    return -1;
}

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
// 21.09.2026, at the user's request ("Vergiss nicht die Huellkurven, auch fuer den Filter"): the
// recipe reached only four of a voice's envelope parameters -- attack, release, filter decay and the
// filter envelope's amount. What shapes a sustained voice most was missing: the amp envelope's decay
// and sustain (the difference between a pad that swells and one that is plucked), the *speed* of the
// position movement (only its depth was drawn, so every voice moved at the same 16 beats), the
// filter's key tracking, and every FM parameter -- so a voice that drew the FM oscillator sounded
// the same in every track whatever else it drew. They cost no table, no memory and no load time.
const Loading kVoiceLoadings[] = {
    { poly::Cutoff,      0, 0.18f }, { poly::EnvAmount,   0, 0.10f }, { poly::Resonance, 0, 0.08f }, { poly::Position, 0, 0.20f },
    { poly::KeyTrack,    0, 0.12f },
    { poly::AmpAttack,   1, 0.15f }, { poly::FilterDecay, 1, 0.15f }, { poly::AmpRelease, 1, 0.10f },
    { poly::AmpDecay,    1, 0.18f }, { poly::AmpSustain,  1, 0.14f },
    { poly::Detune,      2, 0.25f }, { poly::Mix,         2, 0.15f }, { poly::Width,    2, 0.20f },
    { poly::DynamicDetune, 2, 0.15f }, { poly::FmRatio,   2, 0.12f },
    { poly::DelaySend,   3, 0.20f }, { poly::DelayFeedback, 3, 0.10f },
    { poly::PosLfoDepth, 4, 0.30f }, { poly::PosEnv,      4, 0.20f }, { poly::Drift,    4, 0.15f },
    { poly::PosLfoBeats, 4, 0.25f }, { poly::PosDecay,    4, 0.20f }, { poly::FmIndex,  4, 0.18f },
    { poly::FmDecay,     1, 0.15f },
    // 22.09.2026, round "Klangfarben". The second oscillator's balance is thickness by definition,
    // and its detune belongs there too -- a few cents between two oscillators is the width of an
    // analogue pad. The LFO's three depths and its period are the motion direction: that is what the
    // direction is for, and they are the cheapest movement in the engine (one sine per 16 samples for
    // a whole instance, read by the cutoff, the pitch and the level alike).
    { poly::Osc2Mix,     2, 0.30f }, { poly::Osc2Detune,  2, 0.25f },
    { poly::LfoCutoff,   4, 0.35f }, { poly::LfoAmp,      4, 0.25f },
    { poly::LfoPitch,    4, 0.20f }, { poly::LfoBeats,    4, 0.30f },
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
        // A foundation starts where a chord block starts -- or where the floor falls silent inside one
        // (22.09.2026). Until this round a block was two or four bars, so a breakdown's second block
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
static bool droneLowAt(const TrackPlan& plan, int inTrack)
{
    if (!plan.melody.present[mpIndex(MelodyPart::Drone)]) return false;
    return droneBarAt(plan, availabilityOf(plan), inTrack).low;
}

static PartAvailability availabilityOf(const TrackPlan& plan)
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

int Composer::voicePaletteTableCount(PolyInstance voice)
{
    return paletteTableCount(kVoicePalette[polyIndex(voice)]);
}

void Composer::matchMaster(const ParamStore& p, TrackPlan& t, const double* firstReading) const
{
    const int ms = p.base(Module::Master);
    t.masterGainDb = 0.0f;
    if (!p.getBool(ms + master::AutoGain)) return;
    const double target = p.get(ms + master::TargetLufs);
    // Measure, correct, measure again, and take a secant step between the two readings.
    // The first reading may have been rendered already, beside the presence probes (measureTrack).
    const double l0 = firstReading != nullptr ? *firstReading : probeLoudness(p, t, -2, 0.0f);
    t.mixLoudness = l0;
    if (l0 < -60.0) return;
    const double g1 = std::clamp(target - l0, -18.0, 18.0);
    const double l1 = probeLoudness(p, t, -2, static_cast<float>(g1));
    const double slope = std::fabs(g1) > 0.05 ? (l1 - l0) / g1 : 1.0;
    const double g2 = g1 + (target - l1) / std::clamp(slope, 0.2, 1.0);
    t.masterGainDb = static_cast<float>(std::clamp(g2, -18.0, 18.0));
}

/**
 * The presence match (19.09.2026, round "polish"). Measured over 30 tracks of the arrangement round (5 styles,
 * 2 seeds, 3 tracks each; Tools/metrics.py on the rendered drops): the presence band (1.5..6 kHz against the
 * kick-and-bass band 40..140 Hz) of drop 1 ran from -6.2 to +7.1 dB around the reference recordings' median,
 * interquartile -4.4 .. -0.7 -- the listening seed's track 2 at +2.8 / +3.7 over it, its track 1 at -3.1. The
 * level match cannot see it: it matches each part's *loudness* to the first track's, and a line that is
 * brighter at the same loudness puts more into the presence band. A cut on every track would push the dark
 * ones further down, so the match is per track and only outside a band around the median:
 *
 *  - two probe renders of the drops (four bars of each, one per eight-bar group, the form's own instrumentation
 *    and section controls, the master's dynamics off): the lines alone -- lead, counter, arp, stab, the voices
 *    whose recipes carry the brightness -- and everything else. Both are linear in the lines' gain g, so
 *    presence(g) = (g Pl + Pr) / (g Ll + Lr) is solved for g in closed form, no third render;
 *  - the estimate is read against the reference median through kPresenceRefDb: the probe's raw ratio minus it is
 *    the rendered drops' presence (the power mean of the two drops, Tools/metrics.py bands) over 30 calibration
 *    tracks (5 styles, 2 seeds, 3 tracks each; the match off) with a median error of -0.04 dB, interquartile
 *    -0.33 .. +0.33 dB, standard deviation 0.81, correlation 0.97; the three worst read the render 1.8 .. 2.5 dB
 *    low (bright tracks whose drop 2 carries more than the probe's four bars show). Two bars from the middle of each drop without the section controls
 *    had read the bright tracks up to 2.4 dB low, and a constant 7.7 dB apart from the render;
 *  - inside kPresenceBandDb of the median nothing moves (the variety of the voices round stays); outside, the
 *    lines are brought to the band's edge, at most kPresenceCutDb down or kPresenceLiftDb up.
 *
 * **The cap, measured rather than widened (20.09.2026, round "climax-polish").** The brightest of 30 tracks
 * (5 styles, 2 seeds, 3 tracks; the same sample as above) still sat +4.6 dB over the median after the cap --
 * is the cap too narrow, or is the brightness not in the lines to begin with? Tried both -6 and -9 dB on the
 * same 30 tracks: the three tracks the cap actually binds on moved 0.47, 1.28 and 1.30 dB at -6, and 0.55,
 * 1.53 and 1.56 dB at -9 -- 50 % more cut bought at most 0.3 dB more correction. `presenceDb` only ever reads
 * the lines against the rest (probeLoudness's kProbeLines/kProbeRest split); a track whose brightness sits in
 * its acid, its percussion or its pad instead has little for a deeper cut on the lines to remove, however
 * deep it goes -- diminishing returns, not a narrow cap. Left at -6/+3: widening it would have cost more
 * headroom on tracks it does help (the two with all four lines present moved the most) for a track like
 * this one that would stay bright regardless. A cut across the *rest* would reach it, but that is a
 * different, larger change -- it touches the acid and the kit's own recipes, not a level on top of them --
 * and the voices round's own diversity is deliberately outside matchPresence's reach (see above).
 */
namespace {
constexpr double kPresenceRefDb = -7.43;   ///< the probe's reading at the reference median (calibrated, see above)
constexpr double kPresenceBandDb = 1.5;    ///< no correction within this distance of the median
constexpr double kPresenceCutDb = 6.0;     ///< the most the lines are taken down (kept at 6, not widened -- see above)
constexpr double kPresenceLiftDb = 3.0;    ///< the most they are brought up
} // namespace

void Composer::matchPresence(const ParamStore& p, TrackPlan& t, const double* linesReading, const double* restReading) const
{
    t.presenceDb = 0.0;
    t.presenceAfterDb = 0.0;
    t.presenceGainDb = 0.0f;
    // Measured whenever the level match runs, so that a render with the match off still reports where each
    // track stands (phos_render --tracks); corrected only with compose.presence_match on.
    // The two readings may have been rendered already, side by side (measureTrack).
    double lines[2] = {}, rest[2] = {};
    if (linesReading != nullptr && restReading != nullptr) {
        for (int i = 0; i < 2; ++i) { lines[i] = linesReading[i]; rest[i] = restReading[i]; }
    } else {
        probeLoudness(p, t, kProbeLines, 0.0f, lines);
        probeLoudness(p, t, kProbeRest, 0.0f, rest);
    }
    const double pl = lines[0], ll = lines[1], pr = rest[0], lr = rest[1];
    if (pl + pr <= 0.0 || ll + lr <= 0.0) return;
    t.presenceDb = t.presenceAfterDb = 10.0 * std::log10((pl + pr) / (ll + lr)) - kPresenceRefDb;
    if (!p.getBool(p.base(Module::Compose) + compose::PresenceMatch)) return;
    if (std::fabs(t.presenceDb) <= kPresenceBandDb || pl <= 0.0) return;
    // The band's edge on the side the track is on, as a ratio of the two bands' powers.
    const double want = std::pow(10.0, (kPresenceRefDb + (t.presenceDb > 0.0 ? kPresenceBandDb : -kPresenceBandDb)) / 10.0);
    const double num = want * lr - pr, den = pl - want * ll;
    double g = (num > 0.0 && den > 0.0) ? num / den : (t.presenceDb > 0.0 ? 0.0 : 1e9);
    const double db = g > 0.0 ? 10.0 * std::log10(g) : -kPresenceCutDb;
    t.presenceGainDb = static_cast<float>(std::clamp(db, -kPresenceCutDb, kPresenceLiftDb));
    // Where the correction lands, by the same linear model: the band's edge unless a cap stopped it. The
    // plans carry it so that a check (testPresence) can see the match's own prediction, not only its gain.
    const double gain = std::pow(10.0, static_cast<double>(t.presenceGainDb) / 10.0);
    t.presenceAfterDb = 10.0 * std::log10((gain * pl + pr) / (gain * ll + lr)) - kPresenceRefDb;
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
            const double walk = trackBpm + 0.6 * (prev.bpm - trackBpm) + (2.0 * r.uniform() - 1.0) * trackRange * tv;
            w.bpm = std::round(std::clamp(walk, trackBpm - trackRange, trackBpm + trackRange) * 2.0) * 0.5;

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
    // The DJ overlap (Form.h, kDjOverlap): every track after the first starts kDjOverlap bars before the one
    // before it ends, its kick-free intro over the other's bare outro.
    t.firstBar = index == 0 ? 0 : plans_[static_cast<size_t>(index - 1)].firstBar + plans_[static_cast<size_t>(index - 1)].bars - kDjOverlap;
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
    t.form.handover = index == 0 ? 0 : kDjOverlap;
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
        measureTrack(p, t);
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

    measureTrack(p, t);
    return t;
}

/**
 * @brief Everything a plan learns from probe renders: the level match, the presence match and Auto Gain.
 *
 * Until 20.09.2026 these were twelve renders one after the other, 12 s per track. What each probe *reads*
 * decides what may run at the same time (round "speed"; the maths of every step is where it was):
 *
 *  1. the foundation and the melodic parts play the track with every correction taken out (probeLoudness
 *     zeroes gainDb, partGainDb and presenceGainDb for them), so they depend on nothing measured and on
 *     each other not at all -- one stage, up to eight at once;
 *  2. the two presence probes keep the level corrections (the balance they measure is the one those make),
 *     so they wait for stage 1 and then run side by side;
 *  3. the first mix probe plays the track with *all* corrections, the presence gain on the lines included
 *     (trackStartControls), so it waits for stage 2 -- unless compose.presence_match is off, when that gain
 *     is zero by construction and the mix probe runs beside the presence probes;
 *  4. the second mix probe needs the first one's reading for its gain, so it is last and alone.
 *
 * The longest chain is then 2 + 8 + 16 + 16 bars instead of 64 to 72: measured 10.9 s against 15.3 s for the
 * listening seed's first track beside another round's load (the two mix probes are 8.2 s of it and cannot
 * overlap). Starting the first mix probe early on the guess that the presence gain will be zero was
 * measured and dropped: the guess holds for 17 of 44 plans (seeds 1..6 and the listening seed), saves 2.1 s
 * when it does, and burns a 16-bar render when it does not -- CPU a parallel test run has no spare of.
 *
 * With probe::threads() == 1 -- the library's default, the plugin and the Quest -- the old serial order
 * runs unchanged, and testProbeSchedule compares the two bit for bit. The tasks never touch the plan: each
 * reads a snapshot taken before its stage and writes a slot of its own, because a probe copies the whole
 * plan and another task writing one of its fields meanwhile would be a data race even where the value is
 * not used.
 */
void Composer::measureTrack(const ParamStore& p, TrackPlan& t) const
{
    const int cb = p.base(Module::Compose);
    const bool level = p.getBool(cb + compose::LevelMatch);
    const bool first = t.index == 0;
    // The level match's arithmetic, once for both orders. The first track is the reference: it measures
    // every part and corrects nothing.
    auto trackGain = [&] {
        if (!first) t.gainDb = static_cast<float>(std::clamp(plans_[0].loudness - t.loudness, -9.0, 9.0));
    };
    // A part the track does not use needs no correction (the first track measures all of them: it is the
    // reference for every later one).
    auto partProbed = [&](int k) { return first || t.melody.present[k]; };
    // Each melodic part against the same part in the first track; the track gain applies to it as well,
    // so it is taken back out.
    auto partGain = [&](int k) {
        if (first) return;
        if (!t.melody.present[k]) { t.partLoudness[k] = -120.0; t.partGainDb[k] = 0.0f; return; }
        const bool measurable = plans_[0].partLoudness[k] > -60.0 && t.partLoudness[k] > -60.0;
        t.partGainDb[k] = measurable ? static_cast<float>(std::clamp(plans_[0].partLoudness[k] - t.partLoudness[k] - t.gainDb, -12.0, 12.0)) : 0.0f;
    };

    if (probe::threads() <= 1) {
        // The serial order of before 20.09.2026.
        if (level) {
            t.loudness = probeLoudness(p, t);
            trackGain();
            for (int k = 0; k < kMelodyParts; ++k) {
                if (partProbed(k)) t.partLoudness[k] = probeLoudness(p, t, k);
                partGain(k);
            }
            matchPresence(p, t);
        }
        if (deferMaster_) { t.masterGainDb = 0.0f; t.masterDeferred = true; }
        else matchMaster(p, t);
        return;
    }

    // The shared loads are not thread-safe (Probe.h): on this thread, before any worker exists.
    probe::warmSharedData();
    const bool autoGain = p.getBool(p.base(Module::Master) + master::AutoGain);
    std::vector<std::function<void()>> tasks;
    if (level) {
        // Stage 1: the foundation and the parts.
        const TrackPlan snap = t;
        double loudness = 0.0, partLoudness[kMelodyParts] = {};
        tasks.push_back([&] { loudness = probeLoudness(p, snap); });
        for (int k = 0; k < kMelodyParts; ++k)
            if (partProbed(k)) tasks.push_back([&, k] { partLoudness[k] = probeLoudness(p, snap, k); });
        probe::runAll(tasks);
        t.loudness = loudness;
        trackGain();
        for (int k = 0; k < kMelodyParts; ++k) {
            if (partProbed(k)) t.partLoudness[k] = partLoudness[k];
            partGain(k);
        }
    }
    // Stage 2: the presence probes -- and the first mix probe beside them only where the presence match
    // cannot move the lines, because it is off (the mix probe first in the list: it is the longest).
    const bool mixEarly = autoGain && level && !deferMaster_ && !p.getBool(cb + compose::PresenceMatch);
    double lines[2] = {}, rest[2] = {}, mix0 = 0.0;
    if (level) {
        t.presenceGainDb = 0.0f;   // what matchPresence starts from
        const TrackPlan snap = t;
        tasks.clear();
        if (mixEarly) tasks.push_back([&] { mix0 = probeLoudness(p, snap, -2, 0.0f); });
        tasks.push_back([&] { probeLoudness(p, snap, kProbeLines, 0.0f, lines); });
        tasks.push_back([&] { probeLoudness(p, snap, kProbeRest, 0.0f, rest); });
        probe::runAll(tasks);
        matchPresence(p, t, lines, rest);
    }
    // Stages 3 and 4: Auto Gain's two readings, the second after the first -- unless a host asked to
    // have them later (setDeferMasterGain). `mixEarly` puts the first of the two beside the presence
    // probes where the presence match cannot move the lines; a deferred track does not want it there
    // either, so it is not started.
    if (deferMaster_) { t.masterGainDb = 0.0f; t.masterDeferred = true; return; }
    matchMaster(p, t, mixEarly ? &mix0 : nullptr);
}

float Composer::completeMasterGain(const ParamStore& p, int index) const
{
    if (index < 0 || index >= static_cast<int>(plans_.size())) return 0.0f;
    TrackPlan& t = plans_[static_cast<size_t>(index)];
    if (!t.masterDeferred) return t.masterGainDb;
    // The two whole-mix probes this track did not run when it was planned. matchMaster writes
    // masterGainDb and mixLoudness into the plan, which is what a later seek into this track will
    // then push at its start like any other track's.
    matchMaster(p, t);
    t.masterDeferred = false;
    return t.masterGainDb;
}

void Composer::trackStartControls(const ParamStore& p, const TrackPlan& plan, double beat, std::vector<ControlEvent>& out,
                                  ControlScope scope) const
{
    const int cb = p.base(Module::Compose), kb = p.base(Module::Kick), bb = p.base(Module::Bass), mb = p.base(Module::Mix);
    const float sv = p.get(cb + compose::SoundVariation);
    const bool floor = scope != ControlScope::Voices, voices = scope != ControlScope::Floor;
    auto push = [&](int id, ControlEvent::Kind kind, float value) {
        ControlEvent c;
        c.beat = beat;
        c.param = static_cast<int16_t>(id);
        c.kind = kind;
        c.value = value;
        out.push_back(c);
    };
    // The DJ overlap (19.09.2026): a track's voices sound from its first bar, over the previous track's
    // bare outro, but its key, kick, bass, kit, acid and gains take over only at the hand-over, with its
    // kick and bass (ControlScope). A track that starts a set writes both at once.
    if (floor) {
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
    }   // floor
    (void)bb;

    // Melodic parts: switches of the track, delay times, level corrections and sound directions.
    const MelodyPlan& m = plan.melody;
    const int ab = p.base(Module::Acid);
    if (floor) {
    push(ab + acid::Squelch, ControlEvent::Kind::Override, static_cast<float>(m.acidSquelch));
    push(ab + acid::DelayLeft, ControlEvent::Kind::Override, static_cast<float>(m.delay[mpIndex(MelodyPart::Acid)][0]));
    push(ab + acid::DelayRight, ControlEvent::Kind::Override, static_cast<float>(m.delay[mpIndex(MelodyPart::Acid)][1]));
    }
    for (int k = 0; k < kMelodyParts; ++k) {
        const MelodyPart part = static_cast<MelodyPart>(k);
        if (part == MelodyPart::Acid ? !floor : !voices) continue;
        const int level = mb + (part == MelodyPart::Acid ? static_cast<int>(mix::AcidLevel) : mix::polyLevel(melodyPoly(part)));
        const ParamDesc& d = p.desc(level);
        // The lines carry the presence match on top of their level match (matchPresence).
        const bool line = part == MelodyPart::Lead || part == MelodyPart::Counter || part == MelodyPart::Arp || part == MelodyPart::Stab;
        push(level, ControlEvent::Kind::Offset, (plan.partGainDb[k] + (line ? plan.presenceGainDb : 0.0f)) / (d.maxValue - d.minValue));
    }
    // The track's acid voicing (Composer.h, kNumAcidVoicings). Cutoff and Env Amount are written again
    // by every section with the voicing folded into the section's base (sectionControls); they are
    // written here as well so that the level match's part probe, which plays the track start alone,
    // hears the voicing it has to match.
    if (floor) {
        float acidOff[acid::Count] = {};
        int disperse = -1;
        acidVoicingOffsets(p, plan.acidVoicing, sv, acidOff, disperse);
        for (const AcidVoicingParam& v : kAcidVoicingTable) push(ab + v.param, ControlEvent::Kind::Offset, acidOff[v.param]);
        push(ab + acid::Disperse, ControlEvent::Kind::Override, static_cast<float>(disperse));
    }
    // Decay and resonance: the voicing's offset (pushed above) plus the old per-track direction, as one
    // value -- a strand holds an offset, it does not add one; the section ride swings around this sum.
    if (floor) {
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
    for (int v = 0; v < kPolyInstances && voices; ++v) {
        const PolyInstance inst = static_cast<PolyInstance>(v);
        const VoiceRecipe& rc = plan.voice[v];
        const int vb = p.base(inst);
        const bool vary = sv > 0.0f;
        push(vb + poly::Osc, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.osc : -1));
        push(vb + poly::Table, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.table : -1));
        push(vb + poly::FilterType, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.filter : -1));
        push(vb + poly::DelayLeft, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.delayL : -1));
        push(vb + poly::DelayRight, ControlEvent::Kind::Override, static_cast<float>(vary ? rc.delayR : -1));
        // The second oscillator (22.09.2026). Like every other discrete choice it is an override, so
        // Sound Variation at 0 leaves the knobs exactly as they stand.
        push(vb + poly::Osc2, ControlEvent::Kind::Override, static_cast<float>(vary && rc.hasOsc2 ? rc.osc2 : -1));
        push(vb + poly::Osc2Interval, ControlEvent::Kind::Override,
             static_cast<float>(vary && rc.hasOsc2 ? rc.osc2Semis : -1));
        float off[poly::Count] = {};
        voiceRecipeOffsets(inst, rc, sv, off);
        for (const Loading& l : kVoiceLoadings) push(vb + l.param, ControlEvent::Kind::Offset, off[l.param]);
    }
    if (voices) push(p.base(PolyInstance::Pad) + poly::GatePattern, ControlEvent::Kind::Override, static_cast<float>(m.padGatePattern));
    // The loudness offset of Auto Gain, in the normalised domain of master.gain's 36 dB range.
    const ParamDesc& mg = p.desc(p.base(Module::Master) + master::Gain);
    if (floor) push(p.base(Module::Master) + master::Gain, ControlEvent::Kind::Offset, plan.masterGainDb / (mg.maxValue - mg.minValue));
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
                               std::vector<ControlEvent>& out, ControlScope scope) const
{
    const int cb = p.base(Module::Compose), ab = p.base(Module::Acid), mb = p.base(Module::Mix);
    const int lb = p.base(PolyInstance::Lead), pb = p.base(PolyInstance::Pad);
    const float sv = p.get(cb + compose::SoundVariation), mv = p.get(cb + compose::MelodyVariation);
    const MelodyPlan& m = plan.melody;
    // The DJ overlap (19.09.2026): the incoming track's first section (its intro) writes its voices from
    // its first bar and its floor -- acid, gain, rides -- at the hand-over, as a section that starts there
    // and runs to the intro's end, so that no event is ever dated behind the bar it is written in.
    Section s = plan.form.section[bar.index];
    if (scope == ControlScope::Floor && bar.barInSection > 0) {
        s.startBar += bar.barInSection;
        s.bars = std::max(1, s.bars - bar.barInSection);
        s.energy = bar.energy;
    }
    const bool floor = scope != ControlScope::Voices, voices = scope != ControlScope::Floor;
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
        if (floor) push(ab + acid::Cutoff, cutoffAt(acidBase, 1.0f, e0), 0.0f);
        if (voices) push(lb + poly::Cutoff, cutoffAt(leadBase, 0.8f, e0), 0.0f);
        if (voices) push(pb + poly::Position, cutoffAt(padBase, 0.6f, e0), 0.0f);
    }
    // Drop 2 opens the acid's and the lead's filters by kClimaxOpen (Form.h), at its first bar rather than
    // over its length: the lift is the arrival, not another ramp (19.09.2026, round "polish").
    const float open = s.climax && !knobs ? kClimaxOpen : 0.0f;
    if (open > 0.0f) {
        if (floor) push(ab + acid::Cutoff, cutoffAt(acidBase, 1.0f, e0) + open, 0.0f);
        if (voices) push(lb + poly::Cutoff, cutoffAt(leadBase, 0.8f, e0) + open, 0.0f);
    }
    if (floor) push(ab + acid::Cutoff, cutoffAt(acidBase, 1.0f, e1) + open, length);
    if (floor) push(ab + acid::EnvAmount, 0.5f * (cutoffAt(acidBase, 1.0f, e1) - acidBase) + voicing[acid::EnvAmount], length);
    if (voices) push(lb + poly::Cutoff, cutoffAt(leadBase, 0.8f, e1) + open, length);
    if (voices) push(pb + poly::Position, cutoffAt(padBase, 0.6f, e1), length);
    // Drop 2 is wider: the lead, the counter, the arp and the pad by kClimaxWidth over their recipe's width,
    // which every other section writes back (the offset replaces the one before it).
    if (voices && !knobs) {
        for (PolyInstance v : { PolyInstance::Lead, PolyInstance::Counter, PolyInstance::Arp, PolyInstance::Pad }) {
            float off[poly::Count] = {};
            voiceRecipeOffsets(v, plan.voice[polyIndex(v)], sv, off);
            push(p.base(v) + poly::Width, off[poly::Width] + (s.climax ? kClimaxWidth : 0.0f), 0.0f);
        }
    }

    // The hall opens where the floor empties: a breakdown is the wettest part of a track.
    const float wet = s.type == SectionType::Break ? 0.22f : (s.type == SectionType::Intro || s.type == SectionType::Outro ? 0.10f : 0.0f);
    const ParamDesc& hs = p.desc(pb + poly::HallSend);
    const float wetNorm = wet / (hs.maxValue - hs.minValue);
    if (floor) push(ab + acid::HallSend, knobs ? 0.0f : wetNorm, length);
    // Every polyphonic voice, with the hall share of its recipe's space direction folded in (19.09.2026).
    for (int v = 0; v < kPolyInstances && voices; ++v) {
        const float space = kVoiceHallWeight * sv * kVoicePalette[v].scale[3] * plan.voice[v].macro[3];
        push(p.base(static_cast<PolyInstance>(v)) + poly::HallSend, knobs ? 0.0f : wetNorm + space, length);
    }

    // Loudness (Farbood): at most +-2 dB around the section's energy, on top of the track's level match.
    const ParamDesc& g = p.desc(mb + mix::TrackGain);
    const float span = g.maxValue - g.minValue;
    // The climax trims (Form.h, kDrop1HoldDb): drop 1 held under drop 2, the buildup into drop 2 ramping
    // down to leave it headroom. Read at the bar this call writes from (the incoming track's floor starts at
    // its hand-over, inside its intro) and at the section's end.
    const Section& whole = plan.form.section[bar.index];
    const double u0 = whole.bars > 1 ? static_cast<double>(s.startBar - whole.startBar) / whole.bars : 0.0;
    const float t0 = knobs ? 0.0f : sectionTrimDb(plan.form, bar.index, u0);
    const float t1 = knobs ? 0.0f : sectionTrimDb(plan.form, bar.index, 1.0);
    if (floor) push(mb + mix::TrackGain, (plan.gainDb + (knobs ? 0.0f : energyGainDb(e0)) + t0) / span, 0.0f);
    // A buildup's energy runs from the section before it to the drop, so the gain ramps with it; every
    // other section holds one value (e0 == e1 there).
    if (floor && (e1 != e0 || t1 != t0))
        push(mb + mix::TrackGain, (plan.gainDb + (knobs ? 0.0f : energyGainDb(e1)) + t1) / span, length);

    // The macro ride of this section and the buildup's hall send (Form.h, 16.09.2026). The only line
    // of this file the arrangement-dynamics round of 16.09.2026 added: everything it writes is made
    // in Form.cpp out of the section, its seed and the arc values computed just above.
    // Since 19.09.2026 the ride of resonance and decay swings around the track's voiced values (Form.cpp):
    // the voicing's offset and the old per-track direction, the same sum trackStartControls writes.
    const float acidRecipe = m.recipe[mpIndex(MelodyPart::Acid)];
    if (floor)
        sectionAutomation(p, s, plan.sectionSeed[std::clamp(bar.index, 0, kMaxSections - 1)], beat,
                          cutoffAt(acidBase, 1.0f, e0), cutoffAt(acidBase, 1.0f, e1), knobs, out,
                          voicing[acid::Resonance] + 0.08f * sv * acidRecipe, voicing[acid::Decay] + 0.12f * sv * acidRecipe);

    // The pad's trance gate is a property of the section, not of a 16-bar block.
    if (voices) pushNow(pb + poly::Gate, ControlEvent::Kind::Override, bar.padGate ? 1.0f : 0.0f);
    // The gate *pattern* is the section's since 22.09.2026, not the track's. One pattern for 256 bars
    // was the other half of "das Pad hat immer dasselbe gespielt": the pad's only movement under a
    // drop is the gate, and it ran the same sixteen steps for seven minutes. The track still draws
    // the two patterns (Melody.cpp, makePad: padGatePattern and a padGateAlt that is never the same
    // one); which of them a section takes is the section's own seed, so a set is still a function of
    // its seed and a locked section keeps its pattern.
    if (voices) {
        Rng gr;
        gr.seed(mixSeed(plan.sectionSeed[std::clamp(bar.index, 0, kMaxSections - 1)] ^ kSaltPadGate, 0));
        const bool alt = gr.uniform() < 0.4f;
        pushNow(pb + poly::GatePattern, ControlEvent::Kind::Override,
                static_cast<float>(alt ? m.padGateAlt : m.padGatePattern));
    }
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

double Composer::probeLoudness(const ParamStore& p, const TrackPlan& plan, int part, float masterGainDb, double* bands) const
{
    // The presence probe (19.09.2026, round "polish"; matchPresence): the drops as the form plays them, split
    // into the lines and the rest, so that the balance can be solved for the lines' gain without another render.
    const bool presence = part == kProbeLines || part == kProbeRest;
    const bool mixLike = part == -2 || presence;
    auto isLine = [](MelodyPart mp) {
        return mp == MelodyPart::Lead || mp == MelodyPart::Counter || mp == MelodyPart::Arp || mp == MelodyPart::Stab;
    };
    constexpr double sr = 48000.0;
    // Two bars for the foundation and the parts. For the whole mix eight bars spread evenly over the track,
    // as its blocks really play -- intro and outro included, since the gated integrated loudness counts
    // them: the densest block alone over-read a track by 0.8 LU, four bars from its middle by 0.6 LU.
    const int bars = part == -2 ? 16 : (presence ? 8 : 2);
    // The probe's inputs are collected first -- the parameter values, the controls, the notes -- and the
    // engine is built only when they have to be rendered (20.09.2026, round "speed"): with the probe cache
    // on (Probe.h) the same inputs may already have been rendered, by this process or another. The engine
    // receives exactly what it used to receive, in the same order: its two rings are independent, and
    // nothing is rendered before the last push.
    ParamStore ps;
    ps.copyValuesFrom(p);
    const int mb = p.base(Module::Mix);
    ps.set(mb + mix::KickMute, 0.0f);
    ps.set(mb + mix::BassMute, 0.0f);
    ps.set(mb + mix::TrackGain, 0.0f);
    ps.set(mb + mix::PercMute, 0.0f);
    // The foundation (part -1): kick, bass and percussion. A melodic part: that part alone. The whole mix
    // (part -2): everything with its corrections, through the master. The first two are measured before
    // the master's dynamics, which would bend the relation between gain and loudness.
    for (int k = 0; k < kMelodyParts; ++k) {
        const MelodyPart mp = static_cast<MelodyPart>(k);
        const int mute = mp == MelodyPart::Acid ? static_cast<int>(mix::AcidMute) : mix::polyMute(melodyPoly(mp));
        const bool on = k == part || part == -2 || (part == kProbeLines && isLine(mp)) || (part == kProbeRest && !isLine(mp));
        ps.set(mb + mute, on ? 0.0f : 1.0f);
    }
    ps.set(mb + mix::SfxMute, 1.0f);
    const int ms = p.base(Module::Master);
    if (part != -2) {
        ps.set(ms + master::CompRatio, 1.0f);
        ps.set(ms + master::Limiter, 0.0f);
        ps.set(ms + master::Clip, 0.0f);
        ps.set(ms + master::Clipper, 0.0f);
    }
    if (part >= 0 || part == kProbeLines) {
        ps.set(mb + mix::KickMute, 1.0f);
        ps.set(mb + mix::BassMute, 1.0f);
        ps.set(mb + mix::PercMute, 1.0f);
    }
    TempoMap tm;
    tm.setConstant(plan.bpm);

    std::vector<ControlEvent> controls;
    TrackPlan neutral = plan;
    neutral.masterGainDb = masterGainDb;
    // The presence probe keeps the parts' level corrections: the balance it measures is the one they make.
    if (!mixLike) {
        neutral.gainDb = 0.0f;
        neutral.presenceGainDb = 0.0f;
        for (float& g : neutral.partGainDb) g = 0.0f;
    }
    trackStartControls(p, neutral, 0.0, controls);
    arcControls(p, neutral, 0, 0.0, false, controls);

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
        if (presence) {
            // Four bars of drop 1 and four of drop 2 (the last drop, where a form has only one): the fifth bar
            // of each of their first four eight-bar groups. One per group, because the stab and the counter
            // take whole groups by turns (Form.cpp), and two bars from the middle heard one of them or neither
            // -- on the calibration tracks the probe then read the brightest tracks 2 dB low.
            int d1 = -1, d2 = -1;
            for (int i = 0; i < plan.form.count; ++i)
                if (plan.form.section[i].type == SectionType::Drop) { if (d1 < 0) d1 = i; d2 = i; }
            const Section& s = plan.form.section[std::max(0, b < 4 ? d1 : d2)];
            return s.startBar + std::min(s.bars - 1, 8 * (b % 4) + 4);
        }
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
        if (!mixLike) {
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
            // With the climax trim at the probe bar's place in its section (Form.h, kDrop1HoldDb).
            const double u = sec.bars > 1 ? static_cast<double>(sourceBar(b) - sec.startBar) / sec.bars : 0.0;
            c.value = (plan.gainDb + energyGainDb(0.5f * (sec.energy + sec.energyTo)) + sectionTrimDb(plan.form, bp.index, u))
                    / (g.maxValue - g.minValue);
            controls.push_back(c);
        }
        PercBarSpec spec;
        spec.layers = bp.percLayers;
        spec.fills = false;
        spec.hatsDense = bp.hatsDense;
        spec.rollBar = bp.rollBar;
        spec.rollBars = bp.rollBars;
        spec.pdb = bp.pdb;
        spec.cutBeats = bp.cutBeats;
        spec.quietHats = bp.quietHats;
        spec.shaker = bp.shaker;
        spec.offbeatHat = bp.offbeatHat;
        spec.hatLevel = bp.hatLevel;
        spec.openHats = bp.openHats;
        spec.ride = bp.ride;
        composePercBar(p, plan.perc, plan.percSeed, b, sourceBar(b), plan.bpm, plan.key, plan.scale, spec, notes);
    }
    // The melodic parts: everything the track has for their own measurement, as the form plays them
    // for the whole mix.
    if (part >= 0 || mixLike) {
        std::vector<NoteEvent> mel;
        for (int b = 0; b < bars; ++b) {
            const BarPlan bp = probeBar(sourceBar(b));
            // The drone plays one held chord per run (MelodyContext::droneBars), which a probe bar
            // taken out of the middle of a run would never start: the part probe holds it over its two
            // bars in the upper octave, the mix probe over each four-bar window in the octave the form has
            // there (19.09.2026).
            MelodyContext ctx;
            if (mixLike) {
                ctx = melodyContext(plan, bp, sourceBar(b));
                if (b % 4 == 0 && ctx.droneBars == 0 && (bp.parts & partBit(MelodyPart::Drone)) != 0) {
                    ctx.droneBars = 4;
                    ctx.droneLow = droneLowAt(plan, sourceBar(b));
                }
            } else if (b == 0) {
                ctx.droneBars = bars;
            }
            composeMelodyBar(p, plan.melody, b, mixLike ? sourceBar(b) : 48 + b, plan.scale, bp, mel, ctx);
        }
        for (const NoteEvent& n : mel) if (mixLike || n.part == melodyScorePart(static_cast<MelodyPart>(part))) notes.push_back(n);
    }
    // The presence probe hears each drop's own section controls -- the energy's filter arc, the section's drawn
    // wobble, drop 2's lift (Form.h, kClimaxOpen) -- at once rather than as the ramps a whole section plays them
    // over; without them the probe read the brighter tracks up to 2 dB low against the render (calibration, above
    // matchPresence). Only events inside the probe, in time order behind the track-start controls at beat 0.
    if (presence) {
        std::vector<ControlEvent> sc;
        for (int b = 0; b < bars; b += 4) sectionControls(p, neutral, probeBar(sourceBar(b)), static_cast<double>(b) * kBeatsPerBar, sc);
        std::vector<ControlEvent> keep;
        for (ControlEvent c : sc) {
            if (c.beat >= static_cast<double>(bars) * kBeatsPerBar) continue;
            c.length = 0.0f;
            keep.push_back(c);
        }
        std::stable_sort(keep.begin(), keep.end(), [](const ControlEvent& a, const ControlEvent& b) { return a.beat < b.beat; });
        for (const ControlEvent& c : keep) controls.push_back(c);
    }
    // One ring, so one order: the engine plays from the head of the ring.
    std::stable_sort(notes.begin(), notes.end(), noteLess);
    const int total = static_cast<int>(tm.secondsAt(bars * kBeatsPerBar) * sr);

    // The cache key: everything the render below depends on (Probe.h). Field by field, so that padding
    // bytes never enter it; with the counts, so that a note cannot pass for a control.
    const bool cached = probe::cacheEnabled();
    probe::Key key;
    if (cached) {
        // The shared data is hashed as it is loaded *now*, so it has to be loaded before the key is made and
        // not by the engine after it: "not loaded yet" and "the file is missing" would otherwise be one key
        // for two different renders. A flag test when it has happened (and it has, before any worker runs).
        probe::warmSharedData();
        probe::Hasher h;
        h.text("phos probe 1");   // the layout of this key
        h.text(probe::buildId());
        h.add(probe::sharedDataId());
        h.add(sr);
        h.add(static_cast<int32_t>(512));
        h.add(static_cast<int32_t>(total));
        h.add(static_cast<uint8_t>(bands != nullptr ? 1 : 0));
        h.add(plan.bpm);
        h.add(static_cast<int32_t>(ps.count()));
        for (int i = 0; i < ps.count(); ++i) h.add(ps.get(i));
        h.add(static_cast<uint64_t>(controls.size()));
        for (const ControlEvent& c : controls) { h.add(c.beat); h.add(c.length); h.add(c.value); h.add(c.param); h.add(c.kind); }
        h.add(static_cast<uint64_t>(notes.size()));
        for (const NoteEvent& n : notes) { h.add(n.beat); h.add(n.length); h.add(n.part); h.add(n.lane); h.add(n.pitch); h.add(n.velocity); h.add(n.flags); }
        key = h.finish();
        double v[3];
        if (probe::cacheLookup(key, v)) {
            if (bands != nullptr) { bands[0] = v[1]; bands[1] = v[2]; }
            return v[0];
        }
    }

    auto engine = std::make_unique<Engine>();
    engine->prepare(sr, 512);
    engine->params().copyValuesFrom(ps);
    engine->setTempoMap(tm);
    for (const ControlEvent& c : controls) engine->pushControl(c);
    for (const NoteEvent& n : notes) engine->pushEvent(n);
    LoudnessMeter meter;
    meter.prepare(sr);
    std::vector<float> L(512), R(512);
    // Band powers for the presence match: 1.5..6 kHz and 40..140 Hz, each a fourth-order band (two
    // second-order high passes and two low passes, Butterworth Q), the bands of Tools/metrics.py.
    Svf bandFilter[2][2][4];
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < 2; ++k) {
            bandFilter[0][c][k].setQ(1500.0f, 0.7071f, static_cast<float>(sr));
            bandFilter[0][c][2 + k].setQ(6000.0f, 0.7071f, static_cast<float>(sr));
            bandFilter[1][c][k].setQ(40.0f, 0.7071f, static_cast<float>(sr));
            bandFilter[1][c][2 + k].setQ(140.0f, 0.7071f, static_cast<float>(sr));
        }
    double bandSum[2] = {};
    for (int done = 0; done < total; done += 512) {
        const int n = std::min(512, total - done);
        engine->process(L.data(), R.data(), n);
        meter.process(L.data(), R.data(), n);
        if (bands == nullptr) continue;
        for (int c = 0; c < 2; ++c) {
            const float* x = c == 0 ? L.data() : R.data();
            for (int i = 0; i < n; ++i)
                for (int band = 0; band < 2; ++band) {
                    float y = x[i], lp, bp, hp;
                    for (int k = 0; k < 2; ++k) { bandFilter[band][c][k].tick(y, lp, bp, hp); y = hp; }
                    for (int k = 2; k < 4; ++k) y = bandFilter[band][c][k].lp(y);
                    bandSum[band] += static_cast<double>(y) * y;
                }
        }
    }
    if (bands != nullptr) { bands[0] = bandSum[0]; bands[1] = bandSum[1]; }
    const double integrated = meter.read().integrated;
    if (cached) {
        const double v[3] = { integrated, bandSum[0], bandSum[1] };
        probe::cacheStore(key, v);
    }
    return integrated;
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
    if (bar < t.firstBar + t.bars - kDjOverlap) return -1;
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
    if (consonant) composeMelodyBar(p, guest.melody, bar, inTrack, guest.scale, bp, out, melodyContext(guest, bp, inTrack));
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
        { const TrackPlan& t0 = track(p, ti); if (bar - t0.firstBar >= t0.bars - kDjOverlap) track(p, ti + 1); }
        const TrackPlan& plan = track(p, ti);
        // The incoming track of the DJ overlap, if one sounds in this bar (Form.h, kDjOverlap).
        const int incoming = ti + 1 < static_cast<int>(plans_.size()) && bar >= plans_[static_cast<size_t>(ti + 1)].firstBar ? ti + 1 : -1;
        const size_t barNotes = out.size();
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
