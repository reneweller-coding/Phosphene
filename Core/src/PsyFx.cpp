/**
 * @file PsyFx.cpp
 * @brief Flanger, phaser, frequency shifter, their chain, and the stutter (PsyFx.h).
 */
#include "phos/PsyFx.h"
#include "phos/Params.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {
constexpr double kPiD = 3.141592653589793;

/** @brief Raised-cosine ramp 0..1 for u in [0, 1], clamped outside. */
inline float smoothRamp(double u)
{
    if (u >= 1.0) return 1.0f;
    if (u <= 0.0) return 0.0f;
    return static_cast<float>(0.5 - 0.5 * std::cos(kPiD * u));
}

/** @brief The fractional part of a beat position divided by a period: an LFO phase in [0, 1). */
inline double lfoPhase(double beat, double period)
{
    const double p = beat / std::max(1e-6, period);
    return p - std::floor(p);
}

/**
 * @brief The two all-pass chains of the 90-degree network (PsyFx.h), four second-order sections each.
 *
 * Their squares are what the difference equation uses. The one-sample delay belongs to chain A. The
 * phase difference was measured, not taken on trust: the self test drives the pair with sines from
 * 50 Hz to 20 kHz and reads the angle between the two outputs.
 */
constexpr double kHilbertA[4] = { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
constexpr double kHilbertB[4] = { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };
} // namespace

// ---------------------------------------------------------------------------------------------
// Flanger

void Flanger::prepare(double sampleRate)
{
    sr_ = sampleRate;
    size_t n = 1;
    while (static_cast<double>(n) < 0.02 * sampleRate) n <<= 1;   // 20 ms: well past the 6.3 ms sweep
    bufL_.assign(n, 0.0f);
    bufR_.assign(n, 0.0f);
    mask_ = n - 1;
    reset();
}

void Flanger::reset()
{
    std::fill(bufL_.begin(), bufL_.end(), 0.0f);
    std::fill(bufR_.begin(), bufR_.end(), 0.0f);
    write_ = 0;
}

void Flanger::set(float periodBeats, float depth, float feedback, float mix)
{
    period_ = std::max(0.0625f, periodBeats);
    depth_ = clampv(depth, 0.0f, 1.0f);
    fb_ = clampv(feedback, -0.9f, 0.9f);
    mix_ = clampv(mix, 0.0f, 1.0f);
}

float Flanger::read(const std::vector<float>& b, double delay) const
{
    const double pos = static_cast<double>(write_) - delay;
    const double fl = std::floor(pos);
    const float frac = static_cast<float>(pos - fl);
    const size_t i0 = static_cast<size_t>(static_cast<long long>(fl)) & mask_;
    const size_t i1 = (i0 + 1) & mask_;
    return b[i0] + frac * (b[i1] - b[i0]);
}

void Flanger::tick(float& l, float& r, double beat)
{
    const double ph = lfoPhase(beat, period_);
    // 0.3 ms to 6 ms: the whole audible comb range, from a metallic sheen to a jet sweep.
    const double spanMs = 5.7 * depth_;
    const double dl = (0.3 + spanMs * (0.5 - 0.5 * std::cos(2.0 * kPiD * ph))) * 0.001 * sr_;
    const double dr = (0.3 + spanMs * (0.5 - 0.5 * std::cos(2.0 * kPiD * (ph + 0.25)))) * 0.001 * sr_;
    const float yl = read(bufL_, dl), yr = read(bufR_, dr);
    bufL_[write_] = l + fb_ * yl;
    bufR_[write_] = r + fb_ * yr;
    write_ = (write_ + 1) & mask_;
    l = (1.0f - mix_) * l + mix_ * 0.5f * (l + yl);
    r = (1.0f - mix_) * r + mix_ * 0.5f * (r + yr);
}

// ---------------------------------------------------------------------------------------------
// Phaser

void Phaser::prepare(double sampleRate)
{
    sr_ = sampleRate;
    reset();
}

void Phaser::reset()
{
    for (int k = 0; k < kStages; ++k) zL_[k] = zR_[k] = 0.0f;
    fbL_ = fbR_ = 0.0f;
}

