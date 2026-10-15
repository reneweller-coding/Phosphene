/**
 * @file Audibility.cpp
 * @brief Partial loudness of stems in a mix (Audibility.h has the model and its sources).
 * @author Rene Weller
 */
#include "phos/Audibility.h"
#include "phos/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace phos {

namespace {

constexpr int Frame = 2048;   ///< the analysis frame, samples
constexpr int Hop = 1024;   ///< the hop between frames, samples
constexpr double Alpha = 0.2;          ///< the compressive exponent of specific loudness
constexpr double FullScaleSpl = 100.0; ///< dB SPL of a full-scale sine
constexpr double CamStep = 0.5;        ///< band width in Cam
constexpr double LowerSlope = 27.0;    ///< dB per Bark on the low side of a source (Terhardt 1979)
constexpr double MinUpperSlope = 5.0;  ///< the upper slope never flattens below this (dB per Bark)
constexpr double AudibleFrame = 0.25;  ///< a frame counts when the part alone is this loud (units; 1 = 1 kHz at 40 dB SPL)

/**
 * @brief Critical-band rate in Bark (Traunmueller 1990).
 * @param[in] hz  the frequency
 * @return the critical-band rate in Bark
 */
double bark(double hz) { return 26.81 * hz / (1960.0 + hz) - 0.53; }

/**
 * @brief ERB-number (Cam) of @p hz (Glasberg & Moore 1990).
 * @param[in] hz  the frequency
 * @return the ERB-number
 */
double cam(double hz) { return 21.4 * std::log10(4.37e-3 * hz + 1.0); }

/**
 * @brief Outer and middle ear weighting of ITU-R BS.1387 (PEAQ), dB.
 * @param[in] hz  the frequency
 * @return the weighting in dB
 */
double earWeightDb(double hz)
{
    const double f = std::max(hz, 20.0) / 1000.0;
    return -0.6 * 3.64 * std::pow(f, -0.8) + 6.5 * std::exp(-0.6 * (f - 3.3) * (f - 3.3)) - 1e-3 * std::pow(f, 3.6);
}

/**
 * @brief The threshold's excitation after the ear weighting, dB SPL (the internal noise of PEAQ, raised by 3 dB).
 * @param[in] hz  the frequency
 * @return the threshold in dB SPL
 */
double thresholdDb(double hz)
{
    const double f = std::max(hz, 20.0) / 1000.0;
    return 3.0 + 0.4 * 3.64 * std::pow(f, -0.8);
}

} // namespace

