/**
 * @file Audibility.cpp
 * @brief Partial loudness of stems in a mix (Audibility.h has the model and its sources).
 */
#include "phos/Audibility.h"
#include "phos/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace phos {

namespace {

constexpr int kFrame = 2048;
constexpr int kHop = 1024;
constexpr double kAlpha = 0.2;          ///< the compressive exponent of specific loudness
constexpr double kFullScaleSpl = 100.0; ///< dB SPL of a full-scale sine
constexpr double kCamStep = 0.5;        ///< band width in Cam
constexpr double kLowerSlope = 27.0;    ///< dB per Bark on the low side of a source (Terhardt 1979)
constexpr double kMinUpperSlope = 5.0;  ///< the upper slope never flattens below this (dB per Bark)

/** @brief Critical-band rate in Bark (Traunmueller 1990). */
double bark(double hz) { return 26.81 * hz / (1960.0 + hz) - 0.53; }

/** @brief ERB-number (Cam) of @p hz (Glasberg & Moore 1990). */
double cam(double hz) { return 21.4 * std::log10(4.37e-3 * hz + 1.0); }

/** @brief Outer and middle ear weighting of ITU-R BS.1387 (PEAQ), dB. */
double earWeightDb(double hz)
{
    const double f = std::max(hz, 20.0) / 1000.0;
    return -0.6 * 3.64 * std::pow(f, -0.8) + 6.5 * std::exp(-0.6 * (f - 3.3) * (f - 3.3)) - 1e-3 * std::pow(f, 3.6);
}

/** @brief The threshold's excitation after the ear weighting, dB SPL (the internal noise of PEAQ, raised by 3 dB). */
double thresholdDb(double hz)
{
    const double f = std::max(hz, 20.0) / 1000.0;
    return 3.0 + 0.4 * 3.64 * std::pow(f, -0.8);
}

} // namespace

AudibilityMeter::AudibilityMeter(int stems, double sampleRate) : stems_(std::max(1, stems)), sr_(sampleRate)
{
    fft_ = std::make_unique<Fft>(kFrame);
    window_.resize(kFrame);
    double w2 = 0.0;
    for (int i = 0; i < kFrame; ++i) {
        window_[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / kFrame);
        w2 += window_[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)];
    }
    const double lo = cam(50.0), hi = cam(15000.0);
    bands_ = static_cast<int>(std::floor((hi - lo) / kCamStep)) + 1;
    // One side of the spectrum: the bins in a band sum to the mean square of the windowed signal times 2 / (N sum w^2)
    // (Parseval), a full-scale sine's mean square is 1/2, and that is kFullScaleSpl.
    const double toMeanSquare = 2.0 / (static_cast<double>(kFrame) * w2);
    bandOfBin_.assign(kFrame / 2, -1);
    binGain_.assign(kFrame / 2, 0.0);
    for (int k = 1; k < kFrame / 2; ++k) {
        const double hz = k * sr_ / kFrame;
        if (hz < 50.0 || hz > 15000.0) continue;
        const int b = std::clamp(static_cast<int>(std::lround((cam(hz) - lo) / kCamStep)), 0, bands_ - 1);
        bandOfBin_[static_cast<size_t>(k)] = b;
        binGain_[static_cast<size_t>(k)] = toMeanSquare / 0.5 * std::pow(10.0, (kFullScaleSpl + earWeightDb(hz)) / 10.0);
    }
    spread_.assign(static_cast<size_t>(bands_) * static_cast<size_t>(bands_), 0.0);
    threshold_.assign(static_cast<size_t>(bands_), 0.0);
    centre_.assign(static_cast<size_t>(bands_), 0.0);
    for (int r = 0; r < bands_; ++r) {
        // Centre frequency of the band from its Cam value.
        const double c = lo + r * kCamStep;
        const double hz = (std::pow(10.0, c / 21.4) - 1.0) / 4.37e-3;
        centre_[static_cast<size_t>(r)] = hz;
        threshold_[static_cast<size_t>(r)] = std::pow(10.0, thresholdDb(hz) / 10.0);
    }
    // The spreading works in Bark (the slopes are Terhardt's, stated per Bark): distance of every receiver from every
    // source, positive when the receiver lies above.
    for (int r = 0; r < bands_; ++r)
        for (int s = 0; s < bands_; ++s)
            spread_[static_cast<size_t>(r) * static_cast<size_t>(bands_) + static_cast<size_t>(s)] = bark(centre_[static_cast<size_t>(r)]) - bark(centre_[static_cast<size_t>(s)]);
    // Calibration through the path every frame takes: a 1 kHz sine at 40 dB SPL (-60 dBFS) in both channels.
    // (Placing its intensity in one band by hand read 1.38 in the real path: the window spreads a tone over
    // neighbouring bins, those can fall into two bands, and the compressive loudness of two halves is more than
    // that of the whole.)
    {
        scale_ = 1.0;
        std::vector<float> sine(kFrame);
        const double amp = std::pow(10.0, (40.0 - kFullScaleSpl) / 20.0);   // peak of a sine at 40 dB SPL (full scale = kFullScaleSpl)
        for (int i = 0; i < kFrame; ++i) sine[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * 3.141592653589793 * 1000.0 * i / sr_));
        std::vector<std::complex<double>> x(kFrame);
        std::vector<double> in(static_cast<size_t>(bands_), 0.0), e(static_cast<size_t>(bands_), 0.0);
        bandIntensity(sine, in, x);
        bandIntensity(sine, in, x);
        double alone = 0.0, inMix = 0.0;
        if (excite(in, e)) loudness(e, nullptr, alone, inMix);
        scale_ = alone > 0.0 ? 1.0 / alone : 1.0;
    }
    bufL_.assign(static_cast<size_t>(stems_), std::vector<float>(kFrame, 0.0f));
    bufR_.assign(static_cast<size_t>(stems_), std::vector<float>(kFrame, 0.0f));
    reset();
}

