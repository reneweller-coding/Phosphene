/**
 * @file Perc.cpp
 * @brief Percussion kit: coefficients, hits and rendering.
 */
#include "phos/Perc.h"
#include "phos/Harmony.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {

constexpr double kPiD = 3.141592653589793;
constexpr double kLn1000 = 6.907755278982137;

/** @brief Mode frequency ratios: membrane (Bessel zeros), free-free bar, loaded membrane (tabla). */
constexpr double kModeRatios[3][kPercModes] = {
    { 1.0, 1.593, 2.136, 2.295 },
    { 1.0, 2.756, 5.404, 8.933 },
    { 1.0, 2.0, 3.0, 4.0 },
};
/** @brief Strike amplitudes of the modes, before normalisation. */
constexpr double kModeAmps[3][kPercModes] = {
    { 1.0, 0.7, 0.5, 0.4 },
    { 1.0, 0.5, 0.25, 0.12 },
    { 1.0, 0.5, 0.33, 0.25 },
};
/** @brief TR-808 cymbal oscillator frequencies. */
constexpr double kMetalHz[kPercMetalOsc] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };

double decayFactor(double seconds, double sr) { return std::exp(-kLn1000 / (std::max(seconds, 1.0e-4) * sr)); }

/**
 * @name The auto-pan (16.09.2026)
 * @{ */
/** @brief sqrt(2): the swing amplitude that keeps E[p^2] at p0^2 together with sqrt(1 - D^2) (Perc.h). */
constexpr double kPanRms = 1.4142135623730951;
/**
 * @brief Largest rotation angle the kernel's Taylor series may be asked for.
 *
 * cos and sin are taken to the w^6 and w^7 terms there and the pair is renormalised by one Newton
 * step afterwards, so the truncation survives only as an angle error: at |w| = 1.6 the first dropped
 * term is w^8/8! = 1.1e-3 rad, which is 0.01 dB on a gain. The angle a lane can ask for is
 * (|panA| + |panB|) and stays under 1.6 for every position and depth the field allows (Perc.h); the
 * clamp is a guard on a hand-edited preset, not a working limit.
 */
constexpr double kPanMaxAngle = 1.6;
/** @} */

/** @brief Coefficients of a trapezoidal SVF (as Svf::setG). */
void svfCoefs(double fc, double damping, double sr, float& a1, float& a2, float& a3)
{
    const double g = std::tan(kPiD * std::clamp(fc, 10.0, 0.45 * sr) / sr);
    const double d1 = 1.0 / (1.0 + g * (g + damping));
    a1 = static_cast<float>(d1);
    a2 = static_cast<float>(g * d1);
    a3 = static_cast<float>(g * g * d1);
}

} // namespace

double PercKit::tuneToScale(double hz, int keyRoot, int scale)
{
    const double note = 69.0 + 12.0 * std::log2(hz / 440.0);
    const int base = static_cast<int>(std::floor(note));
    double best = hz, bestDist = 1e9;
    for (int n = base - 2; n <= base + 3; ++n) {
        if (!inScale(scale, n - keyRoot)) continue;
        const double d = std::fabs(n - note);
        if (d < bestDist) { bestDist = d; best = midiToHz(n); }
    }
    return best;
}

double PercKit::panAngle(int lane) const
{
    if (lane < 0 || lane >= kPercLanes) return 0.0;
    return static_cast<double>(c_.panA[lane]) * static_cast<double>(s_.pr[lane]) + static_cast<double>(c_.panB[lane]);
}

double PercKit::panPosition(int lane) const
{
    if (lane < 0 || lane >= kPercLanes) return 0.0;
    return std::clamp(static_cast<double>(values_[lane][perc::Pan]), -1.0, 1.0) + panAngle(lane) * 4.0 / kPiD;
}

double PercKit::panSwing(int lane) const
{
    return (lane < 0 || lane >= kPercLanes) ? 0.0 : static_cast<double>(c_.panA[lane]);
}