AudibilityMeter::AudibilityMeter(int stems, double sampleRate) : m_stems(std::max(1, stems)), m_sr(sampleRate)
{
    m_fft = std::make_unique<Fft>(Frame);
    m_window.resize(Frame);
    double w2 = 0.0;
    for (int i = 0; i < Frame; ++i) {
        m_window[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / Frame);
        w2 += m_window[static_cast<size_t>(i)] * m_window[static_cast<size_t>(i)];
    }
    const double lo = cam(50.0), hi = cam(15000.0);
    m_bands = static_cast<int>(std::floor((hi - lo) / CamStep)) + 1;
    // One side of the spectrum: the bins in a band sum to the mean square of the windowed signal times 2 / (N sum w^2)
    // (Parseval), a full-scale sine's mean square is 1/2, and that is FullScaleSpl.
    const double toMeanSquare = 2.0 / (static_cast<double>(Frame) * w2);
    m_bandOfBin.assign(Frame / 2, -1);
    m_binGain.assign(Frame / 2, 0.0);
    for (int k = 1; k < Frame / 2; ++k) {
        const double hz = k * m_sr / Frame;
        if (hz < 50.0 || hz > 15000.0) continue;
        const int b = std::clamp(static_cast<int>(std::lround((cam(hz) - lo) / CamStep)), 0, m_bands - 1);
        m_bandOfBin[static_cast<size_t>(k)] = b;
        m_binGain[static_cast<size_t>(k)] = toMeanSquare / 0.5 * std::pow(10.0, (FullScaleSpl + earWeightDb(hz)) / 10.0);
    }
    m_spread.assign(static_cast<size_t>(m_bands) * static_cast<size_t>(m_bands), 0.0);
    m_threshold.assign(static_cast<size_t>(m_bands), 0.0);
    m_centre.assign(static_cast<size_t>(m_bands), 0.0);
    for (int r = 0; r < m_bands; ++r) {
        // Centre frequency of the band from its Cam value.
        const double c = lo + r * CamStep;
        const double hz = (std::pow(10.0, c / 21.4) - 1.0) / 4.37e-3;
        m_centre[static_cast<size_t>(r)] = hz;
        m_threshold[static_cast<size_t>(r)] = std::pow(10.0, thresholdDb(hz) / 10.0);
    }
    // The spreading works in Bark (the slopes are Terhardt's, stated per Bark): distance of every receiver from every
    // source, positive when the receiver lies above.
    for (int r = 0; r < m_bands; ++r)
        for (int s = 0; s < m_bands; ++s)
            m_spread[static_cast<size_t>(r) * static_cast<size_t>(m_bands) + static_cast<size_t>(s)] = bark(m_centre[static_cast<size_t>(r)]) - bark(m_centre[static_cast<size_t>(s)]);
    // Calibration through the path every frame takes: a 1 kHz sine at 40 dB SPL (-60 dBFS) in both channels.
    // (Placing its intensity in one band by hand read 1.38 in the real path: the window spreads a tone over
    // neighbouring bins, those can fall into two bands, and the compressive loudness of two halves is more than
    // that of the whole.)
    {
        m_scale = 1.0;
        std::vector<float> sine(Frame);
        const double amp = std::pow(10.0, (40.0 - FullScaleSpl) / 20.0);   // peak of a sine at 40 dB SPL (full scale = FullScaleSpl)
        for (int i = 0; i < Frame; ++i) sine[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / m_sr));
        std::vector<std::complex<double>> x(Frame);
        std::vector<double> in(static_cast<size_t>(m_bands), 0.0), e(static_cast<size_t>(m_bands), 0.0);
        bandIntensity(sine, in, x);
        bandIntensity(sine, in, x);
        double alone = 0.0, inMix = 0.0;
        if (excite(in, e)) loudness(e, nullptr, alone, inMix);
        m_scale = alone > 0.0 ? 1.0 / alone : 1.0;
    }
    m_bufL.assign(static_cast<size_t>(m_stems), std::vector<float>(Frame, 0.0f));
    m_bufR.assign(static_cast<size_t>(m_stems), std::vector<float>(Frame, 0.0f));
    reset();
}

AudibilityMeter::~AudibilityMeter() = default;

void AudibilityMeter::reset()
{
    m_fill = 0;
    for (auto& b : m_bufL) std::fill(b.begin(), b.end(), 0.0f);
    for (auto& b : m_bufR) std::fill(b.begin(), b.end(), 0.0f);
    m_sumAlone.assign(static_cast<size_t>(m_stems), 0.0);
    m_sumMix.assign(static_cast<size_t>(m_stems), 0.0);
    m_frames.assign(static_cast<size_t>(m_stems), 0);
}

