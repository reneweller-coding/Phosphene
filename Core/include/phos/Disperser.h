/**
 * @file Disperser.h
 * @brief All-pass dispersion: the "pew"/"zapp" colour of modern psytrance leads, as pure group delay.
 *
 * **What it is.** The wet, laser-like attack of a modern psytrance lead or acid stab is not a filter
 * sweep and not a resonance -- it is *dispersion*: the low part of a transient leaves the instrument
 * a few milliseconds after its high part, so a click turns into a short downward chirp. The idiom is
 * the one Kilohearts Disperser made common; the mechanism is the classical one of an all-pass chain,
 * described for audio by Zoelzer ("DAFX -- Digital Audio Effects", 2nd ed. 2011, chapter 2, the
 * second-order all-pass and its group delay) and used for dispersive synthesis by Rocchesso and
 * Smith ("Circulant and elliptic feedback delay networks for conference reverberation", IEEE Trans.
 * Speech and Audio Processing 5(1), 1997) and by Bank ("Physics-based sound synthesis of string
 * instruments including geometric nonlinearities", 2006) for the stiff string, where a chain of
 * all-passes reproduces the frequency-dependent wave speed of a real string.
 *
 * **Why an all-pass and not a filter.** The magnitude response of every section is exactly 1 at
 * every frequency: an all-pass moves energy in *time*, never between bands. That is what makes the
 * effect safe under Phosphene's depth rule -- it cannot put power under 140 Hz that was not there,
 * because it cannot change the power of any band at all. The self test measures that (the deviation
 * from 0 dB over 50 Hz to 20 kHz), and it measures the group delay against the design.
 *
 * **The design.** @p stages second-order all-pass sections with centre frequencies spaced
 * logarithmically over a decade around @p centreHz, each with Q = kDisperseQ:
 * @code
 *   H(z) = (c + d z^-1 + z^-2) / (1 + d z^-1 + c z^-2)
 *   alpha = sin(w0) / (2 Q),  c = (1 - alpha) / (1 + alpha),  d = -2 cos(w0) / (1 + alpha)
 * @endcode
 * (the all-pass of Bristow-Johnson's "Cookbook formulae for audio EQ biquad coefficients", with
 * numerator and denominator normalised by a0, which is where the mirror-image symmetry that makes
 * |H| = 1 becomes visible). Each section's group delay peaks at its own centre and a second-order
 * section carries a delay proportional to 1/f0, so the chain's total group delay is largest at the
 * bottom of the band and falls away above it: the lows lag the highs, which is the chirp.
 *
 * Q = 1 rather than the flatter 0.7 for one reason that matters here: at Q = 1 the maximum of the
 * chain's group delay sits *inside* the band (measured at 48 kHz, eight sections over 400 Hz to
 * 4 kHz: 4.39 ms at 400 Hz against 2.96 ms at 140 Hz), while at Q = 0.7 it keeps rising towards DC
 * (4.07 ms at 400 Hz, 3.95 ms at 140 Hz) and at Q = 0.4 the sub-band gets the most delay of all
 * (5.63 ms at 140 Hz). Putting the maximum in the band is the point of choosing a second-order
 * section over a first-order one, whose group delay always peaks at DC.
 *
 * **Cost of the colour, which is not free.** Dispersion smears a transient, so the crest factor of
 * the signal falls and with it its peak-to-loudness ratio: the same RMS now reaches a lower peak, and
 * a chain calibrated on peaks would let the instrument through louder. Both are measured in the self
 * test (section "acid colour") rather than assumed.
 *
 * The chain is scalar and runs per channel, outside every lane kernel, so it takes no part in the
 * bit-identity of the vector paths. With @p stages 0 it is a bypass that copies its input.
 */
#pragma once
#include <cmath>

namespace phos {

/** @brief Sections the disperser can run. */
constexpr int kDisperseStages = 8;
/** @brief Q of every section (see the file comment for why 1 and not 0.7). */
constexpr double kDisperseQ = 1.0;
/** @brief Half-width of the band in octaves: the sections span centreHz / 2^k to centreHz * 2^k. */
constexpr double kDisperseHalfSpan = 1.6609640474436813;   // log2(sqrt(10)): a decade

/**
 * @brief A chain of up to kDisperseStages second-order all-pass sections, one channel.
 *
 * Direct form I per section: the coefficients are shared between the channels of a stereo pair, the
 * state is not, so a Disperser instance is one channel and a stereo effect holds two of them and one
 * coefficient set.
 */
struct DisperserChannel {
    float x1[kDisperseStages] = {}, x2[kDisperseStages] = {};   ///< input history per section
    float y1[kDisperseStages] = {}, y2[kDisperseStages] = {};   ///< output history per section

    /** @brief Clears the state of every section. */
    void reset()
    {
        for (int i = 0; i < kDisperseStages; ++i) { x1[i] = x2[i] = 0.0f; y1[i] = y2[i] = 0.0f; }
    }

    /**
     * @brief One sample through @p stages sections.
     * @param x      input sample
     * @param c,d    per-section coefficients from Disperser::set()
     * @param stages sections to run (0 = bypass)
     */
    inline float tick(float x, const float* c, const float* d, int stages)
    {
        for (int i = 0; i < stages; ++i) {
            const float y = c[i] * x + d[i] * x1[i] + x2[i] - d[i] * y1[i] - c[i] * y2[i];
            x2[i] = x1[i];
            x1[i] = x;
            y2[i] = y1[i];
            y1[i] = y;
            x = y;
        }
        return x;
    }
};

/** @brief The coefficients of a disperser chain, computed once per update(). */
struct Disperser {
    float c[kDisperseStages] = {}, d[kDisperseStages] = {};   ///< each all-pass section's two coefficients
    int stages = 0;   ///< sections in use

    /**
     * @brief Places @p n sections logarithmically over a decade around @p centreHz.
     * @param n         sections, clamped to 0 .. kDisperseStages
     * @param centreHz  geometric centre of the band
     * @param sr        sample rate
     *
     * The lowest section sits at centreHz / sqrt(10) and the highest at centreHz * sqrt(10); with one
     * section it sits at the centre. Every centre is kept under 0.45 fs so that no section degenerates
     * at the top of the band.
     */
    void set(int n, double centreHz, double sr)
    {
        stages = n < 0 ? 0 : (n > kDisperseStages ? kDisperseStages : n);
        const double lo = centreHz * std::pow(2.0, -kDisperseHalfSpan);
        const double hi = centreHz * std::pow(2.0, kDisperseHalfSpan);
        for (int i = 0; i < stages; ++i) {
            const double t = stages > 1 ? static_cast<double>(i) / (stages - 1) : 0.5;
            double f0 = lo * std::pow(hi / lo, t);
            f0 = f0 < 20.0 ? 20.0 : (f0 > 0.45 * sr ? 0.45 * sr : f0);
            const double w0 = 2.0 * 3.141592653589793 * f0 / sr;
            const double alpha = std::sin(w0) / (2.0 * kDisperseQ);
            c[i] = static_cast<float>((1.0 - alpha) / (1.0 + alpha));
            d[i] = static_cast<float>(-2.0 * std::cos(w0) / (1.0 + alpha));
        }
    }
};

} // namespace phos
