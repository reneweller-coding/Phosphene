/**
 * @file Fft.h
 * @brief A plain radix-2 complex FFT in double precision, for building tables at start-up.
 *
 * Never used on the audio thread. Iterative Cox-de Boor bit reversal and in-place butterflies with
 * the trigonometry precomputed per size.
 */
#pragma once
#include <cmath>
#include <complex>
#include <vector>

namespace phos {

/** @brief FFT of one power-of-two size. */
class Fft {
public:
    /** @brief Prepares for @p n samples (a power of two). */
    explicit Fft(int n) : n_(n)
    {
        cosT_.resize(static_cast<size_t>(n / 2));
        sinT_.resize(static_cast<size_t>(n / 2));
        for (int i = 0; i < n / 2; ++i) {
            cosT_[static_cast<size_t>(i)] = std::cos(2.0 * 3.141592653589793 * i / n);
            sinT_[static_cast<size_t>(i)] = std::sin(2.0 * 3.141592653589793 * i / n);
        }
    }
    /** @brief The size. */
    int size() const { return n_; }
    /**
     * @brief Transforms in place.
     * @param a       n complex values
     * @param inverse false: X(k) = sum x(t) e^{-j 2 pi k t / n}; true: the inverse including 1/n
     */
    void transform(std::vector<std::complex<double>>& a, bool inverse) const
    {
        const int n = n_;
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[static_cast<size_t>(i)], a[static_cast<size_t>(j)]);
        }
        for (int len = 2; len <= n; len <<= 1) {
            const int step = n / len;
            for (int i = 0; i < n; i += len) {
                for (int k = 0; k < len / 2; ++k) {
                    const std::complex<double> w(cosT_[static_cast<size_t>(k * step)], inverse ? sinT_[static_cast<size_t>(k * step)] : -sinT_[static_cast<size_t>(k * step)]);
                    const std::complex<double> u = a[static_cast<size_t>(i + k)], v = a[static_cast<size_t>(i + k + len / 2)] * w;
                    a[static_cast<size_t>(i + k)] = u + v;
                    a[static_cast<size_t>(i + k + len / 2)] = u - v;
                }
            }
        }
        if (inverse) for (auto& v : a) v /= static_cast<double>(n);
    }

private:
    int n_;   ///< size
    std::vector<double> cosT_;   ///< the twiddle factors: cosines
    std::vector<double> sinT_;   ///< ... sines
};

} // namespace phos
