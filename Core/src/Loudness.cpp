/**
 * @file Loudness.cpp
 * @brief BS.1770 meter implementation.
 * @note Copied from Noctuary `Core/src/Loudness.cpp` at b60a2fe (15.09.2026), sone model removed.
 */
#include "phos/Loudness.h"
#include "phos/Dsp.h"
#include <algorithm>
#include <cmath>

namespace phos {

void KFilter::prepare(double sr)
{
    // Not the RBJ cookbook: the cookbook shelf with the standard's f0, Q and gain lands about two
    // per cent away from the published 48 kHz coefficients. This formulation reproduces them.
    {
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K = std::tan(kPi * f0 / sr);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        shelf_.b0 = static_cast<float>((Vh + Vb * K / Q + K * K) / a0);
        shelf_.b1 = static_cast<float>(2.0 * (K * K - Vh) / a0);
        shelf_.b2 = static_cast<float>((Vh - Vb * K / Q + K * K) / a0);
        shelf_.a1 = static_cast<float>(2.0 * (K * K - 1.0) / a0);
        shelf_.a2 = static_cast<float>((1.0 - K / Q + K * K) / a0);
    }
    {   // The RLB high-pass; its numerator is exactly 1, -2, 1 as the standard prints it.
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K = std::tan(kPi * f0 / sr);
        const double d = 1.0 + K / Q + K * K;
        hp_.b0 = 1.0f; hp_.b1 = -2.0f; hp_.b2 = 1.0f;
        hp_.a1 = static_cast<float>(2.0 * (K * K - 1.0) / d);
        hp_.a2 = static_cast<float>((1.0 - K / Q + K * K) / d);
    }
    reset();
}

void KFilter::reset() { shelf_.reset(); hp_.reset(); }

float KFilter::process(float x) { return hp_.process(shelf_.process(x)); }

void LoudnessMeter::prepare(double sampleRate)
{
    sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    hopLen_   = std::max(1, static_cast<int>(sr_ * 0.1));
    blockLen_ = hopLen_ * kHopsPerBlock;
    kL_.prepare(sr_);
    kR_.prepare(sr_);
    sumL_.assign(static_cast<size_t>(kHopsPerShort), 0.0);
    sumR_.assign(static_cast<size_t>(kHopsPerShort), 0.0);
    blocks_.prepare(kBlockLog);
    shortBlocks_.prepare(kBlockLog);
    reset();
}

void LoudnessMeter::reset()
{
    kL_.reset(); kR_.reset();
    std::fill(sumL_.begin(), sumL_.end(), 0.0);
    std::fill(sumR_.begin(), sumR_.end(), 0.0);
    ringPos_ = 0; ringFilled_ = 0; hopPos_ = 0;
    hopL_ = hopR_ = 0.0; hopSamples_ = 0;
    blocks_.clear(); shortBlocks_.clear();
    truePeak_ = 0.0; seconds_ = 0.0; lastShort_ = -120.0f;
    for (int i = 0; i < 4; ++i) { tpHistL_[i] = 0.0f; tpHistR_[i] = 0.0f; }
}

namespace {

/** @brief Loudness of a mean-square pair, both channel weights one. */
inline float lufs(double msL, double msR)
{
    const double s = msL + msR;
    return s > 1.0e-12 ? static_cast<float>(-0.691 + 10.0 * std::log10(s)) : -120.0f;
}

/**
 * @brief Inter-sample peak estimate by four-point Lagrange interpolation at quarter samples.
 *
 * Not the standard's 48-tap filter; within a few tenths of a decibel of it on programme material,
 * which is enough to tell "fine" from "will clip after encoding".
 */
inline double interPeak(const float* h)
{
    double peak = std::fabs(static_cast<double>(h[1]));
    for (int k = 1; k < 4; ++k) {
        const double t = k * 0.25;
        const double a = h[0], b = h[1], c = h[2], d = h[3];
        const double v = b + 0.5 * t * (c - a + t * (2.0 * a - 5.0 * b + 4.0 * c - d + t * (3.0 * (b - c) + d - a)));
        peak = std::max(peak, std::fabs(v));
    }
    return peak;
}

} // namespace

void LoudnessMeter::pushBlock()
{
    sumL_[static_cast<size_t>(ringPos_)] = hopSamples_ > 0 ? hopL_ / static_cast<double>(hopSamples_) : 0.0;
    sumR_[static_cast<size_t>(ringPos_)] = hopSamples_ > 0 ? hopR_ / static_cast<double>(hopSamples_) : 0.0;
    ringPos_ = (ringPos_ + 1) % kHopsPerShort;
    if (ringFilled_ < kHopsPerShort) ++ringFilled_;
    hopL_ = hopR_ = 0.0; hopSamples_ = 0;

    auto meanOver = [this](int hops, double& l, double& r) {
        const int n = std::min(hops, ringFilled_);
        l = r = 0.0;
        for (int k = 1; k <= n; ++k) {
            const int idx = (ringPos_ - k + kHopsPerShort) % kHopsPerShort;
            l += sumL_[static_cast<size_t>(idx)];
            r += sumR_[static_cast<size_t>(idx)];
        }
        if (n > 0) { l /= n; r /= n; }
        return n;
    };

    double bl = 0.0, br = 0.0;
    if (meanOver(kHopsPerBlock, bl, br) == kHopsPerBlock) blocks_.push(lufs(bl, br));
    double sl = 0.0, sr = 0.0;
    if (meanOver(kHopsPerShort, sl, sr) == kHopsPerShort) {
        lastShort_ = lufs(sl, sr);
        shortBlocks_.push(lastShort_);
    }
}

void LoudnessMeter::process(const float* L, const float* R, int n)
{
    for (int i = 0; i < n; ++i) {
        const float l = L[i], r = R[i];
        tpHistL_[0] = tpHistL_[1]; tpHistL_[1] = tpHistL_[2]; tpHistL_[2] = tpHistL_[3]; tpHistL_[3] = l;
        tpHistR_[0] = tpHistR_[1]; tpHistR_[1] = tpHistR_[2]; tpHistR_[2] = tpHistR_[3]; tpHistR_[3] = r;
        truePeak_ = std::max(truePeak_, std::max(interPeak(tpHistL_), interPeak(tpHistR_)));

        const float kl = kL_.process(l), kr = kR_.process(r);
        hopL_ += static_cast<double>(kl) * kl;
        hopR_ += static_cast<double>(kr) * kr;
        ++hopSamples_;
        if (++hopPos_ >= hopLen_) { hopPos_ = 0; pushBlock(); }
    }
    seconds_ += static_cast<double>(n) / sr_;
}

LoudnessReading LoudnessMeter::read() const
{
    LoudnessReading out;
    out.seconds = static_cast<float>(seconds_);
    out.shortTerm = lastShort_;
    {
        double l = 0.0, r = 0.0;
        const int n = std::min(kHopsPerBlock, ringFilled_);
        for (int k = 1; k <= n; ++k) {
            const int idx = (ringPos_ - k + kHopsPerShort) % kHopsPerShort;
            l += sumL_[static_cast<size_t>(idx)];
            r += sumR_[static_cast<size_t>(idx)];
        }
        if (n > 0) out.momentary = lufs(l / n, r / n);
    }
    // Integrated: absolute gate at -70 LUFS, then a relative gate 10 LU under the mean of the rest.
    if (!blocks_.empty()) {
        const std::vector<float> bl = blocks_.snapshot();
        double sum = 0.0; int count = 0;
        for (float b : bl) if (b > -70.0f) { sum += std::pow(10.0, (b + 0.691) / 10.0); ++count; }
        if (count > 0) {
            const float gate = static_cast<float>(-0.691 + 10.0 * std::log10(sum / count)) - 10.0f;
            double s2 = 0.0; int c2 = 0;
            for (float b : bl) if (b > -70.0f && b > gate) { s2 += std::pow(10.0, (b + 0.691) / 10.0); ++c2; }
            if (c2 > 0) out.integrated = static_cast<float>(-0.691 + 10.0 * std::log10(s2 / c2));
        }
    }
    // Loudness range (EBU Tech 3342): short-term values above a -20 LU relative gate, 10th to 95th centile.
    if (shortBlocks_.size() >= 10) {
        const std::vector<float> sb = shortBlocks_.snapshot();
        std::vector<float> v;
        v.reserve(sb.size());
        double sum = 0.0; int count = 0;
        for (float b : sb) if (b > -70.0f) { sum += std::pow(10.0, (b + 0.691) / 10.0); ++count; }
        if (count > 0) {
            const float gate = static_cast<float>(-0.691 + 10.0 * std::log10(sum / count)) - 20.0f;
            for (float b : sb) if (b > gate) v.push_back(b);
            if (v.size() >= 10) {
                std::sort(v.begin(), v.end());
                const size_t lo = static_cast<size_t>(0.10 * static_cast<double>(v.size() - 1));
                const size_t hi = static_cast<size_t>(0.95 * static_cast<double>(v.size() - 1));
                out.range = v[hi] - v[lo];
            }
        }
    }
    out.truePeak = truePeak_ > 1.0e-9 ? static_cast<float>(20.0 * std::log10(truePeak_)) : -120.0f;
    if (out.shortTerm > -119.0f && out.truePeak > -119.0f) out.crest = out.truePeak - out.shortTerm;
    return out;
}

} // namespace phos
