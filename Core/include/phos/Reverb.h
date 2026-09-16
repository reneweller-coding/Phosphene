/**
 * @file Reverb.h
 * @brief Eight-line feedback delay network reverb for the send buses (room and hall).
 *
 * Each channel passes its own pre-delay and a four-stage Schroeder all-pass diffusion into four of the
 * eight lines, and reads those same four back (Reverb.cpp, kLeft); a Householder reflection mixes all
 * eight. The four are not the first half or the second: the line set is ordered by length, so that
 * split handed one channel every short line and the other every long one and the two returns came out
 * with different colours (16.09.2026: 1.45 dB rms between the third octaves 500 Hz .. 8 kHz in the
 * room, 1.36 in the hall, worst band 2.55). Interleaved so that each channel holds two short lines and
 * two long ones, the returns are as decorrelated as before -- they share no line -- and now differ by
 * 0.65 and 0.53 dB rms.
 * Every line carries a short all-pass inside its loop, which scatters each echo into many so the echo
 * density grows quickly (Schlecht and Habets, "Scattering in feedback delay networks", IEEE/ACM TASLP
 * 2020), and the line lengths are the set Noctuary's optimiser found for the flattest third-octave
 * magnitude of the tail (after the colourless-FDN work of Dal Santo, Prawda, Schlecht and Valimaki).
 * Each line's gain gives the decay time exactly: g = 10^(-3 L / (T60 fs)). A one-pole low pass in every
 * loop darkens the tail; a high cut and a 12 dB/octave low cut sit on the return.
 *
 * **Depth rule.** The return's low cut never goes below 150 Hz: a tail under 140 Hz would smear
 * exactly the band where kick and bass are phase-locked.
 *
 * @note Adapted from Noctuary `Core/include/ambient/Effects.h` (class Reverb) at b60a2fe (15.09.2026):
 *       the network, diffusion, scattering, line lengths, damping and return filters are unchanged; the
 *       freeze, the rotating-matrix mode and the classic line set are left behind, the pre-delay is set
 *       in samples by the caller (so it can follow the tempo), and the output is wet only.
 */
#pragma once
#include "phos/Dsp.h"
#include <vector>

namespace phos {

/** @brief Stereo FDN reverb, wet output only. */
class Reverb {
public:
    /** @brief Allocates for a sample rate. */
    void prepare(double sampleRate);
    /** @brief Clears every line. */
    void reset();
    /**
     * @brief Sets the space.
     * @param size          line-length scale, 0.3 .. 3 (1 = 30 to 90 ms lines)
     * @param decaySeconds  T60 of the tail
     * @param damping       0..1, high-frequency loss per loop
     * @param preDelaySamples pre-delay
     * @param lowCutHz      return low cut (at least 150 Hz)
     * @param highCutHz     return high cut
     */
    void set(float size, float decaySeconds, float damping, float preDelaySamples, float lowCutHz, float highCutHz);
    /**
     * @brief Processes a stereo send.
     * @param inL,inR   send input
     * @param outL,outR receives the wet return (replaced)
     */
    void process(const float* inL, const float* inR, float* outL, float* outR, int n);

private:
    static constexpr int kLines = 8;
    static constexpr int kAllpasses = 4;
    double sr_ = 48000.0;
    std::vector<float> line_[kLines], sc_[kLines], ap_[kAllpasses], apR_[kAllpasses], pre_, preR_;
    int mask_ = 0, w_ = 0;
    int apLen_[kAllpasses] = {}, scLen_[kLines] = {};
    float lenTarget_[kLines] = {}, lenCur_[kLines] = {}, gain_[kLines] = {}, lp_[kLines] = {};
    double modPh_[kLines] = {};
    float modRate_[kLines] = {};
    float preTarget_ = 0.0f, preCur_ = 0.0f;
    float damp_ = 0.4f;
    float dcX_[2] = {}, dcY_[2] = {}, dcR_ = 0.999f;
    float hcCoef_ = 1.0f, hcL_ = 0.0f, hcR_ = 0.0f;
    float lcCoef_ = 0.0f, lcL1_ = 0.0f, lcR1_ = 0.0f, lcL2_ = 0.0f, lcR2_ = 0.0f;
};

} // namespace phos