void Phaser::set(float periodBeats, float depth, float feedback, float mix)
{
    period_ = std::max(0.0625f, periodBeats);
    depth_ = clampv(depth, 0.0f, 1.0f);
    fb_ = clampv(feedback, 0.0f, 0.9f);
    mix_ = clampv(mix, 0.0f, 1.0f);
}

float Phaser::stage(float x, float a, float* z, float& fbState)
{
    // Six first-order all-passes H = (a + z^-1) / (1 + a z^-1) in cascade, transposed direct form II,
    // with the last stage fed back to the input.
    float y = x + fb_ * fbState;
    for (int k = 0; k < kStages; ++k) {
        const float o = a * y + z[k];
        z[k] = y - a * o;
        y = o;
    }
    fbState = y;
    return y;
}

void Phaser::tick(float& l, float& r, double beat)
{
    const double ph = lfoPhase(beat, period_);
    auto coef = [&](double phase) {
        const double lfo = 0.5 - 0.5 * std::cos(2.0 * kPiD * phase);
        const double f = std::min(200.0 * std::pow(2.0, 5.0 * depth_ * lfo), 0.45 * sr_);
        const double t = std::tan(kPiD * f / sr_);
        return static_cast<float>((t - 1.0) / (t + 1.0));
    };
    const float yl = stage(l, coef(ph), zL_, fbL_);
    const float yr = stage(r, coef(ph + 0.25), zR_, fbR_);
    l = (1.0f - mix_) * l + mix_ * 0.5f * (l + yl);
    r = (1.0f - mix_) * r + mix_ * 0.5f * (r + yr);
}

// ---------------------------------------------------------------------------------------------
// Frequency shifter

void HilbertPair::reset()
{
    for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 2; ++j) xa[k][j] = ya[k][j] = xb[k][j] = yb[k][j] = 0.0;
    delayA = 0.0;
}

void HilbertPair::tick(double x, double& i, double& q)
{
    double a = x, b = x;
    for (int k = 0; k < 4; ++k) {
        const double ca = kHilbertA[k] * kHilbertA[k];
        const double ya0 = ca * (a + ya[k][1]) - xa[k][1];
        xa[k][1] = xa[k][0]; xa[k][0] = a;
        ya[k][1] = ya[k][0]; ya[k][0] = ya0;
        a = ya0;
        const double cb = kHilbertB[k] * kHilbertB[k];
        const double yb0 = cb * (b + yb[k][1]) - xb[k][1];
        xb[k][1] = xb[k][0]; xb[k][0] = b;
        yb[k][1] = yb[k][0]; yb[k][0] = yb0;
        b = yb0;
    }
    // Chain A lags chain B by 90 degrees once A is delayed by a sample: B is the in-phase output.
    q = delayA;
    delayA = a;
    i = b;
}

void FreqShifter::prepare(double sampleRate)
{
    sr_ = sampleRate;
    reset();
}

void FreqShifter::reset()
{
    hl_.reset();
    hr_.reset();
    phase_ = 0.0;
}

void FreqShifter::set(float hz, float mix)
{
    hz_ = hz;
    mix_ = clampv(mix, 0.0f, 1.0f);
}

void FreqShifter::tick(float& l, float& r)
{
    double il, ql, ir, qr;
    hl_.tick(l, il, ql);
    hr_.tick(r, ir, qr);
    phase_ += static_cast<double>(hz_) / sr_;
    phase_ -= std::floor(phase_);
    const double c = std::cos(2.0 * kPiD * phase_), s = std::sin(2.0 * kPiD * phase_);
    // cos(psi + theta) = I cos(theta) - Q sin(theta): every partial moves up by hz_. The right channel's
    // carrier runs 90 degrees ahead (cos -> -sin, sin -> cos), which decorrelates the two sides without
    // putting them in antiphase.
    const float sl = static_cast<float>(il * c - ql * s);
    const float sr = static_cast<float>(ir * (-s) - qr * c);
    // At zero shift the wet path is only the all-pass network's phase, which beside the dry signal
    // would comb; the wet share therefore fades in over the first 5 Hz of shift.
    const float m = mix_ * std::min(1.0f, std::fabs(hz_) / 5.0f);
    l = (1.0f - m) * l + m * sl;
    r = (1.0f - m) * r + m * sr;
}

