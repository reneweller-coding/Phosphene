/**
 * @file Rhythm.cpp
 * @brief Euclidean rhythms, syncopation, percussion plans and bars.
 */
#include "phos/Rhythm.h"
#include "phos/Dsp.h"
#include "phos/Harmony.h"
#include "phos/Perc.h"
#include "phos/Util.h"
#include <algorithm>
#include <cmath>

#include "Salts.h"
using namespace phos::salts::rhythm;   // this file's seed salts (Salts.h)

namespace phos {

namespace {

/** @brief First active lane with a role, or -1. */
int laneOfRole(const ParamStore& p, PercRoleE role)
{
    for (int l = 0; l < PercLanes; ++l) {
        const int b = p.base(ModuleE::PERC, l);
        if (p.getBool(b + perc::ACTIVE) && p.getInt(b + perc::ROLE) == static_cast<int>(role)) return l;
    }
    return -1;
}

/** @brief Whether a role plays a Euclidean rhythm rather than a written pattern. */
bool isEuclidRole(PercRoleE r)
{
    return r == PercRoleE::RIM || r == PercRoleE::TOM || r == PercRoleE::CONGA || r == PercRoleE::ZAP || r == PercRoleE::BLIP;
}

/** @brief How likely a role is to join a track's groove early (hats and clap are placed separately). */
double layerWeight(PercRoleE r)
{
    switch (r) {
    case PercRoleE::SHAKER: return 1.0;
    case PercRoleE::TOM:    return 0.7;
    case PercRoleE::CONGA:  return 0.6;
    case PercRoleE::RIM:    return 0.6;
    case PercRoleE::RIDE:   return 0.5;
    case PercRoleE::ZAP:    return 0.5;
    case PercRoleE::BLIP:   return 0.5;
    case PercRoleE::OPEN_HAT: return 0.4;
    case PercRoleE::CLAP:   return 0.4;
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
    std::fill(out, out + perc::COUNT, 0.0f);
    out[perc::CUTOFF]       += amount * 0.10f * macro[0];
    out[perc::METAL_SCALE]   += amount * 0.12f * macro[0];
    out[perc::DECAY]        += amount * -0.15f * macro[1];
    out[perc::NOISE_DECAY]   += amount * -0.15f * macro[1];
    out[perc::BURST_SPACING] += amount * -0.08f * macro[1];
    out[perc::DRIVE]        += amount * 0.30f * macro[2];
    out[perc::RESONANCE]    += amount * 0.10f * macro[2];
}

PercPlan makePercPlan(const ParamStore& p, uint64_t seed, bool firstTrack, const uint64_t* laneSeeds)
{
    const int cb = p.base(ModuleE::COMPOSE);
    const float pv = p.get(cb + compose::PERC_VARIATION);
    const float density = p.get(cb + compose::PERC_DENSITY);
    const float sv = p.get(cb + compose::SOUND_VARIATION);
    Rng r;
    r.seed(mixSeed(seed, SaltPlan));

    PercPlan plan;
    for (int l = 0; l < PercLanes; ++l) { plan.m_engineOverride[l] = -1; plan.m_modeSetOverride[l] = -1; }

    const int closedHat = laneOfRole(p, PercRoleE::CLOSED_HAT), openHat = laneOfRole(p, PercRoleE::OPEN_HAT);
    const int shaker = laneOfRole(p, PercRoleE::SHAKER), clap = laneOfRole(p, PercRoleE::CLAP);

    // Hat mode, by weight among the modes the kit can play.
    double w[3] = { 0.45, openHat >= 0 ? 0.30 : 0.0, shaker >= 0 ? 0.25 : 0.0 };
    double x = r.uniform() * (w[0] + w[1] + w[2]);
    plan.m_hatMode = x < w[0] ? 0 : (x < w[0] + w[1] ? 1 : 2);
    // 19.09.2026, round "arrangement": the clap on 2 and 4 is the first layer the groove adds (the
    // user's rule), so every kit that has a clap plays it on the backbeat. The draw stays in the stream.
    const bool clapDraw = r.uniform() < 0.55f;
    plan.m_clapBackbeat = clap >= 0;
    (void)clapDraw;

    // Layer order: the hats first, then the lane the hat mode leans on; behind them the rule's order --
    // clap, congas, ride, one per eight bars of the groove -- and then the rest by weight.
    std::vector<int> order;
    auto add = [&](int lane) { if (lane >= 0 && std::find(order.begin(), order.end(), lane) == order.end()) order.push_back(lane); };
    add(closedHat);
    if (plan.m_hatMode == 1) add(openHat);
    // The shaker belongs to the hats since 19.09.2026: the intro brings it in before the kick and it stays,
    // so the groove's first new layer is the clap and not a shaker that has been playing for sixteen bars.
    add(shaker);
    plan.m_hatLayers = std::max(1, static_cast<int>(order.size()));
    if (plan.m_clapBackbeat) add(clap);
    add(laneOfRole(p, PercRoleE::CONGA));
    add(laneOfRole(p, PercRoleE::RIDE));
    std::vector<int> rest;
    std::vector<double> weights;
    for (int l = 0; l < PercLanes; ++l) {
        const int b = p.base(ModuleE::PERC, l);
        if (!p.getBool(b + perc::ACTIVE)) continue;
        const double lw = layerWeight(static_cast<PercRoleE>(p.getInt(b + perc::ROLE)));
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
    for (size_t i = 0; i < order.size() && i < static_cast<size_t>(PercLanes); ++i) plan.m_layerOrder[i] = order[i];
    const int grooveLanes = static_cast<int>(order.size());
    plan.m_layers = std::clamp(static_cast<int>(std::lround(2.0 + density * 6.0 + (2.0 * r.uniform() - 1.0) * 3.0 * pv)),
                             std::min(2, grooveLanes), grooveLanes);

    // Euclidean lanes: pulses by role, rotation at a moderate syncopation.
    for (int l = 0; l < PercLanes; ++l) {
        const int b = p.base(ModuleE::PERC, l);
        const PercRoleE role = static_cast<PercRoleE>(p.getInt(b + perc::ROLE));
        if (!isEuclidRole(role)) continue;
        static const int Choices[NumPercRoles][2] = {
            { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 3, 5 }, { 0, 0 }, { 3, 5 }, { 5, 7 }, { 2, 3 }, { 3, 5 } };
        // Each lane draws from its own stream, so that one lane can be rerolled or locked on its own.
        Rng lr;
        lr.seed(laneSeeds != nullptr ? laneSeeds[l] : mixSeed(seed ^ SaltLane, static_cast<uint64_t>(l)));
        const int pulses = Choices[static_cast<int>(role)][lr.below(2)];
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
        plan.m_pulses[l] = pulses;
        plan.m_rotation[l] = best;
    }

    if (!firstTrack) {
        for (float& m : plan.m_macro) {
            double v;
            do { v = 0.5 * gaussian(r); } while (v < -1.0 || v > 1.0);
            m = static_cast<float>(v);
        }
        if (closedHat >= 0 && r.uniform() < 0.25f * sv) {
            const int e = p.getInt(p.base(ModuleE::PERC, closedHat) + perc::ENGINE);
            plan.m_engineOverride[closedHat] = e == static_cast<int>(PercEngineE::METAL) ? static_cast<int>(PercEngineE::NOISE) : static_cast<int>(PercEngineE::METAL);
        }
        for (PercRoleE role : { PercRoleE::TOM, PercRoleE::CONGA }) {
            const int lane = laneOfRole(p, role);
            if (lane >= 0 && r.uniform() < 0.3f * sv) plan.m_modeSetOverride[lane] = r.uniform() < 0.5f ? 0 : 2;
        }
    }
    return plan;
}

FillTypeE chooseFill(const ParamStore& p, uint64_t trackSeed, int barInTrack, int cycleBar)
{
    const int phraseBar = cycleBar >= 0 ? cycleBar : barInTrack;
    if (phraseBar % 8 != 7) return FillTypeE::NONE;
    const float pv = p.get(p.base(ModuleE::COMPOSE) + compose::PERC_VARIATION);
    Rng r;
    r.seed(mixSeed(trackSeed ^ SaltFill, static_cast<uint64_t>(barInTrack)));
    double w[static_cast<int>(FillTypeE::COUNT)] = {
        0.0,
        laneOfRole(p, PercRoleE::SNARE) >= 0 ? 1.0 : 0.0,
        laneOfRole(p, PercRoleE::TOM) >= 0 ? 0.6 + 0.4 * pv : 0.0,
        laneOfRole(p, PercRoleE::ZAP) >= 0 ? 0.3 + 0.5 * pv : 0.0,
        laneOfRole(p, PercRoleE::CLAP) >= 0 ? 0.4 * pv : 0.0,
        0.5,
    };
    // At the end of sixteen bars the fill is always a roll when a snare exists.
    if (phraseBar % 16 == 15 && w[1] > 0.0) return FillTypeE::SNARE_ROLL;
    // The eighth bar of a cycle (19.09.2026): a snare fill or a tom run, nothing else, where the kit has
    // either.
    if (cycleBar >= 0 && cycleBar % 32 == 7 && w[1] + w[2] > 0.0)
        return r.uniform() * (w[1] + w[2]) < w[1] ? FillTypeE::SNARE_ROLL : FillTypeE::TOM_RUN;
    double total = 0.0;
    for (double v : w) total += v;
    double x = r.uniform() * total;
    for (int i = 1; i < static_cast<int>(FillTypeE::COUNT); ++i) { if (x < w[i]) return static_cast<FillTypeE>(i); x -= w[i]; }
    return FillTypeE::HAT_DROP;
}

void composePercBar(const ParamStore& p, const PercPlan& plan, uint64_t trackSeed, int bar, int barInTrack,
                    double bpm, int keyRoot, int scale, const PercBarSpec& spec, std::vector<NoteEvent>& out)
{
    const int cb = p.base(ModuleE::COMPOSE);
    const float density = p.get(cb + compose::PERC_DENSITY);
    const float swing = p.get(cb + compose::SWING);
    const double barBeat = static_cast<double>(bar) * BeatsPerBar;
    Rng phrase;
    phrase.seed(mixSeed(trackSeed ^ SaltPhrase, static_cast<uint64_t>(barInTrack / 4)));

    const int layers = std::clamp(spec.m_layers, 0, PercLanes);
    bool inGroove[PercLanes] = {};
    for (int i = 0; i < layers; ++i) inGroove[plan.m_layerOrder[i]] = true;
    // A buildup's roll replaces the phrase fill; everything else keeps the fill bank.
    const FillTypeE fill = (spec.m_fills && spec.m_rollBar < 0) ? chooseFill(p, trackSeed, barInTrack, spec.m_cycleBar) : FillTypeE::NONE;

    auto emit = [&](int lane, double beatInBar, float velocity, int shift) {
        if (beatInBar < static_cast<double>(spec.m_cutBeats)) return;
        const int b = p.base(ModuleE::PERC, lane);
        const PercRoleE role = static_cast<PercRoleE>(p.getInt(b + perc::ROLE));
        double beat = barBeat + beatInBar;
        const double stepPos = beatInBar * 4.0;
        if (std::fabs(stepPos - std::round(stepPos)) < 1e-9 && static_cast<int>(std::lround(stepPos)) % 2 == 1) beat += swing * 0.25;
        beat += p.get(b + perc::SHIFT) * 0.001 * bpm / 60.0;
        NoteEvent n;
        // Kept inside the bar: bars reach the engine whole and in order, so a hit pushed before its
        // bar's start would sit behind later notes of the previous bar in the event ring.
        n.m_beat = std::clamp(beat, barBeat, barBeat + BeatsPerBar - 1e-6);
        n.m_length = 0.125f;
        n.m_part = PartE::PERC;
        n.m_lane = static_cast<uint8_t>(lane);
        n.m_pitch = static_cast<uint8_t>(std::clamp(PercRoleNote[static_cast<int>(role)] + shift, 0, 127));
        n.m_velocity = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(velocity * 127.0f)), 1, 127));
        out.push_back(n);
    };