int PercKit::panGroup(int lane) const
{
    if (lane < 0 || lane >= kPercLanes) return 0;
    const double p0 = static_cast<double>(values_[lane][perc::Pan]);
    if (c_.panA[lane] == 0.0f || p0 == 0.0) return 0;
    return (static_cast<double>(c_.panA[lane]) * p0 > 0.0) ? +1 : -1;
}

void PercKit::setTempo(double bpm)
{
    const double b = std::clamp(bpm, 20.0, 400.0);
    if (b == bpm_) return;
    bpm_ = b;
    // Only the phasor's step depends on the tempo. The engine calls this at every 32-sample chunk,
    // and during a tempo ramp between tracks the value changes at every one of them, so the full
    // coefficient set (filters, modes, the pan groups) is not recomputed here.
    for (int l = 0; l < kPercLanes; ++l) if (valid_[l]) updatePanRate(l);
}

/**
 * @brief Splits the lanes into the two phase groups of the auto-pan and signs their swings.
 *
 * What the 85 ms level difference reads is the power-weighted mean position of whatever sounds in
 * the window (Perc.h). The swinging part of that mean is sum_l w_l s_l |panA_l| with w_l the lane's
 * power and s_l = +-1 its phase; the assignment that keeps the kit's centre still is therefore the
 * one that minimises |sum_l w_l s_l a_l|, a two-way number partitioning. Greedy in descending weight
 * (Graham 1969) is used rather than an exact search: twelve lanes would allow one, but the greedy
 * answer is what a listener hears as "the loud pair swaps sides" and it is stable under a knob
 * moving a little, which an exact partition is not.
 *
 * Runs over all twelve lanes whenever any one of them changes, which happens on a parameter change
 * and not per sample.
 */
void PercKit::assignPanGroups()
{
    // Weight: the lane's power times the size of its swing. A silent or centred lane weighs nothing
    // and takes whichever group is left, which is what the sign of its (zero) swing then means.
    double w[kPercLanes] = {};
    for (int l = 0; l < kPercLanes; ++l)
        w[l] = valid_[l] ? std::pow(10.0, static_cast<double>(values_[l][perc::Level]) / 10.0)
                         * std::fabs(static_cast<double>(c_.panA[l])) : 0.0;
    int order[kPercLanes];
    for (int l = 0; l < kPercLanes; ++l) order[l] = l;
    // Insertion sort by weight, descending, ties by lane index: twelve elements, and the order has to
    // be the same on every platform, which std::sort does not promise for equal keys.
    for (int i = 1; i < kPercLanes; ++i) {
        const int k = order[i];
        int j = i - 1;
        while (j >= 0 && w[order[j]] < w[k]) { order[j + 1] = order[j]; --j; }
        order[j + 1] = k;
    }
    double acc = 0.0;
    for (int i = 0; i < kPercLanes; ++i) {
        const int l = order[i];
        // The lane's swing carries the sign of its own position, so that "in phase" means "swings
        // outwards first"; the group decides whether that is kept or turned round. The sign is set
        // absolutely and not flipped, so that recomputing one lane cannot invert another.
        const double dir = values_[l][perc::Pan] < 0.0f ? -1.0 : 1.0;
        const double a = w[l] * dir;
        const bool keep = std::fabs(acc + a) <= std::fabs(acc - a);
        acc += keep ? a : -a;
        c_.panA[l] = static_cast<float>(std::fabs(static_cast<double>(c_.panA[l])) * dir * (keep ? 1.0 : -1.0));
    }
    panning_ = false;
    for (int l = 0; l < kPercLanes; ++l) panning_ = panning_ || c_.panA[l] != 0.0f || c_.panB[l] != 0.0f;
}

