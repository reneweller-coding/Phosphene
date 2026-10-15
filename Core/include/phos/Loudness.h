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
    float m_momentary  = -120.0f;   ///< LUFS over the last 400 ms
    float m_shortTerm  = -120.0f;   ///< LUFS over the last 3 s
    float m_integrated = -120.0f;   ///< LUFS, gated (absolute -70, relative -10), since reset
    float m_range      = 0.0f;      ///< LU: 10th to 95th centile of the short-term values
    float m_truePeak   = -120.0f;   ///< dBTP, 8x interpolated (TruePeakInterpolator), since reset
    float m_crest      = 0.0f;      ///< dB: true peak over short-term loudness
    float m_seconds    = 0.0f;      ///< time measured
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
    /** @brief One second-order section of the K-weighting filter. */
    struct Biquad {
        float m_b0 = 1;   ///< the coefficient of x[n]
        float m_b1 = 0;   ///< ... of x[n-1]
        float m_b2 = 0;   ///< ... of x[n-2]
        float m_a1 = 0;   ///< ... of y[n-1]
        float m_a2 = 0;   ///< ... of y[n-2]
        float m_z1 = 0;   ///< the first state
        float m_z2 = 0;   ///< the second state
        /** @brief One sample @p x through the section. */
        float process(float x)
        {
            const float y = m_b0 * x + m_z1;
            m_z1 = m_b1 * x - m_a1 * y + m_z2;
            m_z2 = m_b2 * x - m_a2 * y;
            return y;
        }
        /** @brief Clears the state. */
        void reset() { m_z1 = m_z2 = 0.0f; }
    };
    Biquad m_shelf;   ///< the K-weighting's high shelf
    Biquad m_hp;   ///< the K-weighting's high pass
};

/**
 * @brief A fixed-capacity ring of one value per frame: written on the audio thread, read elsewhere.
 *
 * It never grows, so the audio thread never allocates; what is kept is a window of the recent past.
 */
struct LoudnessLog {
    /** @brief Allocates the ring (not on the audio thread). */
    void prepare(size_t capacity) { m_buf.assign(capacity, 0.0f); m_pos.store(0); m_count.store(0); }
    /** @brief Forgets the contents. */
    void clear() { m_pos.store(0); m_count.store(0); }
    /** @brief Appends a value; never allocates. */
    void push(float v)
    {
        if (m_buf.empty()) return;
        const size_t p = m_pos.load(std::memory_order_relaxed);
        m_buf[p] = v;
        m_pos.store((p + 1) % m_buf.size(), std::memory_order_release);
        const size_t c = m_count.load(std::memory_order_relaxed);
        if (c < m_buf.size()) m_count.store(c + 1, std::memory_order_release);
    }
    size_t size() const { return m_count.load(std::memory_order_acquire); }   ///< values held
    bool  empty() const { return size() == 0; }                             ///< nothing held
    /** @brief The window in order, oldest first. */
    std::vector<float> snapshot() const
    {
        const size_t n = m_count.load(std::memory_order_acquire);
        const size_t p = m_pos.load(std::memory_order_acquire);
        std::vector<float> out;
        out.reserve(n);
        const size_t from = (n < m_buf.size()) ? 0 : p;
        for (size_t k = 0; k < n; ++k) out.push_back(m_buf[(from + k) % m_buf.size()]);
        return out;
    }
private:
    std::vector<float> m_buf;   ///< the ring of values
    std::atomic<size_t> m_pos{ 0 };   ///< the write position
    std::atomic<size_t> m_count{ 0 };   ///< how many were written
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
    /** @brief Closes a 100 ms hop: stores its mean square and updates the gated readings. */
    void pushBlock();

    double m_sr = 48000.0;   ///< sample rate
    KFilter m_kL;   ///< the K-weighting, left
    KFilter m_kR;   ///< ... right
    int    m_blockLen = 19200;   ///< the 400 ms block, samples
    int    m_hopLen = 4800;   ///< the 100 ms hop, samples
    int    m_hopPos = 0;   ///< samples into the hop
    std::vector<double> m_sumL;   ///< the last HopsPerShort hops' mean squares, left
    std::vector<double> m_sumR;   ///< ... right
    int    m_ringPos = 0;   ///< their ring's position
    int    m_ringFilled = 0;   ///< ... and fill
    static constexpr int HopsPerBlock = 4;       ///< 400 ms blocks with 75 % overlap
    static constexpr int HopsPerShort = 30;      ///< 3 s
    double m_hopL = 0.0;   ///< the running hop's sum of squares, left
    double m_hopR = 0.0;   ///< ... right
    long   m_hopSamples = 0;   ///< its samples so far
    static constexpr size_t BlockLog = 1u << 17; ///< about 3.6 hours of blocks at ten a second
    LoudnessLog m_blocks;   ///< every 400 ms block's loudness, for the gated integral
    LoudnessLog m_shortBlocks;   ///< every 3 s window's loudness, for the range
    double m_truePeak = 0.0;   ///< the highest true peak seen, linear
    double m_seconds = 0.0;   ///< seconds measured
    float  m_lastShort = -120.0f;   ///< the last short-term loudness, LUFS
    /// The last TruePeakInterpolator::History samples of each channel. Every sample is written into
    /// both halves of the buffer, so the window the interpolator reads is contiguous and the meter
    /// needs neither a shift nor a modulo per sample (the same arrangement as TruePeakLimiter).
    float  m_tpHistL[2 * TruePeakInterpolator::History] = {}, m_tpHistR[2 * TruePeakInterpolator::History] = {};   ///< ... right
    int    m_tpPos = 0;   ///< position in the true-peak history
    TruePeakInterpolator m_tpInterp;   ///< reads the peaks between the samples
};

} // namespace phos