    for (int l = 0; l < PercLanes; ++l) {
        const int b = p.base(ModuleE::PERC, l);
        if (!p.getBool(b + perc::ACTIVE)) continue;
        const PercRoleE role = static_cast<PercRoleE>(p.getInt(b + perc::ROLE));
        const float prob = std::clamp(p.get(b + perc::DENSITY) * density * 2.0f, 0.0f, 1.0f);
        // Optional hits: one decision per step per phrase, repeated through the phrase.
        bool keep[16];
        for (bool& k : keep) k = phrase.uniform() < prob;
        const bool hatsDropped = fill == FillTypeE::HAT_DROP;

        // 19.09.2026, round "arrangement": lanes the form asks for by name, whether or not they are among
        // the layers -- the intro's quiet sixteenth hat and its shaker, the off-beat hat that the intro,
        // the breakdown and the outro's bare bars keep, drop 2's open hats and ride. They play the lane's
        // own pattern (below) at hatLevel, or the named figure where the lane is not a layer.
        const bool named = (role == PercRoleE::SHAKER && spec.m_shaker) || (role == PercRoleE::RIDE && spec.m_ride)
                        || (role == PercRoleE::OPEN_HAT && spec.m_openHats);
        if (!inGroove[l] && role == PercRoleE::CLOSED_HAT && (spec.m_quietHats || spec.m_offbeatHat)) {
            for (int s = 0; s < 16; ++s) {
                const int pos = s % 4;
                if (hatsDropped && s >= 12) continue;
                // The quiet figure: every sixteenth but the downbeat, the off-beat a little stronger.
                const float vel = spec.m_quietHats ? (pos == 2 ? 0.5f : (pos != 0 ? 0.32f : 0.0f)) : (pos == 2 ? 1.0f : 0.0f);
                if (vel > 0.0f) emit(l, s * 0.25, vel * spec.m_hatLevel, 0);
            }
        }
        if (role == PercRoleE::OPEN_HAT && spec.m_openHats) {
            for (int s = 2; s < 16; s += 4) if (!(hatsDropped && s >= 12)) emit(l, s * 0.25, 0.9f, 0);
        } else if (inGroove[l] || named) {
            const float level = inGroove[l] ? 1.0f : spec.m_hatLevel;
            for (int s = 0; s < 16; ++s) {
                const int pos = s % 4;
                if (hatsDropped && s >= 12 && (role == PercRoleE::CLOSED_HAT || role == PercRoleE::OPEN_HAT || role == PercRoleE::SHAKER)) continue;
                float vel = 0.0f;
                switch (role) {
                case PercRoleE::CLOSED_HAT:
                    // In a buildup the hats close up to every sixteenth, which is the densest the lane
                    // ever plays; that rise is one of the buildup's cues.
                    if (spec.m_hatsDense) vel = pos == 2 ? 1.0f : 0.55f;
                    else if (plan.m_hatMode == 1) vel = (pos == 1 || pos == 3) ? 0.55f : 0.0f;
                    else vel = pos == 2 ? 1.0f : ((pos == 1 || pos == 3) && plan.m_hatMode == 0 && keep[s] ? 0.5f : 0.0f);
                    break;
                case PercRoleE::OPEN_HAT:
                    vel = plan.m_hatMode == 1 ? (pos == 2 ? 0.9f : 0.0f) : (s == 14 && barInTrack % 2 == 1 ? 0.8f : 0.0f);
                    break;
                case PercRoleE::SHAKER:
                    vel = pos == 2 ? 0.8f : ((pos == 1 || pos == 3) && (plan.m_hatMode == 2 || keep[s]) ? 0.45f : 0.0f);
                    break;
                case PercRoleE::RIDE:
                    vel = pos == 0 ? 0.6f : (pos == 2 ? 0.8f : 0.0f);
                    break;
                case PercRoleE::CLAP:
                    vel = plan.m_clapBackbeat && (s == 4 || s == 12) ? 1.0f : 0.0f;
                    break;
                case PercRoleE::RIM: case PercRoleE::TOM: case PercRoleE::CONGA: case PercRoleE::ZAP: case PercRoleE::BLIP: {
                    const std::vector<bool> e = euclid(plan.m_pulses[l], 16, plan.m_rotation[l]);
                    if (e[static_cast<size_t>(s)] && pos != 0) vel = pos == 2 ? 0.9f : 0.65f;
                    break;
                }
                default: break;
                }
                if (vel > 0.0f) emit(l, s * 0.25, vel * level, 0);
            }
        }
        // Phrase marks outside the layers: the crash on a drop, the snare's ghost note.
        if (role == PercRoleE::CRASH && spec.m_crash) emit(l, 0.0, 0.9f, 0);
        // The ghost note belongs to bars that may carry fills (19.09.2026): not to the bare ends of a track.
        if (role == PercRoleE::SNARE && fill == FillTypeE::NONE && spec.m_rollBar < 0 && barInTrack % 4 == 3 && keep[15] && phrase.uniform() < 0.5f
            && spec.m_fills)
            emit(l, 3.75, 0.35f, 0);
    }

