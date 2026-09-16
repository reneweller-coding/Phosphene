/**
 * @file Dynamics.h
 * @brief The master's dynamics: a log-domain bus compressor, a 4x interpolated true-peak estimate and a
 *        lookahead true-peak limiter.
 *
 * **Bus compressor** after Giannoulis, Massberg and Reiss, "Digital dynamic range compressor design --
 * a tutorial and analysis" (JAES 60(6), 2012): feed-forward, the gain computer in the log domain with a
 * quadratic soft knee (their eq. 4), and the smooth decoupled peak detector placed after the gain
 * computer (eq. 17 on the gain reduction), which they recommend for its artefact-free release and
 * because attack and release then act on decibels, not on the waveform. The two channels share one
 * detector (the larger of the two), so the stereo image does not move.
 *
 * **True peak.** A signal's peak between the samples can lie several decibels above its largest sample
 * (a sine at a quarter of the sampling rate sampled at 45 degrees: 3 dB). The estimate interpolates
 * three points between every pair of samples with a Kaiser-windowed sinc of twelve taps per phase,
 * designed at start-up for the sampling rate -- the method of ITU-R BS.1770-4 Annex 2, with a filter
 * of our own rather than the standard's table. Measured in the self test on sines up to 0.4 of the
 * sampling rate at every phase.
 *
 * **Limiter.** For every sample the gain that keeps its true peak at the ceiling is computed; a
 * sliding minimum over the lookahead window followed by a moving average of the same length gives a
 * gain that has fully arrived when the peak does -- each average contains the peak's own required
 * gain, and every value in it is at most that -- and moves smoothly, without overshoot; the recovery
 * afterwards is exponential. The audio is delayed by the window (plus the interpolator's half length),
 * which the engine reports as its latency.
 */
#pragma once
#include "phos/Dsp.h"
#include <vector>

namespace phos {

/** @brief Feed-forward log-domain compressor, stereo-linked. */
class BusCompressor {
public:
    /** @brief Sets the sample rate and clears the detector. */
    void prepare(double sampleRate) { sr_ = sampleRate; reset(); }
    /** @brief Clears the detector. */
    void reset() { y1_ = 0.0; yL_ = 0.0; }
    /**
     * @brief Settings.
     * @param thresholdDb threshold T
     * @param ratio       R (1 = off)
     * @param kneeDb      knee width W
     * @param attackMs,releaseMs detector time constants
     */
    void set(float thresholdDb, float ratio, float kneeDb, float attackMs, float releaseMs)
    {
        T_ = thresholdDb;
        R_ = std::max(1.0f, ratio);
        W_ = std::max(0.0f, kneeDb);
        aA_ = std::exp(-1.0 / (std::max(0.01f, attackMs) * 0.001 * sr_));
        aR_ = std::exp(-1.0 / (std::max(0.01f, releaseMs) * 0.001 * sr_));
    }
    /** @brief The static curve: output level for an input level, both in dB (Giannoulis eq. 4). */
    double curve(double x) const
    {
        const double d = x - T_;
        if (2.0 * d < -W_) return x;
        if (W_ > 0.0f && 2.0 * std::fabs(d) <= W_) return x + (1.0 / R_ - 1.0) * (d + W_ / 2.0) * (d + W_ / 2.0) / (2.0 * W_);
        return T_ + d / R_;
    }
    /** @brief Processes a stereo block in place. */
    void process(float* L, float* R, int n)
    {
        for (int i = 0; i < n; ++i) {
            const double peak = std::max(std::fabs(static_cast<double>(L[i])), std::fabs(static_cast<double>(R[i])));
            const double xG = peak > 1.0e-6 ? 20.0 * std::log10(peak) : -120.0;
            const double xL = xG - curve(xG);   // gain reduction the static curve asks for, >= 0
            y1_ = std::max(xL, aR_ * y1_ + (1.0 - aR_) * xL);
            yL_ = aA_ * yL_ + (1.0 - aA_) * y1_;
            const float g = static_cast<float>(std::pow(10.0, -yL_ / 20.0));
            L[i] *= g;
            R[i] *= g;
            reduction_ = static_cast<float>(yL_);
        }
    }
    /** @brief Gain reduction of the last sample, dB. */
    float reduction() const { return reduction_; }

private:
    double sr_ = 48000.0, aA_ = 0.99, aR_ = 0.999, y1_ = 0.0, yL_ = 0.0;
    float T_ = -12.0f, R_ = 2.0f, W_ = 6.0f, reduction_ = 0.0f;
};

/** @brief 4x interpolation for true-peak estimates: three phases of twelve taps each. */
class TruePeakInterpolator {
public:
    static constexpr int kTaps = 12;   ///< taps per phase
    static constexpr int kHalf = 6;    ///< samples of lookahead the filter needs
    /** @brief Designs the Kaiser-windowed sinc (beta 8, cutoff at 0.45 of the input rate). */
    TruePeakInterpolator();
    /**
     * @brief Largest magnitude of the three interpolated points between x[0] and x[1].
     * @param x pointer to the earlier sample; x[-5] .. x[6] must be readable
     */
    double between(const float* x) const
    {
        double peak = 0.0;
        for (int k = 0; k < 3; ++k) {
            double s = 0.0;
            for (int m = 0; m < kTaps; ++m) s += h_[k][m] * static_cast<double>(x[m - kHalf + 1]);
            peak = std::max(peak, std::fabs(s));
        }
        return peak;
    }
private:
    double h_[3][kTaps] = {};
};

/** @brief Stereo lookahead true-peak limiter. */
class TruePeakLimiter {
public:
    /** @brief Allocates for a sample rate; @p lookaheadMs sets the window. */
    void prepare(double sampleRate, float lookaheadMs = 1.5f);
    /** @brief Clears the delay lines and the gain. */
    void reset();
    /** @brief Ceiling in dBTP and recovery time. */
    void set(float ceilingDb, float releaseMs);
    /** @brief Processes in place. */
    void process(float* L, float* R, int n);
    /** @brief Samples of delay the limiter adds. */
    int latency() const { return window_ - 1 + TruePeakInterpolator::kHalf; }
    /** @brief Gain reduction of the last sample, dB (>= 0). */
    float reduction() const { return reduction_; }

private:
    TruePeakInterpolator interp_;
    double sr_ = 48000.0;
    int window_ = 72;
    float ceiling_ = 0.891f;
    double release_ = 0.999;
    // Input history for the interpolator (both channels), in a ring of 2 * kTaps.
    std::vector<float> histL_, histR_;
    int histPos_ = 0;
    double prevBetween_ = 0.0;
    // Required gains, the sliding-minimum deque and the moving average.
    std::vector<double> req_;
    std::vector<int> dq_;
    int dqHead_ = 0, dqTail_ = 0;
    std::vector<double> minRing_;
    double minSum_ = 0.0;
    long long t_ = 0;
    int sinceRecompute_ = 0;
    double gain_ = 1.0;
    // The audio delay.
    std::vector<float> delayL_, delayR_;
    int delayPos_ = 0;
    float reduction_ = 0.0f;
};

} // namespace phos
