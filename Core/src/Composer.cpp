/**
 * @file Composer.cpp
 * @brief Track plans, sound recipes, bars of kick and bass, and the conductor.
 */
#include "phos/Composer.h"
#include "phos/Dsp.h"
#include "phos/Engine.h"
#include "phos/Harmony.h"
#include "phos/Loudness.h"
#include "phos/Params.h"
#include "phos/Patterns.h"
#include <algorithm>
#include <cmath>

namespace phos {

const char* const kKickMacroNames[kNumKickMacros] = { "length", "punch", "body", "grit", "click" };
const char* const kBassMacroNames[kNumBassMacros] = { "brightness", "pluck", "squelch", "grit", "weight" };

namespace {

constexpr uint64_t kSaltTrack  = 0x545241434B000001ull;
constexpr uint64_t kSaltRecipe = 0x5245434950450002ull;
constexpr uint64_t kSaltBlock  = 0x424C4F434B000003ull;
constexpr uint64_t kSaltPhrase = 0x5048524153450004ull;
constexpr uint64_t kSaltArc    = 0x4152430000000005ull;
constexpr uint64_t kSaltPerc   = 0x5045524300000006ull;
constexpr uint64_t kSaltMelody = 0x4D454C4F44590007ull;

/** @brief One move of a perceptual direction: parameter (module table index), direction, weight. */
struct Loading { int param; int macro; float weight; };

// Kick: length, punch, body, grit, click. Weights in normalised knob units at full variation.
const Loading kKickLoadings[] = {
    { kick::AmpHold,    0,  0.20f }, { kick::AmpDecay,   0,  0.30f }, { kick::PitchDecay, 0,  0.10f },
    { kick::Punch,      1,  0.30f }, { kick::PunchDecay, 1, -0.20f }, { kick::PitchStart, 1,  0.20f },
    { kick::PitchDecay, 2,  0.25f }, { kick::Tone,       2,  0.12f }, { kick::PitchStart, 2, -0.10f },
    { kick::Drive,      3,  0.35f }, { kick::ClickLevel, 3,  0.08f }, { kick::Level,      3, -0.05f },
    { kick::ClickLevel, 4,  0.25f }, { kick::ClickTone,  4,  0.25f }, { kick::ClickDecay, 4,  0.15f },
};
// Bass: brightness, pluck, squelch, grit, weight.
const Loading kBassLoadings[] = {
    { bass::Cutoff,      0,  0.12f }, { bass::EnvAmount,   0,  0.15f },
    { bass::FilterDecay, 1, -0.20f }, { bass::AmpDecay,    1, -0.15f }, { bass::AmpSustain, 1, -0.20f },
    { bass::Resonance,   2,  0.30f },
    { bass::Drive,       3,  0.30f }, { bass::Wave,        3,  0.25f }, { bass::PulseWidth, 3,  0.10f }, { bass::Level, 3, -0.03f },
    { bass::Sub,         4,  0.20f }, { bass::SplitRatio,  4,  0.15f }, { bass::KeyTrack,   4, -0.10f },
};

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

/** @brief Longest gate that still lets the lowest note's release finish before the next slot. */
float gateLimit(int pattern, double bpm, int lowestNote, float releaseKnobMs)
{
    const double f0 = midiToHz(lowestNote);
    const double release = std::max(releaseKnobMs * 0.001, 0.5 / f0);   // the bass's release floor
    const double slot = shortestBassSlot(pattern) * 60.0 / bpm;
    return static_cast<float>(1.0 - (release + 0.004) / slot);
}

} // namespace

void Composer::recipeOffsets(bool kickModule, const float* macros, float amount, float* out)
{
    const int n = ParamStore::moduleCount(kickModule ? Module::Kick : Module::Bass);
    std::fill(out, out + n, 0.0f);
    const Loading* table = kickModule ? kKickLoadings : kBassLoadings;
    const size_t count = kickModule ? sizeof(kKickLoadings) / sizeof(Loading) : sizeof(kBassLoadings) / sizeof(Loading);
    for (size_t i = 0; i < count; ++i) out[table[i].param] += amount * table[i].weight * macros[table[i].macro];
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
    if (knobs != planKnobs_) { plans_.clear(); planKnobs_ = std::move(knobs); }
}

TrackPlan Composer::makeTrack(const ParamStore& p, int index) const
{
    const int cb = p.base(Module::Compose);
    const float tv = p.get(cb + compose::TrackVariation);
    const float sv = p.get(cb + compose::SoundVariation);
    const double baseBpm = p.get(cb + compose::Bpm);
    const double range = p.get(cb + compose::TempoRange);
    const int baseBars = std::max(32, (p.getInt(cb + compose::TrackBars) / 16) * 16);
    const int reg = p.getInt(cb + compose::BassRegister);
    const float releaseKnob = p.get(p.base(Module::Bass) + bass::AmpRelease);

    TrackPlan t;
    t.index = index;
    t.percSeed = mixSeed(seed_ ^ kSaltPerc, static_cast<uint64_t>(index));
    t.perc = makePercPlan(p, t.percSeed, index == 0);
    if (index == 0) {
        t.firstBar = 0;
        t.bars = baseBars;
        t.key = p.getInt(cb + compose::Key);
        t.scale = p.getInt(cb + compose::Scale);
        t.bpm = baseBpm;
        t.primaryPattern = std::clamp(p.getInt(cb + compose::BassPattern), 0, kNumBassPatterns - 1);
        t.secondaryPattern = t.primaryPattern == 0 ? 1 : 0;
        t.gate = p.get(cb + compose::BassGate);
        t.melodySeed = mixSeed(seed_ ^ kSaltMelody, 0);
        t.melody = makeMelodyPlan(p, t.melodySeed, t.key, t.scale, t.bars, true);
        if (p.getBool(cb + compose::LevelMatch)) {
            t.loudness = probeLoudness(p, t);
            for (int k = 0; k < kMelodyParts; ++k) t.partLoudness[k] = probeLoudness(p, t, k);
        }
        matchMaster(p, t);
        return t;   // the first track is the knobs, exactly
    }

    const TrackPlan& prev = plans_[static_cast<size_t>(index - 1)];
    Rng r;
    r.seed(mixSeed(seed_ ^ kSaltTrack, static_cast<uint64_t>(index)));
    t.firstBar = prev.firstBar + prev.bars;

    // Length: the knob, give or take up to two 16-bar blocks.
    t.bars = baseBars;
    if (r.uniform() < tv) t.bars = std::max(32, baseBars + 16 * (r.below(5) - 2));

    // Key: by fifths and whole tones, now and then a semitone.
    t.key = prev.key;
    if (r.uniform() < tv) {
        static const int kMoves[6] = { 7, 5, 2, -2, 1, -1 };
        static const double kWeights[6] = { 0.3, 0.3, 0.15, 0.15, 0.05, 0.05 };
        t.key = ((prev.key + kMoves[pick(r, kWeights, 6)]) % 12 + 12) % 12;
    }
    // Mode: mostly kept; when it changes, the dark ones are likelier.
    t.scale = prev.scale;
    if (r.uniform() < 0.25f * tv) {
        static const double kWeights[kNumScales] = { 0.25, 0.30, 0.15, 0.15, 0.10, 0.05 };
        t.scale = pick(r, kWeights, kNumScales);
    }
    // Tempo: mean-reverting walk around the knob, inside the range, on half-BPM steps.
    const double walk = baseBpm + 0.6 * (prev.bpm - baseBpm) + (2.0 * r.uniform() - 1.0) * range * tv;
    t.bpm = std::round(std::clamp(walk, baseBpm - range, baseBpm + range) * 2.0) * 0.5;

    // Patterns.
    t.primaryPattern = prev.primaryPattern;
    if (r.uniform() < tv) {
        static const double kWeights[kNumBassPatterns] = { 0.45, 0.20, 0.12, 0.08, 0.15 };
        t.primaryPattern = pick(r, kWeights, kNumBassPatterns);
    }
    {
        double w[kNumBassPatterns] = { 0.40, 0.30, 0.20, 0.05, 0.05 };
        w[t.primaryPattern] = 0.0;
        t.secondaryPattern = pick(r, w, kNumBassPatterns);
    }

    // Gate, within what lets the lowest note (the seventh below the root) finish its release.
    const int root = bassRootNote(t.key, reg);
    const int lowest = root + scaleDegree(t.scale, 6) - 12;
    const float gateMax = std::min(gateLimit(t.primaryPattern, t.bpm, lowest, releaseKnob),
                                   gateLimit(t.secondaryPattern, t.bpm, lowest, releaseKnob));
    t.gate = std::clamp(p.get(cb + compose::BassGate) + (2.0f * r.uniform() - 1.0f) * 0.12f * tv, 0.35f, std::max(0.35f, gateMax));

    // Kick character switches.
    t.kickEngine = -1;
    if (r.uniform() < 0.15f * sv) t.kickEngine = 1 - p.getInt(p.base(Module::Kick) + kick::Engine);
    t.kickClip = r.uniform() < 0.3f * sv ? 1 : -1;

    // Recipes by best candidate against the last four tracks.
    Rng rr;
    rr.seed(mixSeed(seed_ ^ kSaltRecipe, static_cast<uint64_t>(index)));
    auto chooseRecipe = [&](float* out, int n, bool kickSide) {
        float best[kNumKickMacros] = {};
        double bestScore = -1.0;
        for (int c = 0; c < 12; ++c) {
            float cand[kNumKickMacros] = {};
            drawRecipe(rr, cand, n);
            double score = 1e9;
            for (int back = 1; back <= 4 && index - back >= 0; ++back) {
                const TrackPlan& o = plans_[static_cast<size_t>(index - back)];
                const float* om = kickSide ? o.kickMacro : o.bassMacro;
                double d = 0.0;
                for (int i = 0; i < n; ++i) d += (cand[i] - om[i]) * (cand[i] - om[i]);
                score = std::min(score, std::sqrt(d));
            }
            if (score > bestScore) { bestScore = score; std::copy(cand, cand + n, best); }
        }
        std::copy(best, best + n, out);
    };
    chooseRecipe(t.kickMacro, kNumKickMacros, true);
    chooseRecipe(t.bassMacro, kNumBassMacros, false);

    t.melodySeed = mixSeed(seed_ ^ kSaltMelody, static_cast<uint64_t>(index));
    t.melody = makeMelodyPlan(p, t.melodySeed, t.key, t.scale, t.bars, false);

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
    const int ab = p.base(Module::Acid), lb = p.base(Module::Poly, 0), rb = p.base(Module::Poly, 1);
    push(ab + acid::Squelch, ControlEvent::Kind::Override, static_cast<float>(m.acidSquelch));
    push(lb + poly::Osc, ControlEvent::Kind::Override, static_cast<float>(m.leadOsc));
    const int delayBase[3][2] = { { ab + acid::DelayLeft, ab + acid::DelayRight }, { lb + poly::DelayLeft, lb + poly::DelayRight },
                                             { rb + poly::DelayLeft, rb + poly::DelayRight } };
    const int levelParam[kMelodyParts] = { mb + mix::AcidLevel, mb + mix::LeadLevel, mb + mix::ArpLevel, mb + mix::PadLevel };
    const int pb = p.base(Module::Poly, 2);
    for (int k = 0; k < kMelodyParts; ++k) {
        if (k < 3) {
            push(delayBase[k][0], ControlEvent::Kind::Override, static_cast<float>(m.delay[k][0]));
            push(delayBase[k][1], ControlEvent::Kind::Override, static_cast<float>(m.delay[k][1]));
        }
        const ParamDesc& d = p.desc(levelParam[k]);
        push(levelParam[k], ControlEvent::Kind::Offset, plan.partGainDb[k] / (d.maxValue - d.minValue));
    }
    push(ab + acid::Decay, ControlEvent::Kind::Offset, 0.12f * sv * m.recipe[0]);
    push(ab + acid::Resonance, ControlEvent::Kind::Offset, 0.08f * sv * m.recipe[0]);
    push(lb + poly::Detune, ControlEvent::Kind::Offset, 0.15f * sv * m.recipe[1]);
    push(lb + poly::Cutoff, ControlEvent::Kind::Offset, 0.10f * sv * m.recipe[1]);
    push(rb + poly::FilterDecay, ControlEvent::Kind::Offset, 0.15f * sv * m.recipe[2]);
    push(rb + poly::Detune, ControlEvent::Kind::Offset, 0.12f * sv * m.recipe[2]);
    push(pb + poly::Position, ControlEvent::Kind::Offset, 0.25f * sv * m.recipe[3]);
    push(pb + poly::GatePattern, ControlEvent::Kind::Override, static_cast<float>(m.padGatePattern));
    // The loudness offset of Auto Gain, in the normalised domain of master.gain's 36 dB range.
    const ParamDesc& mg = p.desc(p.base(Module::Master) + master::Gain);
    push(p.base(Module::Master) + master::Gain, ControlEvent::Kind::Offset, plan.masterGainDb / (mg.maxValue - mg.minValue));
}

void Composer::melodyControls(const ParamStore& p, const TrackPlan& plan, int inTrack, double beat, std::vector<ControlEvent>& out) const
{
    // The acid cutoff travels in arcs: at every 16-bar block it sets out for a new target, reached at
    // the end of the block. The first track starts from the knob.
    const int cb = p.base(Module::Compose), ab = p.base(Module::Acid);
    const float sv = p.get(cb + compose::SoundVariation), mv = p.get(cb + compose::MelodyVariation);
    const MelodyPlan& m = plan.melody;
    const int block = std::min(kMelodyMaxBlocks - 1, inTrack / 16);
    const float base = 0.10f * sv * m.recipe[0];
    const float target = (plan.index == 0 && block == 0) ? 0.0f : base + 0.12f * mv * m.acidArc[block];
    auto push = [&](int id, float value, float length) {
        ControlEvent c;
        c.beat = beat;
        c.param = static_cast<int16_t>(id);
        c.kind = ControlEvent::Kind::Offset;
        c.value = value;
        c.length = length;
        out.push_back(c);
    };
    if (inTrack == 0) push(ab + acid::Cutoff, plan.index == 0 ? 0.0f : base, 0.0f);
    {
        ControlEvent g;
        g.beat = beat;
        g.param = static_cast<int16_t>(p.base(Module::Poly, 2) + poly::Gate);
        g.kind = ControlEvent::Kind::Override;
        g.value = m.padGate[block] ? 1.0f : 0.0f;
        out.push_back(g);
    }
    push(ab + acid::Cutoff, target, 16.0f * kBeatsPerBar);
    push(ab + acid::EnvAmount, 0.5f * (target - base), 16.0f * kBeatsPerBar);
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
    const int bars = part == -2 ? 8 : 2;
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
    const int mutes[kMelodyParts] = { mix::AcidMute, mix::LeadMute, mix::ArpMute, mix::PadMute };
    for (int k = 0; k < kMelodyParts; ++k) engine->params().set(mb + mutes[k], (k == part || part == -2) ? 0.0f : 1.0f);
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
            n.pitch = static_cast<uint8_t>(root);
            n.velocity = 110;
            notes.push_back(n);
        }
    }
    // Which bar of the track each probe bar is: 49 and 50 (the percussion's layers are in) for the foundation
    // and the parts, the middle bar of the block at each fifth of the track for the whole mix.
    auto sourceBar = [&](int b) {
        if (part != -2) return 49 + b;
        const int at = (2 * b + 1) * plan.bars / 16;
        return (at / 16) * 16 + 8;
    };
    for (int b = 0; b < bars; ++b)
        composePercBar(p, plan.perc, plan.percSeed, b, sourceBar(b), plan.bpm, plan.key, plan.scale, false, notes);
    // The melodic parts: in a full block for their own measurement, as scheduled for the whole mix.
    if (part >= 0 || part == -2) {
        std::vector<NoteEvent> mel;
        for (int b = 0; b < bars; ++b) composeMelodyBar(p, plan.melody, b, part == -2 ? sourceBar(b) : 48 + b, plan.scale, part != -2, mel);
        static const Part kParts[kMelodyParts] = { Part::Acid, Part::Lead, Part::Arp, Part::Pad };
        for (const NoteEvent& n : mel) if (part == -2 || n.part == kParts[part]) notes.push_back(n);
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
        const int pattern = (secondaryBlock && inTrack % 16 >= 12) ? plan.secondaryPattern : plan.primaryPattern;
        const BassPatternDef& pat = kBassPatterns[pattern];

        if (controls != nullptr) {
            if (inTrack == 0) {
                trackStartControls(p, plan, barBeat, *controls);
                arcControls(p, plan, inTrack, barBeat, false, *controls);
            } else if (inTrack % 32 == 0) {
                arcControls(p, plan, inTrack, barBeat, true, *controls);
            }
            if (inTrack % 16 == 0) melodyControls(p, plan, inTrack, barBeat, *controls);
            ControlEvent c;
            c.beat = barBeat;
            c.param = static_cast<int16_t>(cb + compose::BassPattern);
            c.kind = ControlEvent::Kind::Override;
            c.value = static_cast<float>(pattern);
            controls->push_back(c);
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

        for (int beat = 0; beat < kBeatsPerBar; ++beat) {
            const double b = barBeat + beat;
            const bool fillGap = kickPattern == 1 && inTrack % 8 == 7 && beat == 3;
            if (kickPattern != 2 && !fillGap) {
                NoteEvent k;
                k.beat = b;
                k.length = 0.25f;
                k.part = Part::Kick;
                k.pitch = 36;
                k.velocity = 127;
                out.push_back(k);
            }
            if (bassBreak && inTrack % 16 == 15 && beat >= 2) continue;

            int figure = -1;
            if (beat == 3 && barInPhrase == 1 && varyBar2) figure = figure2;
            if (beat == 3 && barInPhrase == 3 && varyBar4) figure = figure4;
            for (int s = 0; s < pat.count; ++s) {
                const double pos = pat.pos[s];
                const double next = s + 1 < pat.count ? pat.pos[s + 1] : 1.0;   // the last note ends before the next kick
                int pitch = root;
                if (figure >= 0) {
                    const int idx = 3 - pat.count + s;
                    // Figure degrees count from the chord's degree, so they stay in the scale when the bass follows the chords.
                    pitch = root + scaleDegree(plan.scale, chordDeg + kFigureDegrees[figure][idx]) - scaleDegree(plan.scale, chordDeg)
                          + 12 * kFigureOctaves[figure][idx];
                }
                NoteEvent n;
                n.beat = b + pos;
                n.length = static_cast<float>((next - pos) * plan.gate);
                n.part = Part::Bass;
                n.pitch = static_cast<uint8_t>(std::clamp(pitch, 0, 127));
                n.velocity = 110;
                out.push_back(n);
            }
        }
        composePercBar(p, plan.perc, plan.percSeed, bar, inTrack, plan.bpm, plan.key, plan.scale, true, out);
        composeMelodyBar(p, plan.melody, bar, inTrack, plan.scale, false, out);
        composeSfxBar(plan.melody, static_cast<double>(plan.firstBar) * kBeatsPerBar, inTrack, out);
    }
    std::stable_sort(out.begin() + static_cast<long>(noteStart), out.end(), noteLess);
    if (controls != nullptr)
        std::stable_sort(controls->begin() + static_cast<long>(ctlStart), controls->end(),
                         [](const ControlEvent& a, const ControlEvent& b) { return a.beat < b.beat; });
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
        while (controlPos_ < controls_.size()) {
            if (!engine_.pushControl(controls_[controlPos_])) return;
            ++controlPos_;
        }
        while (notePos_ < notes_.size()) {
            if (!engine_.pushEvent(notes_[notePos_])) return;
            if (record != nullptr) record->push_back(notes_[notePos_]);
            ++notePos_;
        }
        if (static_cast<double>(nextBar_) * kBeatsPerBar >= target) return;
        notes_.clear();
        controls_.clear();
        notePos_ = controlPos_ = 0;
        composer_.composeBars(params, nextBar_, 1, notes_, &controls_);
        ++nextBar_;
    }
}

} // namespace phos
