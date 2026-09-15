/**
 * @file Corpus.cpp
 * @brief Pitch model and chord successions from the corpus tables.
 */
#include "phos/Corpus.h"
#include <algorithm>

namespace phos {

namespace {

/** @brief Count of a key in a sorted gram list (binary search). */
uint32_t countOf(const CorpusGram* grams, int n, uint32_t key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (grams[mid].key == key) return grams[mid].count;
        if (grams[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

} // namespace

PitchModel::PitchModel(CorpusRoleId id)
{
    const CorpusRole& r = kCorpusRoles[static_cast<int>(id)];
    const int A = kCorpusAlphabet;
    // Order 0, add-one smoothed so no symbol is impossible before the constraints speak.
    double n0 = 0.0;
    for (int c = 0; c < A; ++c) n0 += r.uni[c];
    std::vector<double> p0(static_cast<size_t>(A));
    for (int c = 0; c < A; ++c) p0[static_cast<size_t>(c)] = (r.uni[c] + 1.0) / (n0 + A);

    // Order 1: Witten-Bell -- (C(b,c) + T(b) P0(c)) / (C(b) + T(b)).
    std::vector<double> p1(static_cast<size_t>(A * A));
    for (int b = 0; b < A; ++b) {
        double total = 0.0, types = 0.0;
        for (int c = 0; c < A; ++c) {
            const uint32_t k = countOf(r.bi, r.numBi, static_cast<uint32_t>(b * A + c));
            total += k;
            types += k > 0 ? 1.0 : 0.0;
        }
        for (int c = 0; c < A; ++c) {
            const double k = countOf(r.bi, r.numBi, static_cast<uint32_t>(b * A + c));
            p1[static_cast<size_t>(b * A + c)] = total > 0.0 ? (k + types * p0[static_cast<size_t>(c)]) / (total + types) : p0[static_cast<size_t>(c)];
        }
    }

    // Order 2, interpolated with order 1 the same way.
    p_.assign(static_cast<size_t>(A * A * A), 0.0);
    for (int a = 0; a < A; ++a) {
        for (int b = 0; b < A; ++b) {
            double total = 0.0, types = 0.0;
            for (int c = 0; c < A; ++c) {
                const uint32_t k = countOf(r.tri, r.numTri, static_cast<uint32_t>((a * A + b) * A + c));
                total += k;
                types += k > 0 ? 1.0 : 0.0;
            }
            for (int c = 0; c < A; ++c) {
                const double k = countOf(r.tri, r.numTri, static_cast<uint32_t>((a * A + b) * A + c));
                const double lower = p1[static_cast<size_t>(b * A + c)];
                p_[static_cast<size_t>((a * A + b) * A + c)] = total > 0.0 ? (k + types * lower) / (total + types) : lower;
            }
        }
    }
}

double chordTransition(int fromRel, int toRel)
{
    const int from = ((fromRel % 12) + 12) % 12, to = ((toRel % 12) + 12) % 12;
    // Roots that occur at all; add-half smoothing among them only, so the progressions stay in the
    // vocabulary the corpus actually uses.
    bool seen[12] = {};
    for (int a = 0; a < 12; ++a) for (int b = 0; b < 12; ++b) if (kCorpusChordTransitions[a][b] > 0) { seen[a] = true; seen[b] = true; }
    if (!seen[to]) return 0.0;
    double row = 0.0;
    int kinds = 0;
    for (int b = 0; b < 12; ++b) {
        if (!seen[b]) continue;
        row += kCorpusChordTransitions[from][b] + 0.5;
        ++kinds;
    }
    if (kinds == 0) return 0.0;
    return (kCorpusChordTransitions[from][to] + 0.5) / row;
}

} // namespace phos
