/**
 * @file Loudness.h
 * @brief Loudness to ITU-R BS.1770-4 / EBU R 128 and the true peak between the samples.
 *
 * Psytrance is mastered loud -- integrated loudness around -8 to -6 LUFS is common -- and a set
 * that jumps several LU between tracks is broken. The meter runs on the finished master output and
 * reports momentary, short-term and gated integrated loudness, the loudness range, the true peak
 * and the crest factor.
 *
 * @note Copied from Noctuary `Core/include/ambient/Loudness.h` at b60a2fe (15.09.2026) without the
 *       Zwicker sone model (and therefore without its FFT). Comments in Doxygen form.
 */
#pragma once
#include "phos/Dynamics.h"
#include <atomic>
#include <cstddef>
#include <vector>

namespace phos {

/** @brief One reading of the meter. */
struct LoudnessReading {
    float momentary  = -120.0f;   ///< LUFS over the last 400 ms
    float shortTerm  = -120.0f;   ///< LUFS over the last 3 s
    float integrated = -120.0f;   ///< LUFS, gated (absolute -70, relative -10), since reset
    float range      = 0.0f;      ///< LU: 10th to 95th centile of the short-term values
    float truePeak   = -120.0f;   ///< dBTP, 4x interpolated, since reset
    float crest      = 0.0f;      ///< dB: true peak over short-term loudness
    float seconds    = 0.0f;      ///< time measured
};

/**
 * @brief K-weighting: the high shelf and the RLB high-pass of BS.1770.
 *
 * Derived from the analogue prototypes the standard names, so the coefficients are right at any
 * sample rate; at 48 kHz they reproduce the published table to sixteen digits.
 */
class KFilter {
public:
    /** @brief Computes the coefficients for @p sampleRate. */
    void prepare(double sampleRate);
    /** @brief Clears the states. */
    void reset();
    /** @brief One sample. */
    float process(float x);
private:
    struct Biquad {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;
        float process(float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() { z1 = z2 = 0.0f; }
    };
    Biquad shelf_, hp_;
};

/**
 * @brief A fixed-capacity ring of one value per frame: written on the audio thread, read elsewhere.
 *
 * It never grows, so the audio thread never allocates; what is kept is a window of the recent past.
 */
struct LoudnessLog {
    /** @brief Allocates the ring (not on the audio thread). */
    void prepare(size_t capacity) { buf_.assign(capacity, 0.0f); pos_.store(0); count_.store(0); }
    /** @brief Forgets the contents. */
    void clear() { pos_.store(0); count_.store(0); }
    /** @brief Appends a value; never allocates. */
    void push(float v)
    {
        if (buf_.empty()) return;
        const size_t p = pos_.load(std::memory_order_relaxed);
        buf_[p] = v;
        pos_.store((p + 1) % buf_.size(), std::memory_order_release);
        const size_t c = count_.load(std::memory_order_relaxed);
        if (c < buf_.size()) count_.store(c + 1, std::memory_order_release);
    }
    size_t size() const { return count_.load(std::memory_order_acquire); }   ///< values held
    bool  empty() const { return size() == 0; }                             ///< nothing held
    /** @brief The window in order, oldest first. */
    std::vector<float> snapshot() const
    {
        const size_t n = count_.load(std::memory_order_acquire);
        const size_t p = pos_.load(std::memory_order_acquire);
        std::vector<float> out;
        out.reserve(n);
        const size_t from = (n < buf_.size()) ? 0 : p;
        for (size_t k = 0; k < n; ++k) out.push_back(buf_[(from + k) % buf_.size()]);
        return out;
    }
private:
    std::vector<float> buf_;
    std::atomic<size_t> pos_{ 0 }, count_{ 0 };
};

/** @brief The BS.1770 meter with gating, range and true peak. */
class LoudnessMeter {
public:
    /** @brief Allocates and computes filters for @p sampleRate. */
    void prepare(double sampleRate);
    /** @brief Starts a new measurement. */
    void reset();
    /** @brief Feeds the finished stereo output. */
    void process(const float* L, const float* R, int n);
    /** @brief The current figures. */
    LoudnessReading read() const;
private:
    void pushBlock();

    double sr_ = 48000.0;
    KFilter kL_, kR_;
    int    blockLen_ = 19200, hopLen_ = 4800, hopPos_ = 0;
    std::vector<double> sumL_, sumR_;
    int    ringPos_ = 0, ringFilled_ = 0;
    static constexpr int kHopsPerBlock = 4;       ///< 400 ms blocks with 75 % overlap
    static constexpr int kHopsPerShort = 30;      ///< 3 s
    double hopL_ = 0.0, hopR_ = 0.0;
    long   hopSamples_ = 0;
    static constexpr size_t kBlockLog = 1u << 17; ///< about 3.6 hours of blocks at ten a second
    LoudnessLog blocks_;
    LoudnessLog shortBlocks_;
    double truePeak_ = 0.0;
    double seconds_ = 0.0;
    float  lastShort_ = -120.0f;
    float  tpHistL_[12] = {}, tpHistR_[12] = {};   ///< the last twelve samples, for the true-peak interpolator
    TruePeakInterpolator tpInterp_;
};

} // namespace phos