void PercKit::prepare(double sampleRate)
{
    sr_ = sampleRate;
    const size_t n = static_cast<size_t>(kMaxBlock * kPercLaneSlots);
    noise_.assign(n, 0.0f);
    reset_.assign(n, 0.0f);
    dNoise_.assign(n, 1.0f);
    outL_.assign(n, 0.0f);
    outR_.assign(n, 0.0f);
    chokeFactor_ = static_cast<float>(decayFactor(0.008, sr_));
    for (int l = 0; l < kPercLanes; ++l) { valid_[l] = false; shiftMul_[l] = 1.0; }
    reset();
}

void PercKit::reset()
{
    s_ = PercState{};
    for (int l = 0; l < kPercLaneSlots; ++l) {
        s_.cr[l] = 1.0f;
        s_.mr[l] = 1.0f;
        s_.choke[l] = 1.0f;
        c_.chokeD[l] = 1.0f;
        c_.dP[l] = 1.0f;
        c_.dA[l] = 1.0f;
    }
    for (int l = 0; l < kPercLaneSlots; ++l) {
        // Every auto-pan phasor starts at angle zero: the two phase groups differ by the sign of
        // panA, not by a phase offset (Perc.h), so one starting point serves the whole kit and the
        // movement is reproducible from a reset without carrying a phase table.
        s_.pr[l] = 1.0f;
        s_.pi[l] = 0.0f;
        c_.panC[l] = 1.0f;
        c_.panS[l] = 0.0f;
    }
    for (int l = 0; l < kPercLanes; ++l) {
        noiseRng_[l].seed(0x5045524300ull + static_cast<uint64_t>(l));
        burstsLeft_[l] = 0;
        burstTimer_[l] = 0.0;
    }
}

void PercKit::update(int lane, const float* v, int keyRoot, int scale)
{
    bool same = valid_[lane] && keyRoot == keyRoot_[lane] && scale == scale_[lane];
    for (int i = 0; same && i < perc::Count; ++i) same = values_[lane][i] == v[i];
    if (same) return;
    std::copy(v, v + perc::Count, values_[lane]);
    keyRoot_[lane] = keyRoot;
    scale_[lane] = scale;
    valid_[lane] = true;
    computeCoefs(lane);
}

