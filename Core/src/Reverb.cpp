/**
 * @file Reverb.cpp
 * @brief FDN reverb implementation.
 */
#include "phos/Reverb.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {
int pow2At(int n) { int p = 1; while (p < n) p <<= 1; return p; }

/**
 * @brief Which output channel each delay line feeds, and which input it is driven by (1 = left).
 *
 * Not the first half against the second. The line set is ordered by length (29.7 .. 89.0 ms at size
 * one), so splitting it in the middle gave the left channel the four short lines and the right the
 * four long ones -- and a line's length is where its comb peaks sit, so the two returns were not the
 * same instrument: measured over the third octaves from 500 Hz to 8 kHz they differed by 1.45 dB rms
 * in the room and 1.36 in the hall, worst band 2.05 and 2.55 dB. Each channel now takes two short
 * lines and two long ones ({29.7, 46.6, 58.6, 68.4} against {31.4, 39.3, 55.1, 89.0}), which leaves
 * the two returns as decorrelated as before -- they still share no line -- while giving them the same
 * distribution of delays and therefore the same colour. The sign an output tap carries is the sign
 * that line's input carries, so the first pass through a line still adds rather than cancels.
 */
constexpr int kLeft[8] = { 1, 0, 0, 1, 0, 1, 1, 0 };

/** @brief Linear-interpolated read @p delay samples behind the write position @p w. */
inline float ringRead(const float* buf, int mask, int w, float delay)
{
    // In double: a float write index of 2^17 resolves only 1/64 of a sample.
    const double pos = static_cast<double>(w) - static_cast<double>(delay);
    const double fl = std::floor(pos);
    const float frac = static_cast<float>(pos - fl);
    const int i0 = static_cast<int>(fl) & mask;
    return buf[i0] + frac * (buf[(i0 + 1) & mask] - buf[i0]);
}
} // namespace

void Reverb::prepare(double sampleRate)
{
    sr_ = sampleRate;
    // The longest line at size 3 plus the pre-delay of up to half a second.
    const int size = pow2At(static_cast<int>(0.1 * 3.0 * sr_ + 0.6 * sr_) + 64);
    mask_ = size - 1;
    for (auto& l : line_) l.assign(static_cast<size_t>(size), 0.0f);
    for (auto& a : ap_) a.assign(static_cast<size_t>(size), 0.0f);
    for (auto& a : apR_) a.assign(static_cast<size_t>(size), 0.0f);
    for (auto& s : sc_) s.assign(static_cast<size_t>(size), 0.0f);
    pre_.assign(static_cast<size_t>(size), 0.0f);
    preR_.assign(static_cast<size_t>(size), 0.0f);
    static const float kApMs[kAllpasses] = { 5.1f, 7.3f, 11.3f, 13.7f };
    for (int k = 0; k < kAllpasses; ++k) apLen_[k] = std::max(1, static_cast<int>(kApMs[k] * sr_ / 1000.0));
    static const float kScMs[kLines] = { 1.9f, 2.3f, 2.9f, 3.7f, 4.3f, 5.3f, 6.1f, 7.1f };
    for (int l = 0; l < kLines; ++l) scLen_[l] = std::max(1, static_cast<int>(kScMs[l] * sr_ / 1000.0));
    static const float kModHz[kLines] = { 0.11f, 0.13f, 0.17f, 0.19f, 0.23f, 0.29f, 0.31f, 0.37f };
    for (int l = 0; l < kLines; ++l) modRate_[l] = kModHz[l];
    dcR_ = 1.0f - kTwoPi * 5.0f / static_cast<float>(sr_);
    reset();
    set(1.0f, 2.0f, 0.4f, 0.0f, 250.0f, 9000.0f);
}

void Reverb::reset()
{
    for (auto& l : line_) std::fill(l.begin(), l.end(), 0.0f);
    for (auto& a : ap_) std::fill(a.begin(), a.end(), 0.0f);
    for (auto& a : apR_) std::fill(a.begin(), a.end(), 0.0f);
    for (auto& s : sc_) std::fill(s.begin(), s.end(), 0.0f);
    std::fill(pre_.begin(), pre_.end(), 0.0f);
    std::fill(preR_.begin(), preR_.end(), 0.0f);
    w_ = 0;
    for (int l = 0; l < kLines; ++l) { lp_[l] = 0.0f; modPh_[l] = l / static_cast<double>(kLines); lenCur_[l] = 0.0f; }
    preCur_ = -1.0f;
    dcX_[0] = dcX_[1] = dcY_[0] = dcY_[1] = 0.0f;
    hcL_ = hcR_ = lcL1_ = lcR1_ = lcL2_ = lcR2_ = 0.0f;
}

