/**
 * @file TempoDelay.h
 * @brief Tempo-synchronised stereo feedback delay with a band limit inside the loop.
 *
 * Two delay lines, left and right, each with its own time in beats (a dotted sixteenth against a
 * quarter is the classic acid echo). Every repeat passes a 12 dB/octave high pass and a 12 dB/octave
 * low pass inside the feedback loop, so the echoes lose low end and top with each round: the repeats
 * never pile up under 140 Hz, where only kick and bass may play, and they darken like a tape echo
 * instead of ringing brighter than the source. A little of each side's output feeds the other side,
 * which spreads the tail.
 *
 * **Tempo.** The delay time in samples follows the tempo at every call of set(). Tempo ramps between
 * tracks move it by a fraction of a sample per chunk; the read position is interpolated linearly,
 * so the change neither clicks nor steps. A jump of the time setting glides over about 50 ms.
 */
#pragma once
#include "phos/Dsp.h"
#include <vector>

namespace phos {

/** @brief Stereo tempo delay. */
class TempoDelay {
public:
    /** @brief Allocates for a sample rate (up to two seconds of delay at any tempo the engine allows). */
    void prepare(double sampleRate)
    {
        sr_ = sampleRate;
        size_t n = 1;
        while (static_cast<double>(n) < 2.5 * sampleRate) n <<= 1;
        bufL_.assign(n, 0.0f);
        bufR_.assign(n, 0.0f);
        mask_ = n - 1;
        glide_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.05 * sampleRate)));
        reset();
    }
    /** @brief Clears the lines and filters. */
    void reset()
    {
        std::fill(bufL_.begin(), bufL_.end(), 0.0f);
        std::fill(bufR_.begin(), bufR_.end(), 0.0f);
        write_ = 0;
        hpL_.reset(); hpR_.reset(); lpL_.reset(); lpR_.reset();
        timeL_ = targetL_;
        timeR_ = targetR_;
    }
    /**
     * @brief Sets the delay.
     * @param beatsLeft,beatsRight delay times in beats
     * @param bpm      current tempo
     * @param feedback 0..0.9
     * @param hpHz,lpHz band limits inside the loop
     */
    void set(float beatsLeft, float beatsRight, double bpm, float feedback, float hpHz, float lpHz)
    {
        const double maxSamples = static_cast<double>(mask_) - 2.0;
        targetL_ = static_cast<float>(std::min(maxSamples, beatsLeft * 60.0 / bpm * sr_));
        targetR_ = static_cast<float>(std::min(maxSamples, beatsRight * 60.0 / bpm * sr_));
        if (!primed_) { timeL_ = targetL_; timeR_ = targetR_; primed_ = true; }
        feedback_ = clampv(feedback, 0.0f, 0.9f);
        const float sr = static_cast<float>(sr_);
        hpL_.setQ(std::max(hpHz, 150.0f), 0.70710678f, sr);
        hpR_.copyCoefficients(hpL_);
        lpL_.setQ(lpHz, 0.70710678f, sr);
        lpR_.copyCoefficients(lpL_);
    }
    /**
     * @brief Adds the delayed signal of a mono send to a stereo output.
     * @param send mono input, @p n samples (already scaled by the send amount)
     * @param L,R  outputs the echoes are added to
     * @param n    samples
     */
    void process(const float* send, float* L, float* R, int n)
    {
        // No idle shortcut: when a delay stops depending on where the host cuts its blocks, the output
        // would no longer be the same for every block size. Two lines and four filters are cheap.
        for (int i = 0; i < n; ++i) {
            timeL_ += (targetL_ - timeL_) * glide_;
            timeR_ += (targetR_ - timeR_) * glide_;
            const float yl = read(bufL_, timeL_), yr = read(bufR_, timeR_);
            float lp, bp, hp;
            // Loop filters: high pass, then low pass, then the cross-feed.
            hpL_.tick(yl, lp, bp, hp);
            const float fl = lpL_.lp(hp);
            hpR_.tick(yr, lp, bp, hp);
            const float fr = lpR_.lp(hp);
            const float x = send[i];
            bufL_[write_] = x + feedback_ * (0.85f * fl + 0.15f * fr);
            bufR_[write_] = x + feedback_ * (0.85f * fr + 0.15f * fl);
            write_ = (write_ + 1) & mask_;
            L[i] += fl;
            R[i] += fr;
        }

    }

private:
    /** @brief Reads @p buf @p delay samples back, linearly interpolated. */
    float read(const std::vector<float>& buf, float delay) const
    {
        // In double: at a write index of 2^17 a float resolves only 1/64 of a sample.
        const double pos = static_cast<double>(write_) - static_cast<double>(delay);
        const double fl = std::floor(pos);
        const float frac = static_cast<float>(pos - fl);
        const size_t i0 = static_cast<size_t>(static_cast<long long>(fl)) & mask_;
        const size_t i1 = (i0 + 1) & mask_;
        return buf[i0] + frac * (buf[i1] - buf[i0]);
    }

    double sr_ = 48000.0;   ///< sample rate
    std::vector<float> bufL_;   ///< the left delay line, a power of two long
    std::vector<float> bufR_;   ///< the right delay line
    size_t mask_ = 0;   ///< the index mask
    size_t write_ = 0;   ///< the write position
    float timeL_ = 1000.0f;   ///< the left delay time now, samples
    float timeR_ = 1000.0f;   ///< the right delay time now, samples
    float targetL_ = 1000.0f;   ///< where the left one glides to
    float targetR_ = 1000.0f;   ///< where the right one glides to
    float glide_ = 0.001f;   ///< the glide coefficient
    float feedback_ = 0.4f;   ///< feedback
    bool primed_ = false;   ///< the times have been set once (the first set() jumps, later ones glide)
    Svf hpL_;   ///< the feedback path's high pass, left
    Svf hpR_;   ///< ... right
    Svf lpL_;   ///< the feedback path's low pass, left
    Svf lpR_;   ///< ... right
};

} // namespace phos
