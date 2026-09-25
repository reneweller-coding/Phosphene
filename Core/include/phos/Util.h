/**
 * @file Util.h
 * @brief Small helpers several sources of the core share: random draws, a filter's coefficients, file and text utilities.
 *
 * Until 25.09.2026 each of these lived as a private copy in two or three .cpp files (Composer and Rhythm drew their
 * normal numbers each with their own Box-Muller, Form and Melody their weighted index, Perc and Poly their filter
 * coefficients, Model, Vocal and WaveTableFile each read files and reported errors in their own way). The copies that
 * were the same are this one now, to the character, so every render is the same to the bit; where they differed
 * (reading a file, reporting a format error) the stricter version is the one that stayed.
 */
#pragma once
#include "phos/Dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace phos {

/** @brief A standard normal draw from @p r (Box-Muller: two uniforms per draw, the cosine branch). */
inline double gaussian(Rng& r)
{
    const double u1 = 1.0 - static_cast<double>(r.uniform());
    const double u2 = static_cast<double>(r.uniform());
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.141592653589793 * u2);
}

/** @brief An index in [0, @p n) drawn from non-negative weights @p w; uniform when they are all zero. */
inline int drawIndex(Rng& r, const double* w, int n)
{
    double total = 0.0;
    for (int i = 0; i < n; ++i) total += w[i];
    if (total <= 0.0) return r.below(n);
    double x = static_cast<double>(r.uniform()) * total;
    for (int i = 0; i < n; ++i) { if (x < w[i]) return i; x -= w[i]; }
    return n - 1;
}

/**
 * @brief The trapezoidal state-variable filter's coefficients (Zavalishin) for cutoff @p fc and damping @p damping at
 *        @p sr; the cutoff is kept between 10 Hz and 0.45 of the sample rate.
 */
inline void svfCoefs(double fc, double damping, double sr, float& a1, float& a2, float& a3)
{
    constexpr double kPiD = 3.141592653589793;
    const double g = std::tan(kPiD * std::clamp(fc, 10.0, 0.45 * sr) / sr);
    const double d1 = 1.0 / (1.0 + g * (g + damping));
    a1 = static_cast<float>(d1);
    a2 = static_cast<float>(g * d1);
    a3 = static_cast<float>(g * g * d1);
}

/** @brief Reads a whole file into @p out; false if it cannot be opened, is empty, or cannot be read completely. */
inline bool readFile(const char* path, std::vector<uint8_t>& out)
{
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(n));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}
/** @brief readFile() for a path held as a std::string. */
inline bool readFile(const std::string& path, std::vector<uint8_t>& out) { return readFile(path.c_str(), out); }

/**
 * @brief Writes "@p what (at byte @p at)" into @p error (when it is not null) and returns false: the one-line
 *        failure of a binary file reader.
 */
inline bool fileError(std::string* error, const char* what, size_t at)
{
    if (error != nullptr) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (at byte %llu)", what, static_cast<unsigned long long>(at));
        *error = buf;
    }
    return false;
}

/** @brief @p s without leading and trailing spaces, tabs and carriage returns. */
inline std::string_view trimView(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

} // namespace phos