void PercKit::computeCoefs(int l)
{
    const float* v = values_[l];
    const bool active = v[perc::Active] >= 0.5f;
    role_[l] = static_cast<int>(std::lround(v[perc::Role]));
    engine_[l] = static_cast<int>(std::lround(v[perc::Engine]));
    choke_[l] = static_cast<int>(std::lround(v[perc::Choke]));
    const PercEngine engine = static_cast<PercEngine>(engine_[l]);

    double hz = v[perc::Pitch];
    if (v[perc::Tune] >= 0.5f) hz = tuneToScale(hz, keyRoot_[l], scale_[l]);
    tunedHz_[l] = hz;
    const double f = hz * shiftMul_[l];

    // Sources.
    c_.wTone[l] = (engine == PercEngine::Tone || engine == PercEngine::Fm) ? 1.0f : 0.0f;
    c_.wModal[l] = engine == PercEngine::Modal ? 1.0f : 0.0f;
    c_.wMetal[l] = engine == PercEngine::Metal ? 1.0f : 0.0f;
    c_.wNoise[l] = engine == PercEngine::Noise ? 1.0f : v[perc::Noise];

    const double w0 = std::min(2.0 * kPiD * f / sr_, 1.2);
    c_.w0[l] = static_cast<float>(w0);
    c_.pAmt[l] = v[perc::PitchAmount] - 1.0f;
    c_.dP[l] = static_cast<float>(std::exp(-1.0 / (std::max(0.5e-3, v[perc::PitchDecay] * 0.001) * sr_)));
    c_.fmIdx[l] = engine == PercEngine::Fm ? v[perc::FmIndex] : 0.0f;
    const double wm = std::min(w0 * v[perc::FmRatio], 0.95 * kPiD);
    c_.mc[l] = static_cast<float>(std::cos(wm));
    c_.ms[l] = static_cast<float>(std::sin(wm));
    const double decay = v[perc::Decay] * 0.001;
    c_.dA[l] = static_cast<float>(decayFactor(decay, sr_));

    // Modes: each higher mode decays faster by ratio^(-1.5 damping); amplitudes normalised to sum 1.
    const int set = std::clamp(static_cast<int>(std::lround(v[perc::ModeSet])), 0, 2);
    double ampSum = 0.0;
    for (int m = 0; m < kPercModes; ++m) ampSum += kModeAmps[set][m];
    for (int m = 0; m < kPercModes; ++m) {
        const double wk = 2.0 * kPiD * f * kModeRatios[set][m] / sr_;
        const bool audible = wk < 0.95 * kPiD;
        const double tau = decay * std::pow(kModeRatios[set][m], -1.5 * v[perc::ModeDamp]);
        const double r = audible ? decayFactor(tau, sr_) : 0.0;
        modeW_[m][l] = wk;
        modeR_[m][l] = r;
        modeAmp_[m][l] = audible ? static_cast<float>(kModeAmps[set][m] / ampSum) : 0.0f;
        c_.modeC[m][l] = static_cast<float>(r * std::cos(wk));
        c_.modeS[m][l] = static_cast<float>(r * std::sin(wk));
    }

    // Metal.
    const double scale = v[perc::MetalScale];
    for (int j = 0; j < kPercMetalOsc; ++j) {
        const double dt = std::min(kMetalHz[j] * scale / sr_, 0.45);
        c_.dt[j][l] = static_cast<float>(dt);
        c_.inv[j][l] = static_cast<float>(1.0 / dt);
    }

    // Noise envelopes: the tail, and the fast decay inside a burst (about 30 dB per burst spacing).
    const double spacing = v[perc::BurstSpacing] * 0.001;
    burstSpacing_[l] = spacing * sr_;
    noiseTail_[l] = static_cast<float>(decayFactor(v[perc::NoiseDecay] * 0.001, sr_));
    noiseFast_[l] = static_cast<float>(std::exp(std::log(0.03) / std::max(1.0, spacing * sr_)));

    // Main filter.
    const int mode = std::clamp(static_cast<int>(std::lround(v[perc::Filter])), 0, 2);
    const double damping = 2.0 - 1.9 * std::clamp(static_cast<double>(v[perc::Resonance]), 0.0, 1.0);
    svfCoefs(v[perc::Cutoff], damping, sr_, c_.a1[l], c_.a2[l], c_.a3[l]);
    c_.k[l] = static_cast<float>(damping);
    c_.mLp[l] = mode == 0 ? 1.0f : 0.0f;
    c_.mBp[l] = mode == 1 ? 1.0f : 0.0f;
    c_.mHp[l] = mode == 2 ? 1.0f : 0.0f;
    // Low cut, never below 150 Hz. With cut_track it follows the hit's pitch shift: shiftMul_ is
    // 2^(semitones/12), so the corner moves by 2^(cut_track * semitones/12).
    //
    // The useful setting is *above* one, and the reason is worth writing down. At exactly 1 the corner
    // and the note move together, which transposes the lane and thins it not at all -- a shifted copy
    // of the same timbre. Only a corner that climbs faster than the note eats into the body, which is
    // what a buildup roll does: at 2 an octave of pitch puts the high pass two octaves up, the snare's
    // fundamental ends below its own filter, and what is left is the noise. That is the "thin it while
    // it rises" of the review, and it costs no new signal path.
    const double cutTrack = std::clamp(static_cast<double>(v[perc::CutTrack]), 0.0, 2.0);
    const double lowCut = static_cast<double>(v[perc::LowCut])
                        * (cutTrack > 0.0 ? std::pow(shiftMul_[l], cutTrack) : 1.0);
    svfCoefs(std::max(150.0, lowCut), std::sqrt(2.0), sr_, c_.c1[l], c_.c2[l], c_.c3[l]);

    // Drive, level, pan (constant power, unity in the centre).
    const double g = 0.1 + 6.0 * v[perc::Drive];
    c_.drvG[l] = static_cast<float>(g);
    c_.drvN[l] = static_cast<float>(1.0 / g);
    const double level = active ? std::pow(10.0, v[perc::Level] / 20.0) : 0.0;
    const double p0 = std::clamp(static_cast<double>(v[perc::Pan]), -1.0, 1.0);
    const double theta = (p0 + 1.0) * kPiD / 4.0;
    c_.gL[l] = static_cast<float>(level * std::cos(theta) * std::sqrt(2.0));
    c_.gR[l] = static_cast<float>(level * std::sin(theta) * std::sqrt(2.0));

    // The auto-pan (Perc.h). p(t) = p0 [ sqrt(1 - D^2) + sqrt(2) D cos(phase) ], written as the angle
    // the gain pair is turned by: d(t) = (p(t) - p0) pi/4 = panA cos(phase) + panB.
    const double depth = std::clamp(static_cast<double>(v[perc::PanDepth]), 0.0, 1.0);
    double stand = std::sqrt(std::max(0.0, 1.0 - depth * depth)), swing = kPanRms * depth;
    // Keep the swing inside the field. |p0| (stand + swing) can exceed 1 at an intermediate depth
    // even when it does not at D = 1 (the sum peaks at sqrt(3) around D = 0.82), so the guard reads
    // the actual depth. Scaling both parts keeps the shape of the movement and never folds a
    // position past the edge, where the panner would invert a channel. No default kit reaches it:
    // the widest lane stands at 0.60 and asks for 0.849 at D = 1.
    const double reach = std::fabs(p0) * (stand + swing);
    if (reach > 1.0) { stand /= reach; swing /= reach; }
    double a = p0 * swing * kPiD / 4.0;
    double b = p0 * (stand - 1.0) * kPiD / 4.0;
    // And inside the angle the kernel's Taylor series is good for.
    const double span = std::fabs(a) + std::fabs(b);
    if (span > kPanMaxAngle) { const double k = kPanMaxAngle / span; a *= k; b *= k; }
    c_.panA[l] = static_cast<float>(a);
    c_.panB[l] = static_cast<float>(b);
    updatePanRate(l);
    assignPanGroups();
}

