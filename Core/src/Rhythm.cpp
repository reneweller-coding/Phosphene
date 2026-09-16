/**
 * @file Rhythm.cpp
 * @brief Euclidean rhythms, syncopation, percussion plans and bars.
 */
#include "phos/Rhythm.h"
#include "phos/Dsp.h"
#include "phos/Harmony.h"
#include "phos/Perc.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {

constexpr uint64_t kSaltPlan   = 0x5045524350000001ull;
constexpr uint64_t kSaltPhrase = 0x5045524350000002ull;
constexpr uint64_t kSaltFill   = 0x5045524350000004ull;
constexpr uint64_t kSaltLane   = 0x5045524350000005ull;

double normal(Rng& r)
{
    const double u1 = 1.0 - static_cast<double>(r.uniform());
    const double u2 = static_cast<double>(r.uniform());
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.141592653589793 * u2);
}

/** @brief First active lane with a role, or -1. */
int laneOfRole(const ParamStore& p, PercRole role)
{
    for (int l = 0; l < kPercLanes; ++l) {
        const int b = p.base(Module::Perc, l);
        if (p.getBool(b + perc::Active) && p.getInt(b + perc::Role) == static_cast<int>(role)) return l;
    }
    return -1;
}

bool isEuclidRole(PercRole r)
{
    return r == PercRole::Rim || r == PercRole::Tom || r == PercRole::Conga || r == PercRole::Zap || r == PercRole::Blip;
}

/** @brief How likely a role is to join a track's groove early (hats and clap are placed separately). */
double layerWeight(PercRole r)
{
    switch (r) {
    case PercRole::Shaker: return 1.0;
    case PercRole::Tom:    return 0.7;
    case PercRole::Conga:  return 0.6;
    case PercRole::Rim:    return 0.6;
    case PercRole::Ride:   return 0.5;
    case PercRole::Zap:    return 0.5;
    case PercRole::Blip:   return 0.5;
    case PercRole::OpenHat: return 0.4;
    case PercRole::Clap:   return 0.4;
    default: return 0.0;   // crash and snare play at phrase marks and in fills, not as a layer
    }
}

} // namespace

std::vector<bool> euclid(int pulses, int steps, int rotation)
{
    // The Bresenham form: step i is an onset when (i k) mod n < k. It yields the same necklaces as
    // Bjorklund's algorithm -- maximally even onsets -- up to rotation.
    std::vector<bool> out(static_cast<size_t>(std::max(steps, 0)), false);
    if (steps <= 0) return out;
    pulses = std::clamp(pulses, 0, steps);
    for (int i = 0; i < steps; ++i) {
        const int src = ((i - rotation) % steps + steps) % steps;
        out[static_cast<size_t>(i)] = (src * pulses) % steps < pulses;
    }
    return out;
}

int lhlWeight(int step)
{
    step = ((step % 16) + 16) % 16;
    if (step == 0) return 0;
    if (step == 8) return -1;
    if (step % 4 == 0) return -2;
    if (step % 2 == 0) return -3;
    return -4;
}

int lhlSyncopation(const bool* steps)
{
    int total = 0, count = 0;
    for (int i = 0; i < 16; ++i) count += steps[i] ? 1 : 0;
    if (count == 0) return 0;
    for (int i = 0; i < 16; ++i) {
        if (!steps[i]) continue;
        // The strongest position strictly between this note and the next one (cyclically).
        int best = -100;
        for (int d = 1; d < 16; ++d) {
            const int j = (i + d) % 16;
            if (steps[j]) break;
            best = std::max(best, lhlWeight(j));
        }
        if (best > lhlWeight(i)) total += best - lhlWeight(i);
    }
    return total;
}

void percRecipeOffsets(const float* macro, float amount, float* out)
{
    std::fill(out, out + perc::Count, 0.0f);
    out[perc::Cutoff]       += amount * 0.10f * macro[0];
    out[perc::MetalScale]   += amount * 0.12f * macro[0];
    out[perc::Decay]        += amount * -0.15f * macro[1];
    out[perc::NoiseDecay]   += amount * -0.15f * macro[1];
    out[perc::BurstSpacing] += amount * -0.08f * macro[1];
    out[perc::Drive]        += amount * 0.30f * macro[2];
    out[perc::Resonance]    += amount * 0.10f * macro[2];
}