AudibilityMeter::~AudibilityMeter() = default;

void AudibilityMeter::reset()
{
    fill_ = 0;
    for (auto& b : bufL_) std::fill(b.begin(), b.end(), 0.0f);
    for (auto& b : bufR_) std::fill(b.begin(), b.end(), 0.0f);
    sumAlone_.assign(static_cast<size_t>(stems_), 0.0);
    sumMix_.assign(static_cast<size_t>(stems_), 0.0);
    frames_.assign(static_cast<size_t>(stems_), 0);
}

void AudibilityMeter::add(const float* const* L, const float* const* R, int n)
{
    int done = 0;
    while (done < n) {
        const int take = std::min(n - done, kFrame - fill_);
        for (int k = 0; k < stems_; ++k) {
            float* bl = bufL_[static_cast<size_t>(k)].data() + fill_;
            float* br = bufR_[static_cast<size_t>(k)].data() + fill_;
            const float* l = L[k];
            const float* r = R != nullptr ? R[k] : nullptr;
            for (int i = 0; i < take; ++i) {
                bl[i] = l != nullptr ? l[done + i] : 0.0f;
                br[i] = r != nullptr ? r[done + i] : bl[i];
            }
        }
        fill_ += take;
        done += take;
        if (fill_ == kFrame) {
            frame();
            // Keep the second half: the next frame overlaps this one by kFrame - kHop samples.
            for (int k = 0; k < stems_; ++k) {
                std::copy(bufL_[static_cast<size_t>(k)].begin() + kHop, bufL_[static_cast<size_t>(k)].end(), bufL_[static_cast<size_t>(k)].begin());
                std::copy(bufR_[static_cast<size_t>(k)].begin() + kHop, bufR_[static_cast<size_t>(k)].end(), bufR_[static_cast<size_t>(k)].begin());
            }
            fill_ = kFrame - kHop;
        }
    }
}

void AudibilityMeter::bandIntensity(const std::vector<float>& buf, std::vector<double>& out, std::vector<std::complex<double>>& x) const
{
    bool any = false;
    for (int i = 0; i < kFrame; ++i) {
        x[static_cast<size_t>(i)] = std::complex<double>(buf[static_cast<size_t>(i)] * window_[static_cast<size_t>(i)], 0.0);
        any = any || buf[static_cast<size_t>(i)] != 0.0f;
    }
    if (!any) return;
    fft_->transform(x, false);
    for (int b = 1; b < kFrame / 2; ++b) {
        const int band = bandOfBin_[static_cast<size_t>(b)];
        if (band < 0) continue;
        out[static_cast<size_t>(band)] += 0.5 * std::norm(x[static_cast<size_t>(b)]) * binGain_[static_cast<size_t>(b)];   // two channels averaged
    }
}