// ---------------------------------------------------------------------------------------------
// Chain

void PsyFxChain::prepare(double sampleRate)
{
    flanger_.prepare(sampleRate);
    phaser_.prepare(sampleRate);
    shifter_.prepare(sampleRate);
}

void PsyFxChain::reset()
{
    flanger_.reset();
    phaser_.reset();
    shifter_.reset();
    motionHz_ = motionFlange_ = 0.0f;
}

void PsyFxChain::update(const float* v)
{
    for (int k = 0; k < psyfx::Count; ++k) v_[k] = v[k];
    apply();
}

void PsyFxChain::setMotion(float shiftHz, float flangerMix)
{
    motionHz_ = shiftHz;
    motionFlange_ = flangerMix;
    apply();
}

void PsyFxChain::apply()
{
    flanger_.set(v_[psyfx::FlangerBeats], v_[psyfx::FlangerDepth], v_[psyfx::FlangerFeedback],
                 v_[psyfx::FlangerMix] + motionFlange_);
    phaser_.set(v_[psyfx::PhaserBeats], v_[psyfx::PhaserDepth], v_[psyfx::PhaserFeedback], v_[psyfx::PhaserMix]);
    shifter_.set(v_[psyfx::ShiftHz] + motionHz_, v_[psyfx::ShiftMix]);
}

void PsyFxChain::process(float* L, float* R, int n, double beat0, double bps)
{
    for (int i = 0; i < n; ++i) {
        const double beat = beat0 + static_cast<double>(i) * bps;
        float l = L[i], r = R[i];
        flanger_.tick(l, r, beat);
        phaser_.tick(l, r, beat);
        shifter_.tick(l, r);
        L[i] = l;
        R[i] = r;
    }
}

// ---------------------------------------------------------------------------------------------
// Stutter

void Stutter::prepare(double sampleRate)
{
    recL_.assign(static_cast<size_t>(2.0 * sampleRate), 0.0f);
    recR_.assign(recL_.size(), 0.0f);
    fade_ = std::max(8, static_cast<int>(0.001 * sampleRate));
    reset();
}

void Stutter::reset()
{
    on_ = false;
    pos_ = length_ = 0;
    slice_ = 1;
}

void Stutter::trigger(int lengthSamples, int sliceSamples)
{
    on_ = lengthSamples > 0;
    pos_ = 0;
    length_ = lengthSamples;
    slice_ = std::clamp<long long>(sliceSamples, 4LL * fade_, static_cast<long long>(recL_.size()));
}

void Stutter::tick(float& l, float& r)
{
    if (!on_) return;
    const size_t cap = recL_.size();
    if (static_cast<size_t>(pos_) < cap) {
        recL_[static_cast<size_t>(pos_)] = l;
        recR_[static_cast<size_t>(pos_)] = r;
    }
    // The slice, halved over the last quarter of the event (a roll), and where in it this sample is.
    const long long q0 = length_ - length_ / 4;
    const long long sl = pos_ < q0 ? slice_ : std::max<long long>(slice_ / 2, 2LL * fade_);
    const long long j = pos_ < q0 ? pos_ % sl : (pos_ - q0) % sl;
    const bool first = pos_ < slice_;   // the first slice is the live signal itself
    const double f = static_cast<double>(fade_);
    const float head = first ? 1.0f : smoothRamp(static_cast<double>(j) / f);
    const float tail = smoothRamp(static_cast<double>(sl - 1 - j) / f);
    const size_t at = static_cast<size_t>(std::min<long long>(j, static_cast<long long>(cap) - 1));
    const float g = std::min(head, tail);
    const float rl = recL_[at] * g, rr = recR_[at] * g;
    // Back to the live signal over the last millisecond of the event.
    const float e = smoothRamp(static_cast<double>(length_ - pos_) / f);
    l = e * rl + (1.0f - e) * l;
    r = e * rr + (1.0f - e) * r;
    if (++pos_ >= length_) on_ = false;
}

} // namespace phos