    // Fills.
    const int snare = laneOfRole(p, PercRoleE::SNARE), tom = laneOfRole(p, PercRoleE::TOM);
    const int zap = laneOfRole(p, PercRoleE::ZAP), clap = laneOfRole(p, PercRoleE::CLAP), openHat = laneOfRole(p, PercRoleE::OPEN_HAT);
    switch (fill) {
    case FillTypeE::SNARE_ROLL:
        if (snare >= 0) {
            const bool big = barInTrack % 16 == 15;
            if (big) for (int s = 8; s < 12; ++s) emit(snare, s * 0.25, 0.45f + 0.03f * (s - 8), 0);
            if (big) for (int k = 0; k < 8; ++k) emit(snare, 3.0 + k * 0.125, 0.6f + 0.05f * k, 0);
            else for (int s = 12; s < 16; ++s) emit(snare, s * 0.25, 0.5f + 0.13f * (s - 12), 0);
        }
        break;
    case FillTypeE::TOM_RUN:
        if (tom >= 0) {
            static const int Steps[5] = { 10, 12, 13, 14, 15 };
            for (int i = 0; i < 5; ++i) {
                // Down the scale from the tom's own note: degrees 0, -1, -2, -3, -4 below its root.
                const int semis = scaleDegree(scale, -i) - scaleDegree(scale, 0);
                emit(tom, Steps[i] * 0.25, 0.9f - 0.05f * i, semis);
            }
            (void)keyRoot;
        }
        break;
    case FillTypeE::ZAP_BURST:
        if (zap >= 0) for (int k = 0; k < 4; ++k) emit(zap, 3.5 + k * 0.125, 0.55f + 0.12f * k, 0);
        break;
    case FillTypeE::CLAP_TRIPLET:
        if (clap >= 0) for (int k = 0; k < 3; ++k) emit(clap, 3.0 + k / 3.0, 0.7f + 0.1f * k, 0);
        break;
    case FillTypeE::HAT_DROP:
        if (openHat >= 0) emit(openHat, 3.5, 0.9f, 0);
        break;
    default: break;
    }