bool AudibilityMeter::excite(const std::vector<double>& in, std::vector<double>& out) const
{
    const size_t B = static_cast<size_t>(bands_);
    bool any = false;
    for (double v : in) any = any || v > 0.0;
    if (!any) return false;
    // Level-dependent spreading (Terhardt 1979, as in ITU-R BS.1387): 27 dB per Bark below a source, and above it
    // 24 + 230 / f - 0.2 L dB per Bark -- the upper skirt flattens as the source gets louder, which is the upward
    // spread of masking that lets a loud bass or kick cover what lies above it.
    std::fill(out.begin(), out.end(), 0.0);
    constexpr double kLn10Over10 = 0.23025850929940458;
    for (size_t s = 0; s < B; ++s) {
        if (in[s] <= 0.0) continue;
        const double level = 10.0 * std::log10(in[s]);
        const double upper = std::max(kMinUpperSlope, 24.0 + 230.0 / centre_[s] - 0.2 * level);
        for (size_t r = 0; r < B; ++r) {
            const double dz = spread_[r * B + s];
            out[r] += in[s] * std::exp(kLn10Over10 * (dz >= 0.0 ? -upper * dz : kLowerSlope * dz));
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
        const double t = threshold_[r], a = 2.0 * t;
        const double nq = std::pow(s + a, kAlpha) - std::pow(a, kAlpha);
        alone += nq;
        const double n = em != nullptr ? (*em)[r] : 0.0;
        if (n <= 0.0) { inMix += nq; continue; }
        constexpr double K = 0.5;
        const double thrN = K * n + t;
        double np;
        if (s >= thrN) {
            np = (std::pow(s + n + a, kAlpha) - std::pow(a, kAlpha))
               - (std::pow(n * (1.0 + K) + t + a, kAlpha) - std::pow(t + a, kAlpha)) * std::pow(thrN / s, 0.3);
        } else {
            const double den = std::pow(n * (1.0 + K) + t + a, kAlpha) - std::pow(n * (1.0 + K) + a, kAlpha);
            np = std::pow(2.0 * s / (s + thrN), 1.5) * (den > 0.0 ? (std::pow(t + a, kAlpha) - std::pow(a, kAlpha)) / den : 0.0)
               * (std::pow(s + n + a, kAlpha) - std::pow(n + a, kAlpha));
        }
        inMix += std::clamp(np, 0.0, nq);
    }
}

void AudibilityMeter::frame()
{
    const size_t B = static_cast<size_t>(bands_);
    std::vector<std::complex<double>> x(kFrame);
    std::vector<std::vector<double>> exc(static_cast<size_t>(stems_), std::vector<double>(B, 0.0));
    std::vector<bool> sounding(static_cast<size_t>(stems_), false);
    std::vector<double> total(B, 0.0), in(B);
    for (int k = 0; k < stems_; ++k) {
        std::fill(in.begin(), in.end(), 0.0);
        bandIntensity(bufL_[static_cast<size_t>(k)], in, x);
        bandIntensity(bufR_[static_cast<size_t>(k)], in, x);
        // Excitation: every stem's intensity spread over the bands; the total, from which each stem's masker is the rest.
        sounding[static_cast<size_t>(k)] = excite(in, exc[static_cast<size_t>(k)]);
        if (sounding[static_cast<size_t>(k)]) for (size_t r = 0; r < B; ++r) total[r] += exc[static_cast<size_t>(k)][r];
    }
    std::vector<double> masker(B);
    for (int k = 0; k < stems_; ++k) {
        if (!sounding[static_cast<size_t>(k)]) continue;
        for (size_t r = 0; r < B; ++r) masker[r] = std::max(0.0, total[r] - exc[static_cast<size_t>(k)][r]);
        double alone = 0.0, inMix = 0.0;
        loudness(exc[static_cast<size_t>(k)], &masker, alone, inMix);
        if (alone <= 0.0) continue;
        sumAlone_[static_cast<size_t>(k)] += alone * scale_;
        sumMix_[static_cast<size_t>(k)] += inMix * scale_;
        ++frames_[static_cast<size_t>(k)];
    }
}

AudibilityReading AudibilityMeter::read(int k) const
{
    AudibilityReading r;
    if (k < 0 || k >= stems_) return r;
    r.frames = frames_[static_cast<size_t>(k)];
    if (r.frames == 0) return r;
    r.alone = sumAlone_[static_cast<size_t>(k)] / r.frames;
    r.inMix = sumMix_[static_cast<size_t>(k)] / r.frames;
    r.ratio = r.alone > 0.0 ? r.inMix / r.alone : 0.0;
    return r;
}

} // namespace phos