PercPlan makePercPlan(const ParamStore& p, uint64_t seed, bool firstTrack, const uint64_t* laneSeeds)
{
    const int cb = p.base(Module::Compose);
    const float pv = p.get(cb + compose::PercVariation);
    const float density = p.get(cb + compose::PercDensity);
    const float sv = p.get(cb + compose::SoundVariation);
    Rng r;
    r.seed(mixSeed(seed, kSaltPlan));

    PercPlan plan;
    for (int l = 0; l < kPercLanes; ++l) { plan.engineOverride[l] = -1; plan.modeSetOverride[l] = -1; }

    const int closedHat = laneOfRole(p, PercRole::ClosedHat), openHat = laneOfRole(p, PercRole::OpenHat);
    const int shaker = laneOfRole(p, PercRole::Shaker), clap = laneOfRole(p, PercRole::Clap);

    // Hat mode, by weight among the modes the kit can play.
    double w[3] = { 0.45, openHat >= 0 ? 0.30 : 0.0, shaker >= 0 ? 0.25 : 0.0 };
    double x = r.uniform() * (w[0] + w[1] + w[2]);
    plan.hatMode = x < w[0] ? 0 : (x < w[0] + w[1] ? 1 : 2);
    plan.clapBackbeat = clap >= 0 && r.uniform() < 0.55f;

    // Layer order: the hats first, then the lane the hat mode leans on, the clap, then the rest by weight.
    std::vector<int> order;
    auto add = [&](int lane) { if (lane >= 0 && std::find(order.begin(), order.end(), lane) == order.end()) order.push_back(lane); };
    add(closedHat);
    if (plan.hatMode == 1) add(openHat);
    if (plan.hatMode == 2) add(shaker);
    if (plan.clapBackbeat) add(clap);
    std::vector<int> rest;
    std::vector<double> weights;
    for (int l = 0; l < kPercLanes; ++l) {
        const int b = p.base(Module::Perc, l);
        if (!p.getBool(b + perc::Active)) continue;
        const double lw = layerWeight(static_cast<PercRole>(p.getInt(b + perc::Role)));
        if (lw <= 0.0 || std::find(order.begin(), order.end(), l) != order.end()) continue;
        rest.push_back(l);
        weights.push_back(lw);
    }
    while (!rest.empty()) {   // weighted draw without replacement
        double total = 0.0;
        for (double v : weights) total += v;
        double y = r.uniform() * total;
        size_t k = 0;
        for (; k + 1 < rest.size(); ++k) { if (y < weights[k]) break; y -= weights[k]; }
        order.push_back(rest[k]);
        rest.erase(rest.begin() + static_cast<long>(k));
        weights.erase(weights.begin() + static_cast<long>(k));
    }
    for (size_t i = 0; i < order.size() && i < static_cast<size_t>(kPercLanes); ++i) plan.layerOrder[i] = order[i];
    const int grooveLanes = static_cast<int>(order.size());
    plan.layers = std::clamp(static_cast<int>(std::lround(2.0 + density * 6.0 + (2.0 * r.uniform() - 1.0) * 3.0 * pv)),
                             std::min(2, grooveLanes), grooveLanes);

    // Euclidean lanes: pulses by role, rotation at a moderate syncopation.
    for (int l = 0; l < kPercLanes; ++l) {
        const int b = p.base(Module::Perc, l);
        const PercRole role = static_cast<PercRole>(p.getInt(b + perc::Role));
        if (!isEuclidRole(role)) continue;
        static const int kChoices[kNumPercRoles][2] = {
            { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 3, 5 }, { 0, 0 }, { 3, 5 }, { 5, 7 }, { 2, 3 }, { 3, 5 } };
        // Each lane draws from its own stream, so that one lane can be rerolled or locked on its own.
        Rng lr;
        lr.seed(laneSeeds != nullptr ? laneSeeds[l] : mixSeed(seed ^ kSaltLane, static_cast<uint64_t>(l)));
        const int pulses = kChoices[static_cast<int>(role)][lr.below(2)];
        int lo = 1000, hi = -1000;
        int syn[16] = {};
        for (int rot = 0; rot < 16; ++rot) {
            const std::vector<bool> e = euclid(pulses, 16, rot);
            bool steps[16] = {};
            for (int s = 0; s < 16; ++s) steps[s] = e[static_cast<size_t>(s)] && s % 4 != 0;   // never on the kick
            syn[rot] = lhlSyncopation(steps);
            lo = std::min(lo, syn[rot]);
            hi = std::max(hi, syn[rot]);
        }
        const double target = lo + (hi - lo) * std::clamp(0.5 + (2.0 * lr.uniform() - 1.0) * 0.25 * pv, 0.0, 1.0);
        int best = 0;
        for (int rot = 1; rot < 16; ++rot) if (std::fabs(syn[rot] - target) < std::fabs(syn[best] - target)) best = rot;
        plan.pulses[l] = pulses;
        plan.rotation[l] = best;
    }

    if (!firstTrack) {
        for (float& m : plan.macro) {
            double v;
            do { v = 0.5 * normal(r); } while (v < -1.0 || v > 1.0);
            m = static_cast<float>(v);
        }
        if (closedHat >= 0 && r.uniform() < 0.25f * sv) {
            const int e = p.getInt(p.base(Module::Perc, closedHat) + perc::Engine);
            plan.engineOverride[closedHat] = e == static_cast<int>(PercEngine::Metal) ? static_cast<int>(PercEngine::Noise) : static_cast<int>(PercEngine::Metal);
        }
        for (PercRole role : { PercRole::Tom, PercRole::Conga }) {
            const int lane = laneOfRole(p, role);
            if (lane >= 0 && r.uniform() < 0.3f * sv) plan.modeSetOverride[lane] = r.uniform() < 0.5f ? 0 : 2;
        }
    }
    return plan;
}