    // The buildup's snare roll over the last four bars: sixteenths, then thirty-seconds, and in the
    // pre-drop break the roll stops on beat 3, leaving beat 4 to the formant shot alone (Grosz et al.
    // 2025 describe the roll of a PDB on every third sixteenth; the plan asks for 1/16 -> 1/32 -> 1/64).
    //
    // **The lift (16.09.2026.)** A roll that only gets faster and louder does not lift; what a
    // psytrance buildup does over it is raise the snare's pitch continuously towards an octave and
    // thin it while it rises. Both are written here, into the note, and not as per-hit randomness:
    // the shift is a function of where the hit sits in the four bars, so the same bar always lifts the
    // same way, the MIDI export carries it, and a listener hears a single line rather than a jitter.
    // The thinning is the lane's own business -- @c perc.cut_track ties its high pass to this shift
    // (Perc.h) -- so nothing here has to know how the snare is built.
    //
    // Why a rising pitch reads as rising tension at all: Huron ("Sweet Anticipation", 2006, ch. 12)
    // and Juslin and Laukka ("Communication of emotions in vocal expression", Psych. Bull. 2003)
    // collect the evidence that rising pitch together with rising loudness and rate is the
    // cross-cultural signal of increasing arousal, which is exactly what a buildup is for.
    //
    // **The roll's subdivision (19.09.2026, round "arrangement").** The user's rule for the big buildup:
    // "a 16-bar snare roll: quarters -> eighths -> sixteenths -> thirty-seconds". The roll is cut into
    // four equal quarters, each with its note value, whatever its length: four bars each over sixteen,
    // one bar each over the four-bar roll of buildup 1. (Until then a four-bar roll played eighths,
    // sixteenths and two bars of thirty-seconds.)
    if (spec.m_rollBar >= 0 && snare >= 0) {
        const int n = std::max(1, spec.m_rollBars);
        const int r = std::clamp(spec.m_rollBar, 0, n - 1);
        static const double Step[4] = { 1.0, 0.5, 0.25, 0.125 };
        const double step = Step[std::min(3, (4 * r) / n)];
        const float stage = 4.0f * static_cast<float>(r) / static_cast<float>(n);   // 0..4 over the roll
        // The pre-drop break's roll ends inside beat 3 -- its last hit one step before beat 4, which holds
        // nothing (19.09.2026: until then the last hit fell on beat 4's own downbeat); every other roll bar
        // fills its four beats.
        const double last = spec.m_pdb ? 3.0 - step : 4.0 - step;
        // The thirty-seconds play at three quarters of the velocity (19.09.2026, round "polish"): twice the
        // hits of the sixteenths are +3 dB of snare by themselves, and with the full velocity ramp on top the
        // last quarter of the big buildup was the brightest eight bars of the track, 2.2 dB above drop 2 over
        // 1.5 kHz (median of 30 tracks) -- the roll must lead into the climax, not outshine it. -2.5 dB per hit
        // leaves the step from sixteenths to thirty-seconds a rise of about half a decibel, and the rate.
        // 20.09.2026, round "climax-polish": the sixteenths now take a smaller trim of their own (0.85, about
        // -1.4 dB) and the thirty-seconds a deeper one (0.65, about -3.7 dB against the untrimmed rate). The
        // last eight bars of the buildup -- sixteenths then thirty-seconds -- were still 0.90 dB brighter than
        // drop 2's own brightest eight bars above 1.5 kHz in 20 of 30 tracks even with only the thirty-seconds
        // trimmed (opening drop 2's filter further, ClimaxOpen, was tried first and measured to do nothing:
        // Composer.cpp's cutoffAt already pushes the climax's cutoff to the knob's own ceiling, so a bigger
        // offset has no headroom left to open into). The step from sixteenths to thirty-seconds still rises,
        // now by about half a decibel less than before, so the roll keeps climbing into the drop.
        const float rollScale = step < 0.25 ? 0.65f : (step < 0.5 ? 0.85f : 1.0f);
        int k = 0;
        for (double b = 0.0; b <= last + 1e-9; b += step, ++k) {
            const float vel = rollScale * std::clamp(0.35f + 0.3f * stage + 0.02f * static_cast<float>(k), 0.2f, 1.0f);
            // Position within the whole roll, 0 at its first hit and 1 at the drop. The ramp is over the
            // roll, not over the bar, so its bars form one gesture.
            const double u = (static_cast<double>(r) + b / BeatsPerBar) / static_cast<double>(n);
            emit(snare, b, vel, static_cast<int>(std::lround(RollSemitones * u)));
        }
    }
}

} // namespace phos
