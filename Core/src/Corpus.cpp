/**
 * @file Corpus.cpp
 * @brief Pitch model and chord successions from the corpus tables.
 * @author Rene Weller
 */
#include "phos/Corpus.h"
#include <algorithm>
#include <cmath>

namespace phos {

namespace {

/**
 * @brief The chain context of a key, or null (binary search over the sorted table).
 * @param[in] key  the packed key
 * @return the context, or null
 */
const CorpusBassStep* stepOf(int key)
{
    int lo = 0, hi = NumCorpusBassSteps - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int k = static_cast<int>(CorpusBassSteps[mid].m_key);
        if (k == key) return &CorpusBassSteps[mid];
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

/**
 * @brief The stored count of a bar pattern, or zero (binary search over the sorted table).
 * @param[in] mask  the bar's onset mask
 * @return its count
 */
uint32_t barCountOf(unsigned mask)
{
    int lo = 0, hi = NumCorpusBassBars - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const unsigned m = CorpusBassBars[mid].m_mask;
        if (m == mask) return CorpusBassBars[mid].m_count;
        if (m < mask) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/**
 * @brief Count of a key in a sorted gram list (binary search).
 * @param[in] grams  the sorted grams
 * @param[in] n      their count
 * @param[in] key    the key
 * @return its count
 */
uint32_t countOf(const CorpusGram* grams, int n, uint32_t key)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (grams[mid].m_key == key) return grams[mid].m_count;
        if (grams[mid].m_key < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

} // namespace

PitchModel::PitchModel(CorpusRoleIdE id)
{
    const CorpusRole& r = CorpusRoles[static_cast<int>(id)];
    const int A = CorpusAlphabet;
    // Order 0, add-one smoothed so no symbol is impossible before the constraints speak.
    double n0 = 0.0;
    for (int c = 0; c < A; ++c) n0 += r.m_uni[c];
    std::vector<double> p0(static_cast<size_t>(A));
    for (int c = 0; c < A; ++c) p0[static_cast<size_t>(c)] = (r.m_uni[c] + 1.0) / (n0 + A);

    // Order 1: Witten-Bell -- (C(b,c) + T(b) P0(c)) / (C(b) + T(b)).
    std::vector<double> p1(static_cast<size_t>(A * A));
    for (int b = 0; b < A; ++b) {
        double total = 0.0, types = 0.0;
        for (int c = 0; c < A; ++c) {
            const uint32_t k = countOf(r.m_bi, r.m_numBi, static_cast<uint32_t>(b * A + c));
            total += k;
            types += k > 0 ? 1.0 : 0.0;
        }
        for (int c = 0; c < A; ++c) {
            const double k = countOf(r.m_bi, r.m_numBi, static_cast<uint32_t>(b * A + c));
            p1[static_cast<size_t>(b * A + c)] = total > 0.0 ? (k + types * p0[static_cast<size_t>(c)]) / (total + types) : p0[static_cast<size_t>(c)];
        }
    }

    // Order 2, interpolated with order 1 the same way.
    m_p.assign(static_cast<size_t>(A * A * A), 0.0);
    for (int a = 0; a < A; ++a) {
        for (int b = 0; b < A; ++b) {
            double total = 0.0, types = 0.0;
            for (int c = 0; c < A; ++c) {
                const uint32_t k = countOf(r.m_tri, r.m_numTri, static_cast<uint32_t>((a * A + b) * A + c));
                total += k;
                types += k > 0 ? 1.0 : 0.0;
            }
            for (int c = 0; c < A; ++c) {
                const double k = countOf(r.m_tri, r.m_numTri, static_cast<uint32_t>((a * A + b) * A + c));
                const double lower = p1[static_cast<size_t>(b * A + c)];
                m_p[static_cast<size_t>((a * A + b) * A + c)] = total > 0.0 ? (k + types * lower) / (total + types) : lower;
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
    for (int a = 0; a < 12; ++a) for (int b = 0; b < 12; ++b) if (CorpusChordTransitions[a][b] > 0) { seen[a] = true; seen[b] = true; }
    if (!seen[to]) return 0.0;
    double row = 0.0;
    int kinds = 0;
    for (int b = 0; b < 12; ++b) {
        if (!seen[b]) continue;
        row += CorpusChordTransitions[from][b] + 0.5;
        ++kinds;
    }
    if (kinds == 0) return 0.0;
    return (CorpusChordTransitions[from][to] + 0.5) / row;
}

// ------------------------------------------------------------------------------------ bass rhythm

unsigned BassRhythm::familyMask(int pattern)
{
    const int p = pattern < 0 ? 0 : (pattern >= NumBassPatterns ? NumBassPatterns - 1 : pattern);
    const BassPatternDef& d = BassPatterns[p];
    unsigned mask = 0;
    for (int beat = 0; beat < 4; ++beat)
        for (int i = 0; i < d.m_count; ++i) {
            // The same rounding Composer::bassSlotStep uses, and for the same reason (Corpus.h).
            const int step = beat * 4 + static_cast<int>(std::lround(d.m_pos[i] * 4.0));
            if (step % 4 != 0 && step < Steps) mask |= 1u << step;
        }
    return mask;
}

int BassRhythm::contextKey(unsigned mask, int step)
{
    // Base three per neighbour -- absent (2), silent (0), struck (1) -- most distant neighbour first,
    // then the step itself in the low four digits of base 16. The packing is the one
    // Tools/corpus/bass_rhythm.py::key_code writes, and the self test checks the two agree on a case
    // computed by hand.
    int code = 0;
    for (int i = Context - 1; i >= 0; --i) {
        const int s = step - ContextDelta[i];
        code = code * 3 + (s < 0 ? 2 : static_cast<int>((mask >> s) & 1u));
    }
    return code * Steps + step;
}

double BassRhythm::stepProbability(unsigned mask, int step)
{
    if (step % 4 == 0) return 0.0;                     // the kick step is not in the alphabet
    const CorpusBassStep* c = stepOf(contextKey(mask, step));
    // Krichevsky-Trofimov: add one half per outcome. An unseen context is therefore a fair coin
    // rather than an impossibility, which is what keeps every clean bar reachable.
    const double miss = c != nullptr ? static_cast<double>(c->m_miss) : 0.0;
    const double hit = c != nullptr ? static_cast<double>(c->m_hit) : 0.0;
    return (hit + 0.5) / (miss + hit + 1.0);
}

double BassRhythm::chainLogProbability(unsigned mask)
{
    if (!clean(mask)) return -1.0e300;
    double total = 0.0;
    for (int s = 0; s < Steps; ++s) {
        if (s % 4 == 0) continue;                      // a kick step is silent with certainty here
        const double p = stepProbability(mask, s);
        total += std::log2((mask >> s) & 1u ? p : 1.0 - p);
    }
    return total;
}

double BassRhythm::barProbability(unsigned mask)
{
    if (!clean(mask)) return 0.0;
    const double w = mixWeight();
    const double lookup = CorpusBassBarTotal > 0 ? static_cast<double>(barCountOf(mask)) / CorpusBassBarTotal : 0.0;
    return w * lookup + (1.0 - w) * std::exp2(chainLogProbability(mask));
}

} // namespace phos
