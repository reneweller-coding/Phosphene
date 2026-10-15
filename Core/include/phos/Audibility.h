/**
 * @file Audibility.h
 * @brief How much of each part is heard in the mix (23.09.2026, round "Hörbarkeit"): partial loudness per
 *        stem against the sum of all the others.
 *
 * The level match and the presence match measure how *loud* a part is. The complaints they did not catch
 * were about masking: the counter-lead playing at a sensible level and still not heard, the shamanic bed
 * placed correctly and 25 dB under the mix. What a listener hears of a part inside a mix is its *partial
 * loudness* -- the loudness it adds on top of everything else -- and that is what this meter estimates.
 *
 * **The model** is a simplified stationary version of Moore, Glasberg & Baer (1997, "A model for the
 * prediction of thresholds, loudness and partial loudness", JAES 45(4)), frame by frame:
 *  1. 2048-sample Hann frames, hop 1024; power spectrum per channel, the two channels' powers averaged.
 *     Levels are referred to a full-scale sine at 100 dB SPL (a loud monitoring level; the thresholds are the
 *     only place the absolute level enters).
 *  2. Outer and middle ear: the frequency weighting of ITU-R BS.1387 (PEAQ),
 *     W(f) = -0.6 * 3.64 f^-0.8 + 6.5 exp(-0.6 (f - 3.3)^2) - 1e-3 f^3.6 dB, f in kHz.
 *  3. Bands half an ERB wide on the ERB-number scale (Glasberg & Moore 1990, Cam = 21.4 log10(4.37 f + 1)),
 *     50 Hz to 15 kHz; the excitation is the band intensity spread over the other bands with Terhardt's
 *     level-dependent slopes (1979, as used by ITU-R BS.1387): 27 dB per Bark below a source, 24 + 230 / f -
 *     0.2 L dB per Bark above it, so that a loud source masks further upwards. (The spreading in Bark rather
 *     than with MGB's roex filters is the simplification.)
 *  4. Specific loudness and partial specific loudness by Moore, Glasberg & Baer's equations (3) to (6), with
 *     the compressive exponent 0.2, A = 2 E_THRQ, K = 0.5 (their value above 500 Hz) and the cochlear gain G = 1
 *     (the simplification: G departs from 1 only below 500 Hz). The masker of a stem is the excitation of all
 *     the other stems (powers added, the parts taken as incoherent). Above its masked threshold a part keeps
 *     the loudness of part and masker together less a share of the masker's that shrinks as the part rises
 *     above it; below the threshold its loudness falls steeply to zero. Summed over the bands and scaled so
 *     that a 1 kHz sine at 40 dB SPL, run through the same path, is 1: sone-like units.
 *
 * The readings average over the frames in which the part alone is at least a quarter unit loud -- the frames in which it
 * plays, not its release tails. `ratio` is the partial loudness over the loudness alone, frames summed first: 1 means the part is heard in
 * the mix as loud as on its own, 0.3 that seven tenths of it disappear under the rest. It is a *relative*
 * measure and meant to be compared between parts, sections and versions of the program, not read as an
 * absolute sone value; the model's constants are the published ones where it has them and stated where it
 * simplifies.
 * @author Rene Weller
 */
#pragma once

#include <complex>
#include <memory>
#include <vector>

namespace phos
{

class Fft;

/** @brief What the meter says about one stem. */
struct AudibilityReading
{
    double m_alone = 0.0;   ///< mean loudness of the stem on its own (sone-like)
    double m_inMix = 0.0;   ///< mean partial loudness in the mix of all stems
    double m_ratio = 0.0;   ///< inMix / alone, 0 when the stem was silent
    int    m_frames =
        0;   ///< frames in which the stem alone was heard (at least a quarter unit; release tails do not count)
};

/** @brief Partial loudness of each of a set of stems against the sum of the others. */
class AudibilityMeter
{
public:
    /**
     * @param[in] stems       how many stems every add() call carries
     * @param[in] sampleRate  their rate
     */
    AudibilityMeter( int stems, double sampleRate );
    ~AudibilityMeter();
    AudibilityMeter( const AudibilityMeter & ) = delete;
    AudibilityMeter &operator=( const AudibilityMeter & ) = delete;

    /**
     * @brief Appends @p n samples of every stem; @p L[k] and @p R[k] are stem k's channels (R may be null for
     *        mono). Stems whose pointer is null count as silent.
     * @param[in] L  the stems' left channels, null for a silent stem
     * @param[in] R  the stems' right channels, null for mono
     * @param[in] n  the samples
     */
    void add( const float *const *L, const float *const *R, int n );
    /**
     * @brief The reading of stem @p k over everything added since the last reset().
     * @param[in] k  the stem
     * @return its reading
     */
    AudibilityReading read( int k ) const;
    /** @brief Forgets the readings (and the partial frame). */
    void reset();

private:
    /** @brief Analyses the completed frame of every stem and adds it to the readings. */
    void frame();
    /**
     * @brief Adds the band intensities of one channel's frame to @p out (half weight: two channels are averaged).
     * @param[in]     buf  the channel's frame
     * @param[in,out] out  the band intensities, added to
     * @param[in,out] x    scratch for the transform
     */
    void bandIntensity( const std::vector<float> &buf, std::vector<double> &out,
                        std::vector<std::complex<double>> &x ) const;
    /**
     * @brief Spreads band intensities into an excitation pattern; false when everything was silent.
     * @param[in]  in   the band intensities
     * @param[out] out  the excitation pattern
     * @return false when everything was silent
     */
    bool excite( const std::vector<double> &in, std::vector<double> &out ) const;
    /**
     * @brief Loudness of excitation @p es alone and in the masker @p em (null: in quiet), unscaled.
     * @param[in]  es     the excitation
     * @param[in]  em     the masker's excitation, or null for quiet
     * @param[out] alone  the loudness alone
     * @param[out] inMix  the loudness in the masker
     */
    void loudness( const std::vector<double> &es, const std::vector<double> *em, double &alone, double &inMix ) const;
    int                             m_stems;         ///< stems per add()
    double                          m_sr;            ///< their sample rate
    std::unique_ptr<Fft>            m_fft;           ///< one frame's transform
    std::vector<double>             m_window;        ///< the analysis window
    std::vector<int>                m_bandOfBin;     ///< -1 for bins outside 50 Hz .. 15 kHz
    std::vector<double>             m_binGain;       ///< ear weighting and level reference per bin
    std::vector<double>             m_spread;        ///< bands x bands, Bark distance of receiver above source
    std::vector<double>             m_centre;        ///< centre frequency of each band, Hz
    std::vector<double>             m_threshold;     ///< threshold excitation per band
    int                             m_bands = 0;     ///< critical bands in use
    double                          m_scale = 1.0;   ///< makes a 1 kHz tone at 40 dB SPL one unit
    std::vector<std::vector<float>> m_bufL;          ///< the current frame of each stem, left
    std::vector<std::vector<float>> m_bufR;          ///< ... right
    int                             m_fill = 0;      ///< samples in the current frame
    std::vector<double>             m_sumAlone;      ///< per stem: the loudness alone, summed over frames
    std::vector<double>             m_sumMix;        ///< ... and its partial loudness in the mix
    std::vector<int>                m_frames;        ///< per stem: the frames it sounded in
};

}   // namespace phos