void PercKit::updatePanRate(int l)
{
    const double bars = std::clamp(static_cast<double>(values_[l][perc::PanBars]), 0.0625, 64.0);
    const double w = 2.0 * kPiD / std::max(1.0, bars * 4.0 * 60.0 / bpm_ * sr_);
    c_.panC[l] = static_cast<float>(std::cos(w));
    c_.panS[l] = static_cast<float>(std::sin(w));
}

void PercKit::trigger(int l, float velocity, int shift, double late)
{
    if (l < 0 || l >= kPercLanes || !valid_[l]) return;
    const double mul = std::pow(2.0, shift / 12.0);
    if (mul != shiftMul_[l]) { shiftMul_[l] = mul; computeCoefs(l); }
    const double vel = clampv(static_cast<double>(velocity), 0.0, 1.0);

    s_.envA[l] = static_cast<float>(vel * std::pow(static_cast<double>(c_.dA[l]), late));
    s_.envP[l] = static_cast<float>(std::pow(static_cast<double>(c_.dP[l]), late));
    const int bursts = static_cast<int>(std::lround(values_[l][perc::Bursts]));
    burstsLeft_[l] = bursts - 1;
    burstTimer_[l] = burstSpacing_[l] - late;
    burstVel_[l] = static_cast<float>(vel);
    const double nf = bursts > 1 ? noiseFast_[l] : noiseTail_[l];
    s_.envN[l] = static_cast<float>(vel * std::pow(nf, late));

    // The carrier starts at phase zero, `late` samples ago at its starting frequency.
    const double wStart = std::min(static_cast<double>(c_.w0[l]) * (1.0 + c_.pAmt[l]), 1.2);
    s_.cr[l] = static_cast<float>(std::cos(wStart * late));
    s_.ci[l] = static_cast<float>(std::sin(wStart * late));
    s_.mr[l] = static_cast<float>(std::cos(std::atan2(c_.ms[l], c_.mc[l]) * late));
    s_.mi[l] = static_cast<float>(std::sin(std::atan2(c_.ms[l], c_.mc[l]) * late));
    // Modes: energy added to the ring, so a roll sums.
    for (int m = 0; m < kPercModes; ++m) {
        const double a = modeAmp_[m][l] * vel * std::pow(modeR_[m][l], late);
        s_.zr[m][l] += static_cast<float>(a * std::cos(modeW_[m][l] * late));
        s_.zi[m][l] += static_cast<float>(a * std::sin(modeW_[m][l] * late));
    }
    // Chokes: this lane opens, the others in its group close within 8 ms.
    s_.choke[l] = 1.0f;
    c_.chokeD[l] = 1.0f;
    if (choke_[l] > 0) {
        for (int j = 0; j < kPercLanes; ++j) {
            if (j != l && choke_[j] == choke_[l]) c_.chokeD[j] = chokeFactor_;
        }
    }
}