void AudibilityMeter::add(const float* const* L, const float* const* R, int n)
{
    int done = 0;
    while (done < n) {
        const int take = std::min(n - done, Frame - m_fill);
        for (int k = 0; k < m_stems; ++k) {
            float* bl = m_bufL[static_cast<size_t>(k)].data() + m_fill;
            float* br = m_bufR[static_cast<size_t>(k)].data() + m_fill;
            const float* l = L[k];
            const float* r = R != nullptr ? R[k] : nullptr;
            for (int i = 0; i < take; ++i) {
                bl[i] = l != nullptr ? l[done + i] : 0.0f;
                br[i] = r != nullptr ? r[done + i] : bl[i];
            }
        }
        m_fill += take;
        done += take;
        if (m_fill == Frame) {
            frame();
            // Keep the second half: the next frame overlaps this one by Frame - Hop samples.
            for (int k = 0; k < m_stems; ++k) {
                std::copy(m_bufL[static_cast<size_t>(k)].begin() + Hop, m_bufL[static_cast<size_t>(k)].end(), m_bufL[static_cast<size_t>(k)].begin());
                std::copy(m_bufR[static_cast<size_t>(k)].begin() + Hop, m_bufR[static_cast<size_t>(k)].end(), m_bufR[static_cast<size_t>(k)].begin());
            }
            m_fill = Frame - Hop;
        }
    }
}

void AudibilityMeter::bandIntensity(const std::vector<float>& buf, std::vector<double>& out, std::vector<std::complex<double>>& x) const
{
    bool any = false;
    for (int i = 0; i < Frame; ++i) {
        x[static_cast<size_t>(i)] = std::complex<double>(buf[static_cast<size_t>(i)] * m_window[static_cast<size_t>(i)], 0.0);
        any = any || buf[static_cast<size_t>(i)] != 0.0f;
    }
    if (!any) return;
    m_fft->transform(x, false);
    for (int b = 1; b < Frame / 2; ++b) {
        const int band = m_bandOfBin[static_cast<size_t>(b)];
        if (band < 0) continue;
        out[static_cast<size_t>(band)] += 0.5 * std::norm(x[static_cast<size_t>(b)]) * m_binGain[static_cast<size_t>(b)];   // two channels averaged
    }
}

bool AudibilityMeter::excite(const std::vector<double>& in, std::vector<double>& out) const
{
    const size_t B = static_cast<size_t>(m_bands);
    bool any = false;
    for (double v : in) any = any || v > 0.0;
    if (!any) return false;
    // Level-dependent spreading (Terhardt 1979, as in ITU-R BS.1387): 27 dB per Bark below a source, and above it
    // 24 + 230 / f - 0.2 L dB per Bark -- the upper skirt flattens as the source gets louder, which is the upward
    // spread of masking that lets a loud bass or kick cover what lies above it.
    std::fill(out.begin(), out.end(), 0.0);
    constexpr double Ln10Over10 = 0.23025850929940458;
    for (size_t s = 0; s < B; ++s) {
        if (in[s] <= 0.0) continue;
        const double level = 10.0 * std::log10(in[s]);
        const double upper = std::max(MinUpperSlope, 24.0 + 230.0 / m_centre[s] - 0.2 * level);
        for (size_t r = 0; r < B; ++r) {
            const double dz = m_spread[r * B + s];
            out[r] += in[s] * std::exp(Ln10Over10 * (dz >= 0.0 ? -upper * dz : LowerSlope * dz));
        }
    }
    return true;
}