FillType chooseFill(const ParamStore& p, uint64_t trackSeed, int barInTrack)
{
    if (barInTrack % 8 != 7) return FillType::None;
    const float pv = p.get(p.base(Module::Compose) + compose::PercVariation);
    Rng r;
    r.seed(mixSeed(trackSeed ^ kSaltFill, static_cast<uint64_t>(barInTrack)));
    double w[static_cast<int>(FillType::Count)] = {
        0.0,
        laneOfRole(p, PercRole::Snare) >= 0 ? 1.0 : 0.0,
        laneOfRole(p, PercRole::Tom) >= 0 ? 0.6 + 0.4 * pv : 0.0,
        laneOfRole(p, PercRole::Zap) >= 0 ? 0.3 + 0.5 * pv : 0.0,
        laneOfRole(p, PercRole::Clap) >= 0 ? 0.4 * pv : 0.0,
        0.5,
    };
    // At the end of sixteen bars the fill is always a roll when a snare exists.
    if (barInTrack % 16 == 15 && w[1] > 0.0) return FillType::SnareRoll;
    double total = 0.0;
    for (double v : w) total += v;
    double x = r.uniform() * total;
    for (int i = 1; i < static_cast<int>(FillType::Count); ++i) { if (x < w[i]) return static_cast<FillType>(i); x -= w[i]; }
    return FillType::HatDrop;
}

