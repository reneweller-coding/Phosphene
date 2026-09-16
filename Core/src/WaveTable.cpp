/**
 * @file WaveTable.cpp
 * @brief Table building from spectra, level choice, and the built-in psytrance pad tables.
 */
#include "phos/WaveTable.h"
#include "phos/Fft.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace phos {

namespace {

using Coeffs = std::vector<std::complex<double>>;
constexpr double kPiD = 3.14159265358979323846;

/** @brief One stored cycle at a level from the harmonics it keeps; writes the guards as well. */
void synthesise(const Coeffs& c, double gain, int level, const Fft& fft, std::vector<std::complex<double>>& buf, float* out)
{
    const int len = WaveTable::levelLength(level);
    const int top = std::min(WaveTable::levelHarmonics(level), static_cast<int>(c.size()));
    std::fill(buf.begin(), buf.begin() + len, std::complex<double>(0.0, 0.0));
    const double half = 0.5 * static_cast<double>(len) * gain;
    for (int h = 1; h <= top; ++h) {
        const std::complex<double> v = c[static_cast<size_t>(h - 1)] * half;
        buf[static_cast<size_t>(h)] = v;
        buf[static_cast<size_t>(len - h)] = std::conj(v);
    }
    fft.transform(buf, true);
    for (int n = 0; n < len; ++n) out[n] = static_cast<float>(buf[static_cast<size_t>(n)].real());
    out[-1] = out[len - 1];
    out[len] = out[0];
    out[len + 1] = out[1];
}

/** @brief Scales a frame to unit RMS (Parseval: a cosine of amplitude A carries A^2 / 2). */
Coeffs unit(Coeffs c)
{
    double e = 0.0;
    for (const auto& v : c) e += std::norm(v);
    const double s = e > 0.0 ? 1.0 / std::sqrt(0.5 * e) : 0.0;
    for (auto& v : c) v *= s;
    return c;
}

const std::complex<double> kSine(0.0, -1.0);   ///< sin x = cos(x - pi/2)

/** @brief Harmonic h of a rising saw, 2t - 1: -(2/pi) sum sin(2 pi h t)/h. */
std::complex<double> sawHarmonic(int h) { return kSine * (-2.0 / (kPiD * h)); }

struct Builtins {
    WaveTable t[kNumBuiltinWaveTables];
    Builtins()
    {
        const int H = WaveTable::levelHarmonics(0);
        // 0 Classic: sine, triangle, saw, square, pulse of a fifth, to the 512th harmonic.
        {
            std::vector<Coeffs> f(5, Coeffs(static_cast<size_t>(H)));
            f[0][0] = kSine;
            for (int h = 1; h <= H; ++h) {
                if (h % 2 == 1) f[1][static_cast<size_t>(h - 1)] = kSine * ((((h - 1) / 2) % 2 == 0 ? 1.0 : -1.0) * 8.0 / (kPiD * kPiD * h * h));
                f[2][static_cast<size_t>(h - 1)] = sawHarmonic(h);
                if (h % 2 == 1) f[3][static_cast<size_t>(h - 1)] = kSine * (4.0 / (kPiD * h));
                f[4][static_cast<size_t>(h - 1)] = std::complex<double>(2.0 / (kPiD * h) * std::sin(kPiD * h * 0.2), 0.0);
            }
            for (auto& c : f) c = unit(std::move(c));
            t[0].buildFromHarmonics(f);
        }
        constexpr int F = 32;
        // 1 Vocal: a saw through three formants of a, e, i, o, u (Peterson and Barney 1952, adult male),
        // placed as if the table were played at C3; the vowels morph across the frames.
        {
            static const double kFormant[5][3] = { { 730, 1090, 2440 }, { 530, 1840, 2480 }, { 270, 2290, 3010 }, { 570, 840, 2410 }, { 300, 870, 2240 } };
            static const double kGain[3] = { 1.0, 0.5, 0.25 }, kWidth[3] = { 80.0, 100.0, 120.0 };
            constexpr double ref = 130.81;
            std::vector<Coeffs> f(F, Coeffs(static_cast<size_t>(H)));
            for (int k = 0; k < F; ++k) {
                const double pos = 4.0 * k / (F - 1);
                const int v = std::min(3, static_cast<int>(pos));
                const double w = pos - v;
                for (int h = 1; h <= H; ++h) {
                    const double hz = h * ref;
                    double a = 0.0;
                    for (int i = 0; i < 3; ++i) {
                        const double fr = kFormant[v][i] + w * (kFormant[v + 1][i] - kFormant[v][i]);
                        const double d = (hz - fr) / kWidth[i];
                        a += kGain[i] / (1.0 + d * d);
                    }
                    f[static_cast<size_t>(k)][static_cast<size_t>(h - 1)] = sawHarmonic(h) * (0.05 + a) * (hz < 12000.0 ? 1.0 : 0.0);
                }
            }
            for (auto& c : f) c = unit(std::move(c));
            t[1].buildFromHarmonics(f);
        }
        // 2 Glass: sparse partials, the upper ones rising across the frames.
        {
            static const int kPartials[] = { 1, 2, 3, 4, 6, 8, 11, 14, 18, 23, 29, 36, 45, 57, 72 };
            std::vector<Coeffs> f(F, Coeffs(static_cast<size_t>(H)));
            for (int k = 0; k < F; ++k) {
                const double x = static_cast<double>(k) / (F - 1);
                for (int h : kPartials) f[static_cast<size_t>(k)][static_cast<size_t>(h - 1)] = kSine * std::pow(static_cast<double>(h), -(1.3 - 0.9 * x));
            }
            for (auto& c : f) c = unit(std::move(c));
            t[2].buildFromHarmonics(f);
        }
        // 3 PWM: a pulse from 50 % to 5 % duty.
        {
            std::vector<Coeffs> f(F, Coeffs(static_cast<size_t>(H)));
            for (int k = 0; k < F; ++k) {
                const double w = 0.5 - 0.45 * k / (F - 1);
                for (int h = 1; h <= H; ++h) f[static_cast<size_t>(k)][static_cast<size_t>(h - 1)] = std::complex<double>(2.0 / (kPiD * h) * std::sin(kPiD * h * w), 0.0);
            }
            for (auto& c : f) c = unit(std::move(c));
            t[3].buildFromHarmonics(f);
        }
        // 4 Sync: a saw hard-synced to the cycle with the slave at 1 to 8 times the master. Written in
        // the time domain at 16 times the finest resolution and analysed, so the discontinuities fold
        // back only from above the 8000th harmonic.
        {
            constexpr int N = 65536;
            Fft fft(N);
            std::vector<std::complex<double>> buf(static_cast<size_t>(N));
            std::vector<Coeffs> f(F, Coeffs(static_cast<size_t>(H)));
            for (int k = 0; k < F; ++k) {
                const double ratio = 1.0 + 7.0 * k / (F - 1);
                for (int n = 0; n < N; ++n) {
                    const double s = ratio * n / N;
                    buf[static_cast<size_t>(n)] = 2.0 * (s - std::floor(s)) - 1.0;
                }
                fft.transform(buf, false);
                for (int h = 1; h <= H; ++h) f[static_cast<size_t>(k)][static_cast<size_t>(h - 1)] = buf[static_cast<size_t>(h)] * (2.0 / N);
            }
            for (auto& c : f) c = unit(std::move(c));
            t[4].buildFromHarmonics(f);
        }
        // 5 Formant Saw: a saw with a resonant peak moving from the 2nd to the 64th harmonic.
        {
            std::vector<Coeffs> f(F, Coeffs(static_cast<size_t>(H)));
            for (int k = 0; k < F; ++k) {
                const double centre = 2.0 * std::pow(2.0, 5.0 * k / (F - 1));
                for (int h = 1; h <= H; ++h) {
                    const double d = std::log2(h / centre) / 0.15;
                    f[static_cast<size_t>(k)][static_cast<size_t>(h - 1)] = sawHarmonic(h) * (1.0 + 8.0 * std::exp(-0.5 * d * d));
                }
            }
            for (auto& c : f) c = unit(std::move(c));
            t[5].buildFromHarmonics(f);
        }
    }
};

} // namespace