void AudibilityMeter::loudness(const std::vector<double>& es, const std::vector<double>* em, double& alone, double& inMix) const
{
    // Moore, Glasberg & Baer 1997, eq. (3) to (6), with the cochlear gain G = 1 (the simplification: it only departs
    // from 1 below 500 Hz), A = 2 E_THRQ and K = 0.5 (-3 dB, their value above 500 Hz). In quiet a part's specific
    // loudness is C [(E + A)^a - A^a]. In a masker it depends on whether the part is above its masked threshold
    // E_THRN = K E_N + E_THRQ: above it, the loudness of part and masker together less the masker's own
    // contribution, the latter weighted by (E_THRN / E_SIG)^0.3 so that a part far above the masker keeps nearly
    // all of its loudness; below it, a steep fall towards zero at threshold. Never more than the part alone.
    alone = 0.0;
    inMix = 0.0;
    for (size_t r = 0; r < es.size(); ++r) {
        const double s = es[r];
        if (s <= 0.0) continue;
        const double t = m_threshold[r], a = 2.0 * t;
        const double nq = std::pow(s + a, Alpha) - std::pow(a, Alpha);
        alone += nq;
        const double n = em != nullptr ? (*em)[r] : 0.0;
        if (n <= 0.0) { inMix += nq; continue; }
        constexpr double K = 0.5;
        const double thrN = K * n + t;
        double np;
        if (s >= thrN) {
            np = (std::pow(s + n + a, Alpha) - std::pow(a, Alpha))
               - (std::pow(n * (1.0 + K) + t + a, Alpha) - std::pow(t + a, Alpha)) * std::pow(thrN / s, 0.3);
        } else {
            const double den = std::pow(n * (1.0 + K) + t + a, Alpha) - std::pow(n * (1.0 + K) + a, Alpha);
            np = std::pow(2.0 * s / (s + thrN), 1.5) * (den > 0.0 ? (std::pow(t + a, Alpha) - std::pow(a, Alpha)) / den : 0.0)
               * (std::pow(s + n + a, Alpha) - std::pow(n + a, Alpha));
        }
        inMix += std::clamp(np, 0.0, nq);
    }
}

void AudibilityMeter::frame()
{
    const size_t B = static_cast<size_t>(m_bands);
    std::vector<std::complex<double>> x(Frame);
    std::vector<std::vector<double>> exc(static_cast<size_t>(m_stems), std::vector<double>(B, 0.0));
    std::vector<bool> sounding(static_cast<size_t>(m_stems), false);
    std::vector<double> total(B, 0.0), in(B);
    for (int k = 0; k < m_stems; ++k) {
        std::fill(in.begin(), in.end(), 0.0);
        bandIntensity(m_bufL[static_cast<size_t>(k)], in, x);
        bandIntensity(m_bufR[static_cast<size_t>(k)], in, x);
        // Excitation: every stem's intensity spread over the bands; the total, from which each stem's masker is the rest.
        sounding[static_cast<size_t>(k)] = excite(in, exc[static_cast<size_t>(k)]);
        if (sounding[static_cast<size_t>(k)]) for (size_t r = 0; r < B; ++r) total[r] += exc[static_cast<size_t>(k)][r];
    }
    std::vector<double> masker(B);
    for (int k = 0; k < m_stems; ++k) {
        if (!sounding[static_cast<size_t>(k)]) continue;
        for (size_t r = 0; r < B; ++r) masker[r] = std::max(0.0, total[r] - exc[static_cast<size_t>(k)][r]);
        double alone = 0.0, inMix = 0.0;
        loudness(exc[static_cast<size_t>(k)], &masker, alone, inMix);
        // Only frames in which the part is heard at all on its own (AudibleFrame): a voice's release tail is never
        // exactly zero, and averaging it in read a line with pauses between its phrases as far quieter than it
        // plays -- the counter of seed 1's drop 2 at a third of the lead where the probe of its phrases read it level.
        if (alone * m_scale < AudibleFrame) continue;
        m_sumAlone[static_cast<size_t>(k)] += alone * m_scale;
        m_sumMix[static_cast<size_t>(k)] += inMix * m_scale;
        ++m_frames[static_cast<size_t>(k)];
    }
}

AudibilityReading AudibilityMeter::read(int k) const
{
    AudibilityReading r;
    if (k < 0 || k >= m_stems) return r;
    r.m_frames = m_frames[static_cast<size_t>(k)];
    if (r.m_frames == 0) return r;
    r.m_alone = m_sumAlone[static_cast<size_t>(k)] / r.m_frames;
    r.m_inMix = m_sumMix[static_cast<size_t>(k)] / r.m_frames;
    r.m_ratio = r.m_alone > 0.0 ? r.m_inMix / r.m_alone : 0.0;
    return r;
}

} // namespace phos