void composePercBar(const ParamStore& p, const PercPlan& plan, uint64_t trackSeed, int bar, int barInTrack,
                    double bpm, int keyRoot, int scale, const PercBarSpec& spec, std::vector<NoteEvent>& out)
{
    const int cb = p.base(Module::Compose);
    const float density = p.get(cb + compose::PercDensity);
    const float swing = p.get(cb + compose::Swing);
    const double barBeat = static_cast<double>(bar) * kBeatsPerBar;
    Rng phrase;
    phrase.seed(mixSeed(trackSeed ^ kSaltPhrase, static_cast<uint64_t>(barInTrack / 4)));

    const int layers = std::clamp(spec.layers, 0, kPercLanes);
    bool inGroove[kPercLanes] = {};
    for (int i = 0; i < layers; ++i) inGroove[plan.layerOrder[i]] = true;
    // A buildup's roll replaces the phrase fill; everything else keeps the fill bank.
    const FillType fill = (spec.fills && spec.rollBar < 0) ? chooseFill(p, trackSeed, barInTrack) : FillType::None;

    auto emit = [&](int lane, double beatInBar, float velocity, int shift) {
        if (beatInBar < static_cast<double>(spec.cutBeats)) return;
        const int b = p.base(Module::Perc, lane);
        const PercRole role = static_cast<PercRole>(p.getInt(b + perc::Role));
        double beat = barBeat + beatInBar;
        const double stepPos = beatInBar * 4.0;
        if (std::fabs(stepPos - std::round(stepPos)) < 1e-9 && static_cast<int>(std::lround(stepPos)) % 2 == 1) beat += swing * 0.25;
        beat += p.get(b + perc::Shift) * 0.001 * bpm / 60.0;
        NoteEvent n;
        // Kept inside the bar: bars reach the engine whole and in order, so a hit pushed before its
        // bar's start would sit behind later notes of the previous bar in the event ring.
        n.beat = std::clamp(beat, barBeat, barBeat + kBeatsPerBar - 1e-6);
        n.length = 0.125f;
        n.part = Part::Perc;
        n.lane = static_cast<uint8_t>(lane);
        n.pitch = static_cast<uint8_t>(std::clamp(kPercRoleNote[static_cast<int>(role)] + shift, 0, 127));
        n.velocity = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(velocity * 127.0f)), 1, 127));
        out.push_back(n);
    };

    for (int l = 0; l < kPercLanes; ++l) {
        const int b = p.base(Module::Perc, l);
        if (!p.getBool(b + perc::Active)) continue;
        const PercRole role = static_cast<PercRole>(p.getInt(b + perc::Role));
        const float prob = std::clamp(p.get(b + perc::Density) * density * 2.0f, 0.0f, 1.0f);
        // Optional hits: one decision per step per phrase, repeated through the phrase.
        bool keep[16];
        for (bool& k : keep) k = phrase.uniform() < prob;
        const bool hatsDropped = fill == FillType::HatDrop;

        if (inGroove[l]) {
            for (int s = 0; s < 16; ++s) {
                const int pos = s % 4;
                if (hatsDropped && s >= 12 && (role == PercRole::ClosedHat || role == PercRole::OpenHat || role == PercRole::Shaker)) continue;
                float vel = 0.0f;
                switch (role) {
                case PercRole::ClosedHat:
                    // In a buildup the hats close up to every sixteenth, which is the densest the lane
                    // ever plays; that rise is one of the buildup's cues.
                    if (spec.hatsDense) vel = pos == 2 ? 1.0f : 0.55f;
                    else if (plan.hatMode == 1) vel = (pos == 1 || pos == 3) ? 0.55f : 0.0f;
                    else vel = pos == 2 ? 1.0f : ((pos == 1 || pos == 3) && plan.hatMode == 0 && keep[s] ? 0.5f : 0.0f);
                    break;
                case PercRole::OpenHat:
                    vel = plan.hatMode == 1 ? (pos == 2 ? 0.9f : 0.0f) : (s == 14 && barInTrack % 2 == 1 ? 0.8f : 0.0f);
                    break;
                case PercRole::Shaker:
                    vel = pos == 2 ? 0.8f : ((pos == 1 || pos == 3) && (plan.hatMode == 2 || keep[s]) ? 0.45f : 0.0f);
                    break;
                case PercRole::Ride:
                    vel = pos == 0 ? 0.6f : (pos == 2 ? 0.8f : 0.0f);
                    break;
                case PercRole::Clap:
                    vel = plan.clapBackbeat && (s == 4 || s == 12) ? 1.0f : 0.0f;
                    break;
                case PercRole::Rim: case PercRole::Tom: case PercRole::Conga: case PercRole::Zap: case PercRole::Blip: {
                    const std::vector<bool> e = euclid(plan.pulses[l], 16, plan.rotation[l]);
                    if (e[static_cast<size_t>(s)] && pos != 0) vel = pos == 2 ? 0.9f : 0.65f;
                    break;
                }
                default: break;
                }
                if (vel > 0.0f) emit(l, s * 0.25, vel, 0);
            }
        }
        // Phrase marks outside the layers: the crash on a drop, the snare's ghost note.
        if (role == PercRole::Crash && spec.crash) emit(l, 0.0, 0.9f, 0);
        if (role == PercRole::Snare && fill == FillType::None && spec.rollBar < 0 && barInTrack % 4 == 3 && keep[15] && phrase.uniform() < 0.5f)
            emit(l, 3.75, 0.35f, 0);
    }

    // Fills.
    const int snare = laneOfRole(p, PercRole::Snare), tom = laneOfRole(p, PercRole::Tom);
    const int zap = laneOfRole(p, PercRole::Zap), clap = laneOfRole(p, PercRole::Clap), openHat = laneOfRole(p, PercRole::OpenHat);
    switch (fill) {
    case FillType::SnareRoll:
        if (snare >= 0) {
            const bool big = barInTrack % 16 == 15;
            if (big) for (int s = 8; s < 12; ++s) emit(snare, s * 0.25, 0.45f + 0.03f * (s - 8), 0);
            if (big) for (int k = 0; k < 8; ++k) emit(snare, 3.0 + k * 0.125, 0.6f + 0.05f * k, 0);
            else for (int s = 12; s < 16; ++s) emit(snare, s * 0.25, 0.5f + 0.13f * (s - 12), 0);
        }
        break;
    case FillType::TomRun:
        if (tom >= 0) {
            static const int kSteps[5] = { 10, 12, 13, 14, 15 };
            for (int i = 0; i < 5; ++i) {
                // Down the scale from the tom's own note: degrees 0, -1, -2, -3, -4 below its root.
                const int semis = scaleDegree(scale, -i) - scaleDegree(scale, 0);
                emit(tom, kSteps[i] * 0.25, 0.9f - 0.05f * i, semis);
            }
            (void)keyRoot;
        }
        break;
    case FillType::ZapBurst:
        if (zap >= 0) for (int k = 0; k < 4; ++k) emit(zap, 3.5 + k * 0.125, 0.55f + 0.12f * k, 0);
        break;
    case FillType::ClapTriplet:
        if (clap >= 0) for (int k = 0; k < 3; ++k) emit(clap, 3.0 + k / 3.0, 0.7f + 0.1f * k, 0);
        break;
    case FillType::HatDrop:
        if (openHat >= 0) emit(openHat, 3.5, 0.9f, 0);
        break;
    default: break;
    }

    // The buildup's snare roll over the last four bars: sixteenths, then thirty-seconds, and in the
    // pre-drop break the roll stops on beat 3, leaving beat 4 to the formant shot alone (Grosz et al.
    // 2025 describe the roll of a PDB on every third sixteenth; the plan asks for 1/16 -> 1/32 -> 1/64).
    if (spec.rollBar >= 0 && snare >= 0) {
        const int r = std::clamp(spec.rollBar, 0, 3);
        const double step = r == 0 ? 0.5 : (r == 1 ? 0.25 : 0.125);
        // The pre-drop break's roll ends exactly on beat 3; every other roll bar fills its four beats.
        const double last = spec.pdb ? 3.0 : 4.0 - step;
        int k = 0;
        for (double b = 0.0; b <= last + 1e-9; b += step, ++k) {
            const float vel = std::clamp(0.35f + 0.3f * static_cast<float>(r) + 0.02f * static_cast<float>(k), 0.2f, 1.0f);
            emit(snare, b, vel, 0);
        }
    }
}

} // namespace phos
