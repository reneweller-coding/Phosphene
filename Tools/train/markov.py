"""The stage-A baseline: the order-2 Witten-Bell model of Core/src/Corpus.cpp, refit and measured.

Phase 8 is only worth shipping if it beats what is already in the program, so the baseline has to be
the model that is already in the program and not a straw man. This is ``phos::PitchModel``
(``Core/src/Corpus.cpp``) reimplemented in Python: order 0 add-one smoothed, order 1 interpolated
with order 0 and order 2 with order 1 by the Witten-Bell escape (Witten and Bell, "The zero-frequency
problem: estimating the probabilities of novel events in adaptive text compression", IEEE Trans.
Information Theory 37(4), 1991; compared for music by Begleiter, El-Yaniv and Yona, "On prediction
using variable order Markov models", JAIR 22, 2004).

Two numbers come out of this module and both belong in the report:

* the **fair** baseline, counted from the training split only -- what stage A would know about a loop
  it has never seen;
* the **shipped** baseline, counted from every line including the held-out ones, which is what
  ``CorpusTables.cpp`` contains today. It is contaminated by construction and is printed only to
  show how much of stage A's apparent quality is the held-out data being inside its own tables.

Counting follows ``build_corpus.analyse``: the n-grams of a loop are taken cyclically, because a
loop repeats. Evaluation is linear and starts from the sampler's context (symbol of interval 0,
twice), which is what ``drawPitches`` does in ``Core/src/Melody.cpp``.
"""
import math
from collections import Counter

from dataset import ALPHABET, ROLES, START_SYMBOL

A = ALPHABET


def count(records, role):
    """Cyclic order-0/1/2 counts of one role, as build_corpus collects them."""
    uni, bi, tri = Counter(), Counter(), Counter()
    for rec in records:
        if rec["role"] != role:
            continue
        s = rec["syms"]
        n = len(s)
        for i in range(n):
            a, b, c = s[i - 2], s[i - 1], s[i]
            uni[c] += 1
            bi[(b, c)] += 1
            tri[(a, b, c)] += 1
    return uni, bi, tri


class MarkovModel:
    """Dense P(c | a, b) for one role, built exactly like phos::PitchModel."""

    def __init__(self, records, role):
        uni, bi, tri = count(records, role)
        n0 = sum(uni.values())
        self.p0 = [(uni.get(c, 0) + 1.0) / (n0 + A) for c in range(A)]
        self.p1 = [[0.0] * A for _ in range(A)]
        for b in range(A):
            total = sum(bi.get((b, c), 0) for c in range(A))
            types = sum(1 for c in range(A) if bi.get((b, c), 0) > 0)
            for c in range(A):
                k = bi.get((b, c), 0)
                self.p1[b][c] = (k + types * self.p0[c]) / (total + types) if total > 0 else self.p0[c]
        self.p2 = {}
        contexts = {(a, b) for (a, b, _c) in tri}
        for (a, b) in contexts:
            total = sum(tri.get((a, b, c), 0) for c in range(A))
            types = sum(1 for c in range(A) if tri.get((a, b, c), 0) > 0)
            row = [0.0] * A
            for c in range(A):
                k = tri.get((a, b, c), 0)
                row[c] = (k + types * self.p1[b][c]) / (total + types) if total > 0 else self.p1[b][c]
            self.p2[(a, b)] = row

    def prob(self, a, b, c, order=2):
        if order == 0:
            return self.p0[c]
        if order == 1:
            return self.p1[b][c]
        row = self.p2.get((a, b))
        return row[c] if row is not None else self.p1[b][c]

    def row(self, a, b):
        row = self.p2.get((a, b))
        return row if row is not None else self.p1[b]


def nll(records, test, per_role=False, order=2):
    """Negative log likelihood per token in nats of the model of ``order`` fit on ``records``.

    ``records`` are the training lines, ``test`` the held-out ones. The evaluation is the one the
    neural models get: linear over the line, starting from the context (START_SYMBOL, START_SYMBOL).
    Order 2 is what ``phos::PitchModel`` uses; orders 1 and 0 come out of the same tables and are
    reported next to it, because on a corpus this small the higher order is not automatically the
    better predictor and the plan wants that measured rather than assumed.
    """
    models = {r: MarkovModel(records, r) for r in range(len(ROLES))}
    total, n = 0.0, 0
    byrole = {r: [0.0, 0] for r in range(len(ROLES))}
    for rec in test:
        m = models[rec["role"]]
        a = b = START_SYMBOL
        for c in rec["syms"]:
            p = max(m.prob(a, b, c, order), 1e-12)
            total -= math.log(p)
            n += 1
            byrole[rec["role"]][0] -= math.log(p)
            byrole[rec["role"]][1] += 1
            a, b = b, c
    if per_role:
        return total / max(1, n), {ROLES[r]: (v[0] / v[1] if v[1] else float("nan")) for r, v in byrole.items()}
    return total / max(1, n)


def unigram_nll(records, test):
    """A floor to measure against: the role's order-0 distribution, add-one smoothed."""
    p0 = {}
    for r in range(len(ROLES)):
        uni, _bi, _tri = count(records, r)
        n0 = sum(uni.values())
        p0[r] = [(uni.get(c, 0) + 1.0) / (n0 + A) for c in range(A)]
    total, n = 0.0, 0
    for rec in test:
        for c in rec["syms"]:
            total -= math.log(max(p0[rec["role"]][c], 1e-12))
            n += 1
    return total / max(1, n)
