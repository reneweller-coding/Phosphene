/**
 * @file Fft.h
 * @brief A plain radix-2 complex FFT in double precision, for building tables at start-up.
 *
 * Never used on the audio thread. Iterative Cox-de Boor bit reversal and in-place butterflies with
 * the trigonometry precomputed per size.
 * @author Rene Weller
 */
#pragma once
#include <cmath>
#include <complex>
#include <vector>

namespace phos {

/** @brief FFT of one power-of-two size. */
class Fft {
public:
    /**
     * @brief Prepares for @p n samples (a power of two).
     * @param[in] n  the transform length, a power of two
     */
    explicit Fft(int n) : m_n(n)
    {
        m_cosT.resize(static_cast<size_t>(n / 2));
        m_sinT.resize(static_cast<size_t>(n / 2));
        for (int i = 0; i < n / 2; ++i) {
            m_cosT[static_cast<size_t>(i)] = std::cos(2.0 * 3.141592653589793 * i / n);
            m_sinT[static_cast<size_t>(i)] = std::sin(2.0 * 3.141592653589793 * i / n);
        }
    }
    /**
     * @brief The size.
     * @return the transform length
     */
    int size() const { return m_n; }
    /**
     * @brief Transforms in place.
     * @param[in,out] a        n complex values
     * @param[in]     inverse  false: X(k) = sum x(t) e^{-j 2 pi k t / n}; true: the inverse including 1/n
     */
    void transform(std::vector<std::complex<double>>& a, bool inverse) const
    {
        const int n = m_n;
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
                    const std::complex<double> w(m_cosT[static_cast<size_t>(k * step)], inverse ? m_sinT[static_cast<size_t>(k * step)] : -m_sinT[static_cast<size_t>(k * step)]);
                    const std::complex<double> u = a[static_cast<size_t>(i + k)], v = a[static_cast<size_t>(i + k + len / 2)] * w;
                    a[static_cast<size_t>(i + k)] = u + v;
                    a[static_cast<size_t>(i + k + len / 2)] = u - v;
                }
            }
        }
        if (inverse) for (auto& v : a) v /= static_cast<double>(n);
    }

private:
    int m_n;   ///< size
    std::vector<double> m_cosT;   ///< the twiddle factors: cosines
    std::vector<double> m_sinT;   ///< ... sines
};

} // namespace phos
