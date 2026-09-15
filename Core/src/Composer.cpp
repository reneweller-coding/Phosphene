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
#include "phos/Sfx.h"
#include <algorithm>
#include <cmath>

namespace phos {

const char* const kKickMacroNames[kNumKickMacros] = { "length", "punch", "body", "grit", "click" };
const char* const kBassMacroNames[kNumBassMacros] = { "brightness", "pluck", "squelch", "grit", "weight" };
const char* const kLockUnitNames[kNumLockUnits] = { "set", "track", "section", "lane" };

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
    const double bpm = std::max(20.0, static_cast<double>(p.get(cb + compose::Bpm)));
    return std::max(32.0, minutes * bpm / kBeatsPerBar);
}

} // namespace

/** @brief What a track can offer the instrumentation matrix. */
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

    // Where the track sits on the set's energy arc (Form.h).
    const double setBars = setLengthBars(p);
    const ArcId arc = arcOf(p);
    t.arcIn = static_cast<float>(arcEnergy(arc, t.firstBar / setBars));
    t.arcOut = static_cast<float>(arcEnergy(arc, (t.firstBar + w.bars) / setBars));

    // The form: a track's own decision, so it hangs off the track's seed and is rerollable.
    t.formSeed = mixSeed(trackSeed(index) ^ kSaltFormU, 0);
    t.form = makeFormPlan(style, t.formSeed, w.bars, t.arcIn, t.arcOut);
    makeFormSfx(t.form, t.formSeed, p.get(cb + compose::SfxAmount));
    t.bars = t.form.bars;
    for (int s = 0; s < kMaxSections; ++s) t.sectionSeed[s] = sectionSeedOf(index, s);

    uint64_t laneSeeds[kPercLanes];
    for (int l = 0; l < kPercLanes; ++l) laneSeeds[l] = laneSeedOf(index, l);
    t.percSeed = mixSeed(trackSeed(index) ^ kSaltPerc, 0);
    t.perc = makePercPlan(p, t.percSeed, index == 0, laneSeeds);
    t.melodySeed = mixSeed(trackSeed(index) ^ kSaltMelody, 0);
    // The colour of the lead -- how much weight the flat second and the augmented second get -- follows
    // the style profile and the track's place on the energy arc (Farbood's dissonance).
    const float colour = std::clamp(style.colour * (0.4f + 0.6f * 0.5f * (t.arcIn + t.arcOut)), 0.0f, 1.0f);
    t.melody = makeMelodyPlan(p, style, t.melodySeed, t.key, t.scale, index == 0, colour);

    if (index == 0) {
        t.primaryPattern = std::clamp(p.getInt(cb + compose::BassPattern), 0, kNumBassPatterns - 1);
        t.secondaryPattern = t.primaryPattern == 0 ? 1 : 0;
        t.gate = p.get(cb + compose::BassGate);
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
    const int lb = p.base(Module::Poly, 0), pb = p.base(Module::Poly, 2);
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
    const float acidBase = 0.10f * sv * m.recipe[0];
    const bool knobs = plan.index == 0 && bar.index == 0;
    const float e0 = s.energy, e1 = s.energyTo;
    auto cutoffAt = [&](float base, float scale, float energy) {
        return knobs ? 0.0f : base + scale * (0.35f * (energy - 0.7f) + 0.12f * mv * wobble);
    };
    if (bar.index == 0) {
        push(ab + acid::Cutoff, cutoffAt(acidBase, 1.0f, e0), 0.0f);
        push(lb + poly::Cutoff, cutoffAt(0.10f * sv * m.recipe[1], 0.8f, e0), 0.0f);
        push(pb + poly::Position, cutoffAt(0.25f * sv * m.recipe[3], 0.6f, e0), 0.0f);
    }
    push(ab + acid::Cutoff, cutoffAt(acidBase, 1.0f, e1), length);
    push(ab + acid::EnvAmount, 0.5f * (cutoffAt(acidBase, 1.0f, e1) - acidBase), length);
    push(lb + poly::Cutoff, cutoffAt(0.10f * sv * m.recipe[1], 0.8f, e1), length);
    push(pb + poly::Position, cutoffAt(0.25f * sv * m.recipe[3], 0.6f, e1), length);

    // The hall opens where the floor empties: a breakdown is the wettest part of a track.
    const float wet = s.type == SectionType::Break ? 0.22f : (s.type == SectionType::Intro || s.type == SectionType::Outro ? 0.10f : 0.0f);
    const ParamDesc& hs = p.desc(pb + poly::HallSend);
    const float wetNorm = wet / (hs.maxValue - hs.minValue);
    for (int id : { ab + acid::HallSend, lb + poly::HallSend, p.base(Module::Poly, 1) + poly::HallSend, pb + poly::HallSend })
        push(id, knobs ? 0.0f : wetNorm, length);

    // Loudness (Farbood): at most +-2 dB around the section's energy, on top of the track's level match.
    const ParamDesc& g = p.desc(mb + mix::TrackGain);
    push(mb + mix::TrackGain, (plan.gainDb + (knobs ? 0.0f : energyGainDb(0.5f * (e0 + e1)))) / (g.maxValue - g.minValue), 0.0f);

    // The pad's trance gate is a property of the section, not of a 16-bar block.
    pushNow(pb + poly::Gate, ControlEvent::Kind::Override, bar.padGate ? 1.0f : 0.0f);
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
            return bp;
        }
        return planBar(plan.form, availabilityOf(plan), plan.sectionSeed, source);
    };
    for (int b = 0; b < bars; ++b) {
        const BarPlan bp = probeBar(sourceBar(b));
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
        for (int b = 0; b < bars; ++b)
            composeMelodyBar(p, plan.melody, b, part == -2 ? sourceBar(b) : 48 + b, plan.scale, probeBar(sourceBar(b)), mel);
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
        if (prev.melody.present[3] && (move == 0 || move == 5 || move == 7)) {
            std::vector<NoteEvent> tmp;
            BarPlan pad;
            pad.parts = 8;
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
        const int pattern = (secondaryBlock && inTrack % 16 >= 12) ? plan.secondaryPattern : plan.primaryPattern;
        const BassPatternDef& pat = kBassPatterns[pattern];

        // What the form says plays in this bar (Form.h).
        const BarPlan bp = planBar(plan.form, availabilityOf(plan), plan.sectionSeed, inTrack);

        if (controls != nullptr) {
            if (inTrack == 0) {
                trackStartControls(p, plan, barBeat, *controls);
                arcControls(p, plan, inTrack, barBeat, false, *controls);
            } else if (inTrack % 32 == 0) {
                arcControls(p, plan, inTrack, barBeat, true, *controls);
            }
            if (bp.barInSection == 0) sectionControls(p, plan, bp, barBeat, *controls);
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
            if (((bp.bassBeats >> beat) & 1) == 0 || static_cast<double>(beat) < bp.cutBeats) continue;
            if (bassBreak && inTrack % 16 == 15 && beat >= 2) continue;

            int figure = -1;
            if (beat == 3 && barInPhrase == 1 && varyBar2) figure = figure2;
            if (beat == 3 && barInPhrase == 3 && varyBar4) figure = figure4;
            // The group's own change: every eight-bar group of a core ends on a figure that differs
            // from the previous group's, so two consecutive groups can never be the same bars.
            if (beat == 1 && bp.groupFigure >= 0) figure = bp.groupFigure;
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
        composeMelodyBar(p, plan.melody, bar, inTrack, plan.scale, bp, out);
        composeSfxBar(plan.form, static_cast<double>(plan.firstBar) * kBeatsPerBar, inTrack, out);
        transitionBar(p, ti, inTrack, bar, out);
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