void Reverb::set(float size, float decaySeconds, float damping, float preDelaySamples, float lowCutHz, float highCutHz)
{
    // Noctuary's colourless line set (Tools/optimise_fdn.py, 0.314 dB third-octave spread).
    static const float kFlatMs[kLines] = { 29.7f, 31.4f, 39.3f, 46.6f, 55.1f, 58.6f, 68.4f, 89.0f };
    const float s = clampv(size, 0.3f, 3.0f);
    const float decay = std::max(decaySeconds, 0.1f);
    damp_ = clampv(damping, 0.0f, 1.0f);
    const float maxLen = static_cast<float>(mask_) - 8.0f;
    for (int l = 0; l < kLines; ++l) {
        lenTarget_[l] = std::min(kFlatMs[l] * s * static_cast<float>(sr_ / 1000.0), maxLen);
        if (lenCur_[l] <= 0.0f) lenCur_[l] = lenTarget_[l];
        gain_[l] = std::pow(10.0f, -3.0f * lenTarget_[l] / (decay * static_cast<float>(sr_)));
    }
    preTarget_ = clampv(preDelaySamples, 0.0f, maxLen - lenTarget_[kLines - 1]);
    if (preCur_ < 0.0f) preCur_ = preTarget_;
    const float hc = clampv(highCutHz, 500.0f, 20000.0f);
    hcCoef_ = hc >= 19000.0f ? 1.0f : 1.0f - std::exp(-kTwoPi * hc / static_cast<float>(sr_));
    // Two cascaded one-poles are 6 dB down at their corner and 3 dB down at 1.554 times it.
    const float lc = std::max(150.0f, lowCutHz);
    lcCoef_ = 1.0f - std::exp(-kTwoPi * (lc / 1.5538f) / static_cast<float>(sr_));
}

void Reverb::process(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    const float lpc = 1.0f - 0.92f * damp_;
    constexpr float inGain = 0.5f, glide = 0.0005f;
    for (int i = 0; i < n; ++i) {
        const float xl0 = inL[i] - dcX_[0] + dcR_ * dcY_[0]; dcX_[0] = inL[i]; dcY_[0] = xl0;
        const float xr0 = inR[i] - dcX_[1] + dcR_ * dcY_[1]; dcX_[1] = inR[i]; dcY_[1] = xr0;
        pre_[static_cast<size_t>(w_ & mask_)] = xl0;
        preR_[static_cast<size_t>(w_ & mask_)] = xr0;
        preCur_ += (preTarget_ - preCur_) * glide;
        float xl = ringRead(pre_.data(), mask_, w_, preCur_ + 1.0f);
        float xr = ringRead(preR_.data(), mask_, w_, preCur_ + 1.0f);
        for (int k = 0; k < kAllpasses; ++k) {
            float* a = ap_[k].data();
            float* ar = apR_[k].data();
            const float d = a[(w_ - apLen_[k]) & mask_];
            const float dr = ar[(w_ - apLen_[k]) & mask_];
            const float y = d - 0.6f * xl;
            const float yr = dr - 0.6f * xr;
            a[w_ & mask_] = xl + 0.6f * y;
            ar[w_ & mask_] = xr + 0.6f * yr;
            xl = y;
            xr = yr;
        }
        float o[kLines];
        float sum = 0.0f;
        for (int l = 0; l < kLines; ++l) {
            lenCur_[l] += (lenTarget_[l] - lenCur_[l]) * glide;
            modPh_[l] += modRate_[l] / sr_;
            if (modPh_[l] >= 1.0) modPh_[l] -= 1.0;
            const float d = lenCur_[l] + 1.5f * sin01(modPh_[l]) + 2.0f;
            float v = ringRead(line_[l].data(), mask_, w_, d);
            float* sb = sc_[l].data();
            const float sd = sb[(w_ - scLen_[l]) & mask_];
            const float sy = sd - 0.5f * v;
            sb[w_ & mask_] = v + 0.5f * sy;
            v = sy;
            lp_[l] += lpc * (v - lp_[l]);
            o[l] = lp_[l];
            sum += o[l];
        }
        const float hh = sum * (2.0f / static_cast<float>(kLines));   // Householder reflection
        float wl = 0.0f, wr = 0.0f;
        for (int l = 0; l < kLines; ++l) {
            const float in = kLeft[l] ? xl : xr;
            line_[l][static_cast<size_t>(w_ & mask_)] = gain_[l] * (o[l] - hh) + ((l & 1) ? -inGain : inGain) * in;
            const float tap = (l & 1) ? -o[l] : o[l];
            if (kLeft[l] != 0) wl += tap; else wr += tap;
        }
        const float wetL = 0.3f * wl;
        const float wetR = 0.3f * wr;
        hcL_ += hcCoef_ * (wetL - hcL_);
        hcR_ += hcCoef_ * (wetR - hcR_);
        lcL1_ += lcCoef_ * (hcL_ - lcL1_);   const float aL = hcL_ - lcL1_;
        lcR1_ += lcCoef_ * (hcR_ - lcR1_);   const float aR = hcR_ - lcR1_;
        lcL2_ += lcCoef_ * (aL - lcL2_);
        lcR2_ += lcCoef_ * (aR - lcR2_);
        outL[i] = aL - lcL2_;
        outR[i] = aR - lcR2_;
        w_ = (w_ + 1) & mask_;
    }
}

} // namespace phos
