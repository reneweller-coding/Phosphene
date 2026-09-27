/**
 * @file ComposerControls.cpp
 * @brief The control events of a plan: a track's sound at its start, the sections' rides, the arcs, the drone's evolution, and the recipe offsets behind them.
 */
#include "phos/FieldLibrary.h"
#include "phos/FieldPresets.h"
#include <limits>
#include "phos/SoundPresets.h"
#include "phos/Composer.h"
#include "ComposerInternal.h"
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
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

#include "Salts.h"
using namespace phos::salts::composer;   // the Composer's seed salts (Salts.h)

namespace phos {

namespace {

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

/**
 * @brief One parameter of the acid voicings: its value in the clean and the liquid voicing.
 *
 * The driven voicing is the parameter table's default (Params.cpp, 18.09.2026), so it needs no column.
 * The values are the candidates the user heard on 18.09.2026 (docs/rounds/2026-09.md, "Fundament und Mix",
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
    // resonance of 0.8, liquid at 0.88 with a 500 ms decay (docs/rounds/2026-09.md, "Fundament und Mix").
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
    // Detune 0.25 -> 0.15 (24.09.2026, the user: "untersuche, ob es zu solchen Verstimmungen auch in den anderen
    // Stimmen kommen kann"). Szabo's curve is steep at the top: the lead's 0.55 put its outer saws 22 cents off, and a
    // thick recipe at full sound variation pushed it to 0.80 -- 54 cents, past a quarter tone, the pad's fault again.
    // At 0.15 the lead stays under 40 cents and every other voice under 25.
    { poly::Detune,      2, 0.15f }, { poly::Mix,         2, 0.15f }, { poly::Width,    2, 0.20f },
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
    // Osc2Detune 0.25 -> 0.06 (24.09.2026): a quarter of the knob's 100 cents put the partner up to 25 cents
    // from the pad's own 9, and seed 5's first pad played every chord tone twice, 31 cents apart and equally
    // loud -- the honky-tonk piano, not the width of an analogue pad (3 to 15 cents, where 9 +- 6 now lies).
    { poly::Osc2Mix,     2, 0.30f }, { poly::Osc2Detune,  2, 0.06f },
    { poly::LfoCutoff,   4, 0.35f }, { poly::LfoAmp,      4, 0.25f },
    { poly::LfoPitch,    4, 0.20f }, { poly::LfoBeats,    4, 0.30f },
};
/** @brief The hall-send share of the space direction, folded into the section's hall ride (sectionControls). */
constexpr float kVoiceHallWeight = 0.12f;

/** @brief Bass parameters that move in slow arcs within a track, with their arc size at full variation. */
struct Arc { int param; float size; };
const Arc kBassArcs[] = { { bass::Cutoff, 0.08f }, { bass::EnvAmount, 0.06f }, { bass::Resonance, 0.06f } };

} // namespace

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

Composer::VoicePaletteView Composer::voicePalette(PolyInstance voice)
{
    const VoicePalette& p = kVoicePalette[std::clamp(polyIndex(voice), 0, kPolyInstances - 1)];
    return { p.osc, p.builtin, p.lane, p.filter, p.osc2, p.interval };
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

const SoundPreset* Composer::presetOf(const ParamStore& p, const TrackPlan& plan, int synth)
{
    if (synth < 0 || synth >= kSoundSynths || plan.soundPreset[synth] < 0) return nullptr;
    if (p.get(p.base(Module::Compose) + compose::SoundVariation) <= 0.0f) return nullptr;   // Sound Variation 0: the knobs
    if (!p.getBool(p.base(Module::Compose) + compose::SoundPresets)) return nullptr;       // the recipes instead
    const Module m = synth == 0 ? Module::Kick : synth == 1 ? Module::Bass : synth == 2 ? Module::Acid : Module::Poly;
    const std::vector<SoundPreset>& bank = factoryPresets(m, synth >= 3 ? synth - 3 : 0);
    const size_t i = static_cast<size_t>(plan.soundPreset[synth]);
    return i < bank.size() ? &bank[i] : nullptr;
}

namespace {

/** @brief The share of a synth's recipe: none where the synth plays a preset (26.09.2026), Sound Variation otherwise. */
float recipeAmount(const ParamStore& p, const TrackPlan& plan, int synth, float sv)
{
    return Composer::presetOf(p, plan, synth) != nullptr ? 0.0f : sv;
}

/**
 * @brief The level correction of the preset synth @p synth plays (SoundPreset::trimDb), as an offset on its Level knob in
 *        the normalised domain; 0 without a preset. It rides in every offset the composer writes on that knob, since an
 *        offset replaces the one before it (26.09.2026: the presets' loudness spread 7 to 10 dB within a synth).
 */
float presetTrim(const ParamStore& p, const TrackPlan& plan, int synth, int levelId)
{
    const SoundPreset* sp = Composer::presetOf(p, plan, synth);
    if (sp == nullptr) return 0.0f;
    const ParamDesc& d = p.desc(levelId);
    return sp->trimDb / (d.maxValue - d.minValue);
}

/** @brief A knob of choices, steps or a switch: a preset's value reaches it as an override, not a base. */
bool isDiscreteCurve(Curve c) { return c == Curve::Choice || c == Curve::Int || c == Curve::Toggle; }


} // namespace

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
    // The sound presets (26.09.2026): first every value a preset of the track before may have left -- a base
    // cleared, an override lifted -- for the synths this scope starts; the recipe of a synth without a preset and
    // the preset of one with it follow, and win, because the events of one instant apply in their order.
    auto presetSynths = [&](auto&& each) {
        if (floor) for (int k = 0; k < 3; ++k) each(k);
        if (voices) for (int k = 3; k < kSoundSynths; ++k) each(k);
    };
    presetSynths([&](int k) {
        const Module m = k == 0 ? Module::Kick : k == 1 ? Module::Bass : k == 2 ? Module::Acid : Module::Poly;
        const int b = p.base(m, k >= 3 ? k - 3 : 0);
        for (int i = 0; i < ParamStore::moduleCount(m); ++i) {
            if (presetLeaves(m, i)) continue;
            if (isDiscreteCurve(p.desc(b + i).curve)) {
                push(b + i, ControlEvent::Kind::Override, -1.0f);
            } else {
                push(b + i, ControlEvent::Kind::Base, 0.0f);
                out.back().length = -1.0f;   // clears the base (Score.h)
            }
        }
    });
    const float svKick = recipeAmount(p, plan, 0, sv);
    // The DJ overlap (19.09.2026): a track's voices sound from its first bar, over the previous track's
    // bare outro, but its key, kick, bass, kit, acid and gains take over only at the hand-over, with its
    // kick and bass (ControlScope). A track that starts a set writes both at once.
    if (floor) {
    push(cb + compose::Key, ControlEvent::Kind::Override, static_cast<float>(plan.key));
    push(cb + compose::Scale, ControlEvent::Kind::Override, static_cast<float>(plan.scale));
    push(cb + compose::BassPattern, ControlEvent::Kind::Override, static_cast<float>(plan.primaryPattern));
    if (svKick > 0.0f || presetOf(p, plan, 0) == nullptr) {
        push(kb + kick::Engine, ControlEvent::Kind::Override, static_cast<float>(plan.kickEngine));
        push(kb + kick::Clip, ControlEvent::Kind::Override, static_cast<float>(plan.kickClip));
    }
    float kickOff[kick::Count] = {};   // sized by the table (26.09.2026: a fixed 64 is the Poly::values_ trap)
    recipeOffsets(true, plan.kickMacro, svKick, kickOff);
    kickOff[kick::Level] += presetTrim(p, plan, 0, kb + kick::Level);
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
    pushPartLevels(p, plan, beat, 0.0f, floor, voices, out);
    // The track's acid voicing (Composer.h, kNumAcidVoicings). Cutoff and Env Amount are written again
    // by every section with the voicing folded into the section's base (sectionControls); they are
    // written here as well so that the level match's part probe, which plays the track start alone,
    // hears the voicing it has to match.
    const float svAcid = recipeAmount(p, plan, 2, sv);
    if (floor) {
        float acidOff[acid::Count] = {};
        int disperse = -1;
        acidVoicingOffsets(p, plan.acidVoicing, svAcid, acidOff, disperse);
        for (const AcidVoicingParam& v : kAcidVoicingTable) push(ab + v.param, ControlEvent::Kind::Offset, acidOff[v.param]);
        push(ab + acid::Level, ControlEvent::Kind::Offset, presetTrim(p, plan, 2, ab + acid::Level));
        push(ab + acid::Disperse, ControlEvent::Kind::Override, static_cast<float>(disperse));
    }
    // Decay and resonance: the voicing's offset (pushed above) plus the old per-track direction, as one
    // value -- a strand holds an offset, it does not add one; the section ride swings around this sum.
    if (floor) {
        float acidOff[acid::Count] = {};
        int unused = -1;
        acidVoicingOffsets(p, plan.acidVoicing, svAcid, acidOff, unused);
        push(ab + acid::Decay, ControlEvent::Kind::Offset, acidOff[acid::Decay] + 0.12f * svAcid * m.recipe[mpIndex(MelodyPart::Acid)]);
        push(ab + acid::Resonance, ControlEvent::Kind::Offset, acidOff[acid::Resonance] + 0.08f * svAcid * m.recipe[mpIndex(MelodyPart::Acid)]);
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
        const float svVoice = recipeAmount(p, plan, 3 + v, sv);
        const bool vary = svVoice > 0.0f;
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
        voiceRecipeOffsets(inst, rc, svVoice, off);
        for (const Loading& l : kVoiceLoadings) push(vb + l.param, ControlEvent::Kind::Offset, off[l.param]);
        push(vb + poly::Level, ControlEvent::Kind::Offset, presetTrim(p, plan, 3 + v, vb + poly::Level));
    }
    if (voices) push(p.base(PolyInstance::Pad) + poly::GatePattern, ControlEvent::Kind::Override, static_cast<float>(m.padGatePattern));
    // The timbral counter (23.09.2026, round "Counter"; Form.h, CounterMode): a texture needs a sustained
    // envelope and a portamento, which the counter's recipe -- the whip of the dialogue round -- does not
    // have. Written as offsets from the knobs' values, so a track in another mode plays the knobs exactly.
    if (voices) {
        const int nb = p.base(PolyInstance::Counter);
        const bool timbral = m.counterMode == static_cast<int>(CounterMode::Timbral);
        const SoundPreset* counterPreset = presetOf(p, plan, 3 + polyIndex(PolyInstance::Counter));
        auto towards = [&](int local, float target) {
            const int id = nb + local;
            // From the value that plays: the preset's where one does (26.09.2026), the knob's otherwise.
            float from = p.get(id);
            if (counterPreset != nullptr)
                for (const auto& kv : counterPreset->values) if (kv.first == local) { from = kv.second; break; }
            push(id, ControlEvent::Kind::Offset, timbral ? p.toNormalised(id, target) - p.toNormalised(id, from) : 0.0f);
        };
        towards(poly::AmpSustain, 0.75f);
        towards(poly::AmpDecay, 400.0f);
        towards(poly::AmpRelease, 220.0f);
        towards(poly::FilterDecay, 400.0f);
        towards(poly::Glide, 60.0f);
    }
    // The loudness offset of Auto Gain, in the normalised domain of master.gain's 36 dB range.
    const ParamDesc& mg = p.desc(p.base(Module::Master) + master::Gain);
    if (floor) push(p.base(Module::Master) + master::Gain, ControlEvent::Kind::Offset, plan.masterGainDb / (mg.maxValue - mg.minValue));
    // The Field track's place (27.09.2026; FieldPresets.h), with the voices: with layer A's category on Auto the
    // composer plays a preset drawn by the track's style -- every knob of the module as a base or an override, so the
    // knobs stay as the user left them -- and asks the library for its recordings now, a track ahead of their first
    // note. A category the user chose is theirs: whatever a track before set is cleared, and the knobs play.
    if (voices) {
        const int fb = p.base(Module::Field);
        for (int i = 0; i < field::Count; ++i) {
            if (isDiscreteCurve(p.desc(fb + i).curve)) push(fb + i, ControlEvent::Kind::Override, -1.0f);
            else { push(fb + i, ControlEvent::Kind::Base, 0.0f); out.back().length = -1.0f; }
        }
        const int fp = p.getInt(fb + field::ACategory) == 0 ? fieldPresetFor(plan.style, mixSeed(plan.formSeed, 0x4649454C44ull)) : -1;
        if (fp >= 0) {
            const FieldPreset& preset = fieldPresets()[static_cast<size_t>(fp)];
            for (const auto& kv : fieldPresetValues(p, preset)) {
                const int id = fb + kv.first;
                push(id, isDiscreteCurve(p.desc(id).curve) ? ControlEvent::Kind::Override : ControlEvent::Kind::Base, kv.second);
            }
            preloadFieldClip(fieldClipIndex(preset.categoryA, fieldPresetVariation(preset.categoryA, preset.fileA, preset.variationA)));
            if (preset.categoryB >= 0)
                preloadFieldClip(fieldClipIndex(preset.categoryB, fieldPresetVariation(preset.categoryB, preset.fileB, preset.variationB)));
        }
    }
    // The track's presets (26.09.2026), last, so they win over what the clearing and the recipes wrote at this beat:
    // every value a preset sets, the continuous ones as bases, the discrete ones as overrides.
    presetSynths([&](int k) {
        const SoundPreset* sp = presetOf(p, plan, k);
        if (sp == nullptr) return;
        const Module m = k == 0 ? Module::Kick : k == 1 ? Module::Bass : k == 2 ? Module::Acid : Module::Poly;
        const int b = p.base(m, k >= 3 ? k - 3 : 0);
        for (const auto& kv : sp->values) {
            const int id = b + kv.first;
            push(id, isDiscreteCurve(p.desc(id).curve) ? ControlEvent::Kind::Override : ControlEvent::Kind::Base, kv.second);
        }
    });
}

void Composer::pushPartLevels(const ParamStore& p, const TrackPlan& plan, double beat, float length, bool floor, bool voices,
                              std::vector<ControlEvent>& out) const
{
    const int mb = p.base(Module::Mix);
    for (int k = 0; k < kMelodyParts; ++k) {
        const MelodyPart part = static_cast<MelodyPart>(k);
        if (part == MelodyPart::Acid ? !floor : !voices) continue;
        const int level = mb + (part == MelodyPart::Acid ? static_cast<int>(mix::AcidLevel) : mix::polyLevel(melodyPoly(part)));
        const ParamDesc& d = p.desc(level);
        // The lines carry the presence match on top of their level match (matchPresence).
        const bool line = part == MelodyPart::Lead || part == MelodyPart::Counter || part == MelodyPart::Arp || part == MelodyPart::Stab;
        ControlEvent c;
        c.beat = beat;
        c.param = static_cast<int16_t>(level);
        c.kind = ControlEvent::Kind::Offset;
        c.value = (plan.partGainDb[k] + (line ? plan.presenceGainDb : 0.0f)) / (d.maxValue - d.minValue);
        c.length = length;
        out.push_back(c);
    }
}

void Composer::levelControls(const ParamStore& p, const TrackPlan& plan, double beat, float rampBeats, std::vector<ControlEvent>& out) const
{
    pushPartLevels(p, plan, beat, rampBeats, true, true, out);
}

/**
 * @brief The mix by phase (25.09.2026, the mix guide's "Mix nach Phase"): the same four quantities -- depth,
 *        width, ducking and level -- set differently in every section, because a track's effect is the
 *        contrast between its phases.
 *
 * All values are offsets on the voice's own (recipe) value, in the knob's normalised units unless said
 * otherwise:
 *  - **padWidth**: the pad narrow in the DJ's intro and outro and at the end of a buildup, at its own width in a
 *    drop and a breakdown. The guide narrows a drop's pad to 60 %; the user's recordings are wider than the guide
 *    (channel correlation 0.88 against Phosphene's 0.91 .. 0.96, Tools/mix_audit.py), and with the far room muted
 *    in a drop a narrowed pad took the mix's mid band under the recordings' lower quartile (testStereoWidth). A
 *    buildup still narrows to 60 % over its length, and the drop opens from there -- "narrow before expanding".
 *  - **leadDistance**: the lead goes back in the breakdown and comes to the front in the drop -- on poly.distance,
 *    so level, low pass, wet and width move together (the Dark-Ambient addon: a voice far by one cue and near by
 *    another sticks to the speakers). 0.6 is about -8 dB, a low pass near 10 kHz and a third of the send wet.
 *  - **padLevelDb**: the pad forward in the breakdown, the one phase where the back plane leads (+3 dB there in
 *    the guide; +1.5 here, because the whole break stands kBreakTrimDb lower as well).
 *  - **padDuck**: the pad's and the drone's duck deeper in a drop (6 .. 8 dB) and shallower in the DJ parts.
 *  - **rampBars**: how long the change takes -- one bar into a drop, four into a breakdown (the guide: 1 .. 2
 *    and 4 .. 8), the buildup its whole length.
 */
struct PhaseMix { float padWidth, leadDistance, padLevelDb, padDuck, rampBars; };

/** @brief The phase values of a section type (see PhaseMix). */
PhaseMix phaseMix(SectionType t)
{
    switch (t) {
    case SectionType::Intro:  return { -0.40f, 0.0f, 0.0f, -0.10f, 4.0f };
    case SectionType::Groove: return { -0.30f, 0.0f, 0.0f,  0.00f, 2.0f };
    case SectionType::Build:  return { -0.40f, 0.0f, 0.0f,  0.00f, 0.0f };   // ramp: the build's length
    case SectionType::Drop:   return {  0.00f, 0.0f, 0.0f,  0.10f, 1.0f };
    case SectionType::Break:  return {  0.00f, 0.6f, 1.5f,  0.00f, 4.0f };
    case SectionType::Outro:  return { -0.50f, 0.0f, 0.0f, -0.10f, 8.0f };
    default:                  return {  0.00f, 0.0f, 0.0f,  0.00f, 2.0f };
    }
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
float Composer::leadCutoffValue(const ParamStore& p, const TrackPlan& plan, const BarPlan& bar) const
{
    // Mirrors the lead's cutoff in sectionControls below -- base, energy, wobble, drop 2's lift -- at the
    // energy of *this bar* (BarPlan::energy is the section's ramp at the bar), so a per-bar event can
    // stand in for the section's ramp without a step. The two must stay the same formula.
    const int cb = p.base(Module::Compose);
    const float sv = p.get(cb + compose::SoundVariation), mv = p.get(cb + compose::MelodyVariation);
    if (plan.index == 0 && bar.index == 0) return 0.0f;   // the knobs, exactly
    const Section& s = plan.form.section[std::clamp(bar.index, 0, kMaxSections - 1)];
    Rng r;
    r.seed(mixSeed(plan.sectionSeed[std::clamp(bar.index, 0, kMaxSections - 1)] ^ kSaltArc, 0));
    const float wobble = (2.0f * r.uniform() - 1.0f);
    float leadOff[poly::Count] = {};
    const float svLead = recipeAmount(p, plan, 3 + polyIndex(PolyInstance::Lead), sv);
    voiceRecipeOffsets(PolyInstance::Lead, plan.voice[polyIndex(PolyInstance::Lead)], svLead, leadOff);
    const float leadBase = 0.10f * svLead * plan.melody.recipe[mpIndex(MelodyPart::Lead)] + leadOff[poly::Cutoff];
    const float open = s.climax ? kClimaxOpen : 0.0f;
    return leadBase + 0.8f * (0.35f * (bar.energy - 0.7f) + 0.12f * mv * wobble) + open;
}

void Composer::leadArcControls(const ParamStore& p, const TrackPlan& plan, const BarPlan& bar, int inTrack, double beat,
                               std::vector<ControlEvent>& out) const
{
    // The phrase's filter arc (22.09.2026, round "Lead"; MelodyLead.cpp, kLeadArchetypes). The brief: "die
    // halbe Melodie ist Modulation" -- the archetype's contour gets a timbral carrier, a cutoff curve
    // over the eight bars of the phrase, written as one-bar ramps around the section's own value, so
    // the arc rides on the energy arc instead of replacing it. Only where the lead plays; the first
    // section of the first track keeps the knobs.
    if (plan.index == 0 && bar.index == 0) return;
    const MelodyPlan& m = plan.melody;
    if (!m.present[mpIndex(MelodyPart::Lead)] || (bar.parts & partBit(MelodyPart::Lead)) == 0) return;
    const int window = (inTrack / 8) % 2;
    const float depth = styleProfile(static_cast<StyleId>(plan.style)).lead.arcDepth;
    ControlEvent c;
    c.beat = beat;
    c.param = static_cast<int16_t>(p.base(PolyInstance::Lead) + poly::Cutoff);
    c.kind = ControlEvent::Kind::Offset;
    c.value = leadCutoffValue(p, plan, bar) + depth * static_cast<float>(leadArc(m.leadArchetype[window], inTrack % 8));
    c.length = static_cast<float>(kBeatsPerBar);
    out.push_back(c);
}

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
    const float svAcid = recipeAmount(p, plan, 2, sv);
    acidVoicingOffsets(p, plan.acidVoicing, svAcid, voicing, disperseUnused);
    const float acidBase = 0.10f * svAcid * m.recipe[mpIndex(MelodyPart::Acid)] + voicing[acid::Cutoff];
    // The lead's cutoff and the pad's table position ride on their voice recipe's offset (19.09.2026).
    float leadOff[poly::Count] = {}, padOff[poly::Count] = {};
    voiceRecipeOffsets(PolyInstance::Lead, plan.voice[polyIndex(PolyInstance::Lead)], sv, leadOff);
    voiceRecipeOffsets(PolyInstance::Pad, plan.voice[polyIndex(PolyInstance::Pad)], sv, padOff);
    const bool knobs = plan.index == 0 && bar.index == 0;
    const PhaseMix phase = phaseMix(s.type);
    const float phaseRamp = s.type == SectionType::Build ? length : phase.rampBars * static_cast<float>(kBeatsPerBar);
    const float leadBase = 0.10f * sv * m.recipe[mpIndex(MelodyPart::Lead)] + leadOff[poly::Cutoff];
    const float padBase = 0.25f * sv * m.recipe[mpIndex(MelodyPart::Pad)] + padOff[poly::Position];
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
    // which every other section writes back (the offset replaces the one before it). The pad's width follows
    // the phase besides (PhaseMix), over the phase's ramp.
    if (voices && !knobs) {
        for (PolyInstance v : { PolyInstance::Lead, PolyInstance::Counter, PolyInstance::Arp, PolyInstance::Pad }) {
            float off[poly::Count] = {};
            voiceRecipeOffsets(v, plan.voice[polyIndex(v)], recipeAmount(p, plan, 3 + polyIndex(v), sv), off);
            const bool pad = v == PolyInstance::Pad;
            push(p.base(v) + poly::Width, off[poly::Width] + (s.climax ? kClimaxWidth : 0.0f) + (pad ? phase.padWidth : 0.0f),
                 pad ? phaseRamp : 0.0f);
        }
        // The pad forward in a breakdown, on top of its level match; the pad's and the drone's duck by phase.
        const int k = mpIndex(MelodyPart::Pad);
        const ParamDesc& lv = p.desc(mb + mix::polyLevel(PolyInstance::Pad));
        push(mb + mix::polyLevel(PolyInstance::Pad), (plan.partGainDb[k] + phase.padLevelDb) / (lv.maxValue - lv.minValue), phaseRamp);
        push(pb + poly::Duck, phase.padDuck, phaseRamp);
        push(p.base(PolyInstance::Drone) + poly::Duck, phase.padDuck, phaseRamp);
        // The buildup pulls the lines up from the bottom (the guide: the melodic sum's high pass climbs to
        // 150 .. 250 Hz, "Sub" gone before the drop): 180 Hz over their own floor by its last bar, back at once
        // wherever else a section starts. The pad and the drone keep theirs -- the sub foundation rides it.
        for (PolyInstance v : { PolyInstance::Lead, PolyInstance::Counter, PolyInstance::Arp, PolyInstance::Stab }) {
            const int id = p.base(v) + poly::HpFloor;
            const float cur = p.get(id);
            const float target = s.type == SectionType::Build ? std::min(cur + 180.0f, p.desc(id).maxValue) : cur;
            push(id, p.toNormalised(id, target) - p.toNormalised(id, cur), s.type == SectionType::Build ? length : 0.0f);
        }
    }
    // The rooms by phase (25.09.2026, the Dark-Ambient addon's architecture). The plate (B) is the main room:
    // 1.5 s in a drop, 4 s in a breakdown, pulled down to 1.2 s over a buildup so its last bars are nearly dry (the
    // guide's "Fallhoehe"). The hall (C) is the far room, fed from the plate (fx.plate_to_hall) and the bed: open in a
    // breakdown, a quarter of it (-12 dB) in the intro, faded out over a buildup and muted everywhere else -- "im
    // Drop existieren nur A und B". Both over the phase's ramp, a buildup over its length.
    if (floor && !knobs) {
        const int fb = p.base(Module::Fx);
        const int decay = fb + fx::PlateDecay, ret = fb + fx::HallReturn;
        const float curD = p.get(decay), curR = p.get(ret), rMin = p.desc(ret).minValue;
        const bool brk = s.type == SectionType::Break, build = s.type == SectionType::Build;
        const float ramp = build ? length : phaseRamp;
        const float dTarget = brk ? std::max(curD, 4.0f) : (build ? std::min(curD, 1.2f) : curD);
        push(decay, p.toNormalised(decay, dTarget) - p.toNormalised(decay, curD), ramp);
        const float rTarget = brk ? curR : (s.type == SectionType::Intro ? std::max(rMin, curR - 12.0f) : rMin);
        push(ret, p.toNormalised(ret, rTarget) - p.toNormalised(ret, curR), ramp);
    }

    // The plate opens where the floor empties: a breakdown is the wettest part of a track (pads 30 .. 50 %, the
    // middle plane more than its drop's 15 .. 25 %). The lead is not in this list: its wet share is its distance's.
    const float wet = s.type == SectionType::Break ? 0.15f : (s.type == SectionType::Intro || s.type == SectionType::Outro ? 0.05f : 0.0f);
    if (floor) push(ab + acid::PlateSend, knobs ? 0.0f : wet, phaseRamp);
    // Every other polyphonic voice, with the share of its recipe's space direction folded in (19.09.2026; into the
    // plate since the addon, where the hall was the one room).
    for (int v = 0; v < kPolyInstances && voices; ++v) {
        if (static_cast<PolyInstance>(v) == PolyInstance::Lead) continue;
        const float space = kVoiceHallWeight * recipeAmount(p, plan, 3 + v, sv) * kVoicePalette[v].scale[3] * plan.voice[v].macro[3];
        push(p.base(static_cast<PolyInstance>(v)) + poly::PlateSend, knobs ? 0.0f : wet + space, phaseRamp);
    }
    if (voices && !knobs) {
        // The lead's distance by phase (PhaseMix), and the counter's approach: in the main breakdown, where the
        // user's Model 3 introduces it alone, it starts far and comes to its own plane over sixteen bars with every
        // cue at once -- the addon's "Annaeherung ueber 8 .. 16 Takte im Break".
        push(lb + poly::Distance, phase.leadDistance, phaseRamp);
        const int cd = p.base(PolyInstance::Counter) + poly::Distance;
        if (s.type == SectionType::Break && bar.mainBreak && m.present[mpIndex(MelodyPart::Counter)]) {
            push(cd, 0.5f, 0.0f);
            push(cd, 0.0f, std::min(length, 16.0f * static_cast<float>(kBeatsPerBar)));
        } else {
            push(cd, 0.0f, 0.0f);
        }
        // Held pads without a beating cloud (the addon): in the intro and the breakdown, where the pad and the drone
        // are often the only tonal voices for 16 .. 32 bars, their unison stays within about 6 cents (Szabo's curve
        // at 0.15) and the second oscillator within 3 -- two voices 15 cents apart beat at 4 Hz at 440 Hz, and a
        // stack of them flickers in a way no EQ reaches. Everywhere else the recipe's own detune.
        const bool held = s.type == SectionType::Intro || s.type == SectionType::Break;
        for (PolyInstance v : { PolyInstance::Pad, PolyInstance::Drone }) {
            float off[poly::Count] = {};
            voiceRecipeOffsets(v, plan.voice[polyIndex(v)], recipeAmount(p, plan, 3 + polyIndex(v), sv), off);
            const int det = p.base(v) + poly::Detune, o2 = p.base(v) + poly::Osc2Detune;
            float dOff = off[poly::Detune], oOff = off[poly::Osc2Detune];
            if (held) {
                dOff = std::min(dOff, 0.15f - p.get(det));
                const float span = p.desc(o2).maxValue - p.desc(o2).minValue;
                const float cents = p.get(o2) + oOff * span;
                oOff += (std::clamp(cents, -3.0f, 3.0f) - cents) / span;
            }
            push(det, dOff, phaseRamp);
            push(o2, oOff, phaseRamp);
        }
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
    // the two patterns (MelodyParts.cpp, makeRangesAndPad: padGatePattern and a padGateAlt that is never the same
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
    voiceRecipeOffsets(PolyInstance::Drone, plan.voice[polyIndex(PolyInstance::Drone)],
                       recipeAmount(p, plan, 3 + polyIndex(PolyInstance::Drone), sv), base);
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
    float bassOff[bass::Count] = {};   // sized by the table, see kickOff
    recipeOffsets(false, plan.bassMacro, recipeAmount(p, plan, 1, sv), bassOff);
    bassOff[bass::Level] += presetTrim(p, plan, 1, bb + bass::Level);
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

} // namespace phos