template <class V>
void PercKit::processWith(float* L, float* R, int total)
{
    const int width = laneWidth<V>();
    int done = 0;
    while (done < total) {
        const int n = std::min(kMaxBlock, total - done);
        // Which sources each group of eight lanes needs; the same decision for every vector width.
        bool tone[2] = {}, modal[2] = {}, metal[2] = {}, noisy[2] = {};
        for (int l = 0; l < kPercLanes; ++l) {
            const int g = l / 8;
            tone[g] = tone[g] || c_.wTone[l] != 0.0f;
            modal[g] = modal[g] || c_.wModal[l] != 0.0f;
            metal[g] = metal[g] || c_.wMetal[l] != 0.0f;
            noisy[g] = noisy[g] || c_.wNoise[l] != 0.0f;
        }
        // Scalar preparation: noise, burst restarts, noise decay per sample.
        for (int l = 0; l < kPercLaneSlots; ++l) {
            const bool live = l < kPercLanes;
            for (int i = 0; i < n; ++i) {
                const size_t row = static_cast<size_t>(i * kPercLaneSlots + l);
                reset_[row] = 0.0f;
                if (!live) { noise_[row] = 0.0f; dNoise_[row] = 1.0f; continue; }
                noise_[row] = noisy[l / 8] ? noiseRng_[l].bipolar() : 0.0f;
                if (burstsLeft_[l] > 0) {
                    burstTimer_[l] -= 1.0;
                    if (burstTimer_[l] <= 0.0) {
                        reset_[row] = burstVel_[l];
                        --burstsLeft_[l];
                        burstTimer_[l] += burstSpacing_[l];
                    }
                }
                dNoise_[row] = burstsLeft_[l] > 0 ? noiseFast_[l] : noiseTail_[l];
            }
        }
        for (int l = 0; l < kPercLaneSlots; l += width) {
            const int g = std::min(l / 8, 1);
            percKernel<V>(s_, c_, l, n, noise_.data(), reset_.data(), dNoise_.data(), tone[g], modal[g], metal[g], panning_,
                          outL_.data(), outR_.data());
        }
        for (int i = 0; i < n; ++i) {
            float sl = 0.0f, sr = 0.0f;
            for (int l = 0; l < kPercLanes; ++l) {
                sl += outL_[static_cast<size_t>(i * kPercLaneSlots + l)];
                sr += outR_[static_cast<size_t>(i * kPercLaneSlots + l)];
            }
            L[done + i] = sl;
            R[done + i] = sr;
        }
        done += n;
    }
}

template void PercKit::processWith<float>(float*, float*, int);
#if PHOS_VEC_PATH != 0
template void PercKit::processWith<VecF>(float*, float*, int);
#endif

void PercKit::process(float* L, float* R, int n)
{
    processWith<VecF>(L, R, n);
}

} // namespace phos