bool WaveTable::buildFromHarmonics(const std::vector<Coeffs>& in)
{
    frames = 0;
    data.clear();
    const int count = std::min(static_cast<int>(in.size()), kMaxFrames);
    if (count <= 0) return false;
    double loudest = 0.0;
    for (int k = 0; k < count; ++k) {
        double e = 0.0;
        const int top = std::min(static_cast<int>(in[static_cast<size_t>(k)].size()), levelHarmonics(0));
        for (int h = 0; h < top; ++h) e += std::norm(in[static_cast<size_t>(k)][static_cast<size_t>(h)]);
        loudest = std::max(loudest, std::sqrt(0.5 * e));
    }
    if (!(loudest > 1.0e-9)) return false;
    const double gain = static_cast<double>(kTargetRms) / loudest;
    size_t total = 0;
    for (int l = 0; l < kLevels; ++l) {
        offset[l] = static_cast<int>(total);
        total += static_cast<size_t>(count) * static_cast<size_t>(levelLength(l) + kGuard);
    }
    data.assign(total, 0.0f);
    std::vector<std::complex<double>> buf(static_cast<size_t>(kStoreLen));
    std::unique_ptr<Fft> fft;
    for (int l = 0; l < kLevels; ++l) {
        const int len = levelLength(l);
        if (fft == nullptr || fft->size() != len) fft = std::make_unique<Fft>(len);
        for (int k = 0; k < count; ++k) {
            float* out = data.data() + static_cast<size_t>(offset[l]) + static_cast<size_t>(k) * static_cast<size_t>(len + kGuard) + 1;
            synthesise(in[static_cast<size_t>(k)], gain, l, *fft, buf, out);
        }
    }
    frames = count;
    return true;
}

int waveLevelFor(double hz, double sampleRate, int current)
{
    const double nyquist = 0.5 * sampleRate;
    int floorLevel = 0;
    while (floorLevel < WaveTable::kLevels - 1 && WaveTable::levelHarmonics(floorLevel) * hz >= nyquist) ++floorLevel;
    if (current > floorLevel && current < WaveTable::kLevels) {
        int l = floorLevel;
        while (l < current && WaveTable::levelHarmonics(l) * hz > 0.9 * nyquist) ++l;
        return l;
    }
    return floorLevel;
}

const WaveTable& builtinWaveTable(int index)
{
    static const Builtins b;
    return b.t[index < 0 ? 0 : (index >= kNumBuiltinWaveTables ? kNumBuiltinWaveTables - 1 : index)];
}

} // namespace phos
