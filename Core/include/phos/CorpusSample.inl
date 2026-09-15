/**
 * @file CorpusSample.inl
 * @brief Exact constrained sampling from an order-2 Markov model (Pachet and Roy 2011), with
 *        per-position weights.
 *
 * States are pairs (a, b) of the last two symbols. Backwards, beta_i(a, b) is the total probability of
 * completing positions i..n-1 inside their allowed sets from state (a, b); beta_n = 1. Forwards, the
 * symbol at position i is drawn with probability proportional to P(c | a, b) * [c allowed at i] *
 * beta_{i+1}(b, c), which is the model's distribution conditioned on every constraint being met.
 * Each beta table is normalised to its maximum as it is built, which leaves the forward weights
 * unchanged and keeps long sequences from underflowing.
 *
 * Only pairs that the allowed sets can reach are computed, so the cost is n times the product of three
 * allowed-set sizes: with a scale-and-ambitus constraint (about ten allowed of 37) a 32-note line costs
 * some 30 000 model look-ups.
 */
#pragma once
#include <algorithm>
#include <cmath>

namespace phos {

template <class Model, class Uniform>
bool sampleConstrained(const Model& model, const std::vector<std::vector<uint8_t>>& allowed, int start2, int start1,
                       double temperature, Uniform&& uniform, std::vector<int>& out)
{
    const int A = model.alphabet();
    const int n = static_cast<int>(allowed.size());
    out.clear();
    if (n == 0) return true;
    const double expo = temperature > 0.0 ? 1.0 / temperature : 1.0;
    // An entry of `allowed` is a relative weight, not only a flag: 0 forbids the symbol, any other
    // value multiplies the model's probability at that position. A position whose entries are all the
    // same value (the plain allowed sets, every entry 1) is unchanged, because a constant factor at one
    // position cancels in both normalisations below. The weights carry the colour of the energy arc
    // (Melody.h): the flat second and the augmented second get more weight where the arc is high.
    auto weight = [&](int i, int a, int b, int c) {
        const double p = model.prob(a, b, c);
        const double w = expo == 1.0 ? p : std::pow(p, expo);
        return i < 0 ? w : w * allowed[static_cast<size_t>(i)][static_cast<size_t>(c)];
    };
    // Allowed symbols per position as index lists.
    std::vector<std::vector<int>> lists(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < A; ++c)
            if (allowed[static_cast<size_t>(i)][static_cast<size_t>(c)]) lists[static_cast<size_t>(i)].push_back(c);
    // The symbols that can stand at position j, including the two context positions -1 and -2.
    const std::vector<int> ctx1{ start1 }, ctx2{ start2 };
    auto at = [&](int j) -> const std::vector<int>& { return j >= 0 ? lists[static_cast<size_t>(j)] : (j == -1 ? ctx1 : ctx2); };
    // beta[i] over pairs (a, b) = (x_{i-2}, x_{i-1}): i runs 1..n, beta[n] = 1. Only reachable pairs
    // are filled, which takes the cost from n A^3 to n times the product of three allowed-set sizes.
    std::vector<std::vector<double>> beta(static_cast<size_t>(n + 1), std::vector<double>(static_cast<size_t>(A * A), 0.0));
    std::fill(beta[static_cast<size_t>(n)].begin(), beta[static_cast<size_t>(n)].end(), 1.0);
    for (int i = n - 1; i >= 1; --i) {
        const std::vector<double>& next = beta[static_cast<size_t>(i + 1)];
        std::vector<double>& cur = beta[static_cast<size_t>(i)];
        const std::vector<int>& here = lists[static_cast<size_t>(i)];
        double mx = 0.0;
        for (int a : at(i - 2)) {
            for (int b : at(i - 1)) {
                double s = 0.0;
                for (int c : here) s += weight(i, a, b, c) * next[static_cast<size_t>(b * A + c)];
                cur[static_cast<size_t>(a * A + b)] = s;
                mx = std::max(mx, s);
            }
        }
        if (mx <= 0.0) return false;
        for (double& v : cur) v /= mx;
    }
    int a = start2, b = start1;
    std::vector<double> w(static_cast<size_t>(A));
    for (int i = 0; i < n; ++i) {
        double total = 0.0;
        std::fill(w.begin(), w.end(), 0.0);
        for (int c : lists[static_cast<size_t>(i)]) {
            const double x = weight(i, a, b, c) * beta[static_cast<size_t>(i + 1)][static_cast<size_t>(b * A + c)];
            w[static_cast<size_t>(c)] = x;
            total += x;
        }
        if (total <= 0.0) { out.clear(); return false; }
        double r = uniform() * total;
        int c = -1;
        for (int cand : lists[static_cast<size_t>(i)]) {
            if (w[static_cast<size_t>(cand)] <= 0.0) continue;
            c = cand;   // the last symbol with weight takes what rounding leaves over
            if (r < w[static_cast<size_t>(cand)]) break;
            r -= w[static_cast<size_t>(cand)];
        }
        out.push_back(c);
        a = b;
        b = c;
    }
    return true;
}

} // namespace phos
