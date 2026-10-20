/**
 * @file Resonance.h
 * @brief The master's resonance suppressor: narrow peaks that stand out of the mix's spectrum are taken down while
 *        they stand out, without latency.
 *
 * A dense mix gathers resonances no single part has: three layers that each ring a little at 3 kHz ring a lot
 * together, a filter's peak meets a hat's, a room's mode sits under a chord. The tools that find them in a studio
 * (soothe, Gullfoss and their kin) compare the short spectrum with a smoothed version of itself and cut where it
 * stands out; they do it on the spectrum itself and pay for it with latency. This one cannot -- a live instrument,
 * a jam -- so it splits the job: the **analysis** looks at what has just played, the **cut** is a handful of moving
 * bell filters on what plays now.
 *
 * **Analysis**, every Hop samples: the last Fft samples of the mid signal under a Hann window, the power spectrum in
 * dB, smoothed over time per bin (attack 30 ms, release 250 ms: a peak must last to count, and a cut does not flicker
 * with every hit). Its **envelope** is the mean of those dB values over a third of an octave on either side of each
 * bin -- the mean of the decibels, not of the power, so that a peak hardly lifts its own reference. What lies more
 * than the threshold over the envelope is a candidate; the largest candidates, a quarter octave apart, become the
 * bells' targets, each with a depth of a share of its excess (at most MaxCut), its frequency from a parabola through
 * the peak bin and its neighbours, its Q from the peak's width 3 dB down.
 *
 * Where it looks, and how hard: 200 Hz to 14 kHz, and hardest between 1.5 and 8 kHz -- the region the ear is most
 * sensitive in and where a mix turns harsh (ISO 226 equal-loudness contours) -- falling to 40 % at both ends. Under
 * 200 Hz a bass's harmonics are peaks by nature and the analysis (23 Hz per bin) cannot tell them from a resonance.
 *
 * **The bells** are Simper's trapezoidal state-variable bells (Cytomic, "Solving the continuous SVF equations using
 * trapezoidal integration and equivalent currents", 2013), stable and free of zipper noise when their coefficients
 * move every sample, which they do: between two analyses each bell's coefficients glide linearly, so the cut moves
 * smoothly whatever the host's block size -- the analysis runs on its own sample count, and the same input gives
 * the same output in a live block of 32 and in a render's block of 4096. A bell keeps following the peak it was
 * given (the nearest target within a sixth of an octave); a bell without one returns to 0 dB with the release.
 * Both channels go through the same bells, so the image does not move.
 *
 * Amount 0 is a true bypass (the bells are not run once they are back at 0 dB). At 0.5 the threshold is 9 dB and a
 * peak loses half its excess over it, at most 3 dB; at 1 the threshold is 6 dB, the share 0.8, at most 6 dB.
 * @note Copied from Totality `Core/include/tot/fx/Resonance.h` (20.10.2026); namespace phos.
 * @author Rene Weller
 */
#pragma once
#include <array>
#include <vector>

namespace phos
{

/** @brief Dynamic resonance suppression with moving bells: zero latency, stereo-linked. */
class ResonanceSuppressor
{
public:
    static constexpr int Bells = 6;   ///< the most resonances taken down at once

    /**
     * @brief Sets the rate and clears everything; the analysis grows with the rate (2048 points at 48 kHz).
     * @param[in] sampleRate  the sample rate in Hz
     */
    void prepare( double sampleRate );

    /** @brief Clears the history and returns every bell to 0 dB at once. */
    void reset();

    /**
     * @brief How hard it works.
     * @param[in] amount  0 (off) .. 1 (strong)
     */
    void set( float amount );

    /**
     * @brief Processes @p n samples in place.
     * @param[in,out] L  the left channel
     * @param[in,out] R  the right channel
     * @param[in]     n  the number of samples
     */
    void process( float *L, float *R, int n );

    /** @return the deepest cut of the moment in dB (0 or less), for a meter */
    float reduction() const;

    /**
     * @brief The radix-2 transform the analysis runs on, in place (also the Leveler's tone reading).
     * @param[in,out] re  the real parts (a power of two of them)
     * @param[in,out] im  the imaginary parts
     */
    static void transform( std::vector<float> &re, std::vector<float> &im );

private:
    /** @brief One bell: where it is, where it goes, and its two channels' states. */
    struct Bell
    {
        float m_hz = 1000.0f;      ///< its frequency
        float m_q = 4.0f;          ///< its Q
        float m_db = 0.0f;         ///< its gain now (0 or less)
        float m_aimDb = 0.0f;      ///< the gain the last analysis asked for
        float m_aimHz = 1000.0f;   ///< the frequency it asked for
        float m_aimQ = 4.0f;       ///< the Q it asked for
        float m_g0 = 0.0f;         ///< the coefficient g at the last analysis
        float m_k0 = 1.0f;         ///< the coefficient k at the last analysis
        float m_m0 = 0.0f;         ///< the band's share at the last analysis (0: the bell is flat)
        float m_g1 = 0.0f;         ///< g at the next
        float m_k1 = 1.0f;         ///< k at the next
        float m_m1 = 0.0f;         ///< the band's share at the next
        float m_ic1[2] = {};       ///< the first integrator per channel
        float m_ic2[2] = {};       ///< the second integrator per channel
    };

    /** @brief One analysis: the spectrum, its envelope, the peaks over it, the bells' new aims and coefficients. */
    void analyse();
    /**
     * @brief The coefficients of bell @p b as it stands.
     * @param[in]  b  the bell
     * @param[out] g  receives tan(pi f / fs)
     * @param[out] k  receives 1 / (Q A)
     * @param[out] m  receives the band's share, k (A^2 - 1)
     */
    void coefficients( const Bell &b, float &g, float &k, float &m ) const;

    double                  m_rate = 48000.0;      ///< the sample rate
    int                     m_fft = 2048;          ///< the analysis' length
    int                     m_hop = 256;           ///< samples between two analyses
    int                     m_until = 256;         ///< samples until the next analysis
    int                     m_step = 0;            ///< samples since the last analysis
    int                     m_write = 0;           ///< the history's write position
    float                   m_amount = 0.0f;       ///< how hard it works (0 .. 1)
    bool                    m_active = false;      ///< a bell is away from 0 dB (or the amount is up)
    std::vector<float>      m_history;             ///< the last m_fft samples of the mid signal (a ring)
    std::vector<float>      m_window;              ///< the Hann window
    std::vector<float>      m_re;                  ///< the transform's real parts
    std::vector<float>      m_im;                  ///< the transform's imaginary parts
    std::vector<float>      m_smooth;              ///< the time-smoothed spectrum, dB per bin
    std::vector<float>      m_prefix;              ///< its running sum for the envelope
    std::vector<int>        m_lo;                  ///< each bin's envelope span, its first bin
    std::vector<int>        m_hi;                  ///< and its last
    std::vector<float>      m_weight;              ///< how hard each bin may be cut (0: not looked at)
    std::array<Bell, Bells> m_bells{};             ///< the bells
    float                   m_attack = 0.0f;       ///< the spectrum's smoothing per hop, rising
    float                   m_release = 0.0f;      ///< and falling
    float                   m_cutAttack = 0.0f;    ///< the bells' gain per hop, deepening
    float                   m_cutRelease = 0.0f;   ///< and returning
};

}   // namespace phos
