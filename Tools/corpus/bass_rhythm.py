"""The bass **rhythm** of Phosphene: measured first, then turned into a corpus table.

The pitch round of 16.09.2026 (docs/PLAN.md, "Phase 8 (Bass)") made the bass a learned role and
learned only its *pitch*. Its onset pattern stayed the five hard-wired families of
``Core/include/phos/Patterns.h``, and the same round measured what that costs: of the held-out
corpus bars, **61.5 % cannot be produced at all**, and a learned onset model is worth about
6.4 bit/bar. This module rebuilds those numbers from scratch rather than trusting them, measures the
one candidate nobody had measured -- a bar-pattern lookup with a *parametric* fallback -- and writes
the winner into ``Core/src/CorpusTables.cpp`` as counts.

**Why a lookup with a fallback is the obvious third candidate.** The pitch round measured two models
on the same held-out split: a bar-pattern lookup at 7.532 bit/bar, which is blind to 18.6 % of test
bars, and a parametric chain at 7.604 bit/bar, which can produce every bar but cannot memorise one.
They fail on disjoint sets of bars -- the lookup on the unseen ones, the chain on the sharply
repeated ones -- so a mixture

    P(bar) = w * lookup(bar) + (1 - w) * chain(bar)

is strictly better than either unless one of them dominates everywhere. It is also exactly as easy
to *sample* as its parts: draw a coin, then either a stored pattern by its count or sixteen
Bernoulli steps. The mixture weight is fitted on the validation split and never on the test split.

**The smoothing, named rather than hidden.** The chain is smoothed with the Krichevsky-Trofimov
estimator -- add 1/2 per binary outcome, the minimax-optimal add-constant for a binary alphabet
(Krichevsky and Trofimov, "The performance of universal encoding", IEEE Trans. Inf. Theory 27(2),
1981) -- so an unseen context costs a bounded number of bits instead of an infinity. The lookup has
no smoothing of its own; the chain *is* its smoothing, which is the whole point of the mixture.
Plug-in entropies of a distribution over 2^16 patterns estimated from ten thousand bars are biased
low by roughly (K-1)/(2N ln 2) bits (Miller, "Note on the bias of information estimates", 1955;
Paninski, "Estimation of entropy and mutual information", Neural Computation 15(6), 2003), so every
number that compares models below is a held-out cross-entropy and the plug-in entropies are only
ever reported as descriptions of a sample.

**The kick step.** ``Kick::constrainTail`` and ``Kick::setPhaseTarget`` both switch themselves off
when the slot is zero or less (``Core/src/Kick.cpp``), so a bass onset landing exactly on a kick
would silently disable the tail limit and the phase lock. The generator therefore works in the
subspace of bars with no onset on steps 0, 4, 8, 12, and this module reports what that subspace
costs -- its share of the corpus and its cross-entropy -- instead of leaving it as an assumption.

Usage::

    python Tools/corpus/bass_rhythm.py --root M:/Midi
    python Tools/corpus/bass_rhythm.py --root M:/Midi --generated out/bassmidi
    python Tools/corpus/bass_rhythm.py --root M:/Midi --emit Core/src/CorpusTables.cpp
"""
import argparse
import math
import os
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_TRAIN = os.path.join(os.path.dirname(_HERE), "train")

#: Sixteenth steps a four-on-the-floor kick occupies.
KICK_STEPS = (0, 4, 8, 12)

#: The context of the parametric chain: the exact step plus the occupancy of five earlier steps.
#: ``1``/``2``/``3`` are the three sixteenths before, ``4`` the same step one beat earlier and ``8``
#: two beats earlier -- the periodicities a rolling psytrance figure is made of. A step outside the
#: bar is coded 2, so the first steps of a bar have contexts of their own instead of borrowing the
#: previous bar's.
CHAIN_CONTEXT = (1, 2, 3, 4, 8)

KT_ALPHA = 0.5   #: Krichevsky-Trofimov add-constant (see the module docstring).


def _import_bass():
    """Imports ``Tools/train/bass.py`` lazily.

    Lazily, because ``bass`` imports ``dataset`` which imports ``build_corpus`` -- and
    ``build_corpus`` imports *this* module to emit the table. Doing it inside a call keeps that from
    being a circular import at module level.
    """
    if _TRAIN not in sys.path:
        sys.path.insert(0, _TRAIN)
    import bass                                                # noqa: E402
    return bass


# ------------------------------------------------------------------------------------ bar plumbing

def mask_of(grid):
    """16-bit mask of a 16-entry bar, step 0 in the least significant bit."""
    m = 0
    for k in range(16):
        if grid[k]:
            m |= 1 << k
    return m


def grid_of(mask):
    """Inverse of :func:`mask_of`."""
    return [(mask >> k) & 1 for k in range(16)]


def show(mask):
    """A bar as sixteen characters, for a report."""
    return "".join("x" if (mask >> k) & 1 else "." for k in range(16))


def clean(mask):
    """True when no onset sits on a kick step -- the subspace the generator may use."""
    return all(not ((mask >> k) & 1) for k in KICK_STEPS)


def bars_of(recs):
    """Every bar of every line as a 16-entry list."""
    out = []
    for r in recs:
        for b in range(r["bars"]):
            out.append(r["grid"][b * 16:(b + 1) * 16])
    return out


def lines_of(recs):
    """Every line as a list of its bar masks (bar identity kept, unlike :func:`bars_of`)."""
    return [[mask_of(r["grid"][b * 16:(b + 1) * 16]) for b in range(r["bars"])] for r in recs]


def melodic_grids(recs, role):
    """Bars of a melodic role, built from the ``steps`` list ``dataset`` already stores."""
    out = []
    for r in recs:
        if r["role"] != role:
            continue
        grid = [0] * (r["bars"] * 16)
        for s in r["steps"]:
            if 0 <= s < len(grid):
                grid[s] = 1
        for b in range(r["bars"]):
            out.append(grid[b * 16:(b + 1) * 16])
    return out


def entropy_bits(counter):
    n = sum(counter.values())
    if n <= 0:
        return 0.0
    return -sum((c / n) * math.log2(c / n) for c in counter.values() if c > 0)


# --------------------------------------------------------------------------------------- the models

def chain_key(grid, s, context=CHAIN_CONTEXT):
    """Context key of step @p s: the step itself and the occupancy of the earlier steps."""
    k = [s]
    for d in context:
        k.append(grid[s - d] if s - d >= 0 else 2)
    return tuple(k)


def chain_counts(grids, context=CHAIN_CONTEXT):
    """Per-context [misses, hits] counts of the parametric chain."""
    counts = defaultdict(lambda: [0, 0])
    for g in grids:
        for s in range(16):
            counts[chain_key(g, s, context)][g[s]] += 1
    return counts


def chain_logp(counts, grid, context=CHAIN_CONTEXT, alpha=KT_ALPHA):
    """log2 P(bar) under the parametric chain."""
    tot = 0.0
    for s in range(16):
        c = counts.get(chain_key(grid, s, context), (0, 0))
        tot += math.log2((c[grid[s]] + alpha) / (c[0] + c[1] + 2 * alpha))
    return tot


def lookup_counts(grids):
    """Bar-pattern counts."""
    return Counter(mask_of(g) for g in grids)


def lookup_p(counts, total, grid):
    """Empirical P(bar) of the lookup (exactly zero for a pattern never seen)."""
    return counts.get(mask_of(grid), 0) / max(1, total)


def mixture_xent(lookup, total, counts, grids, w, context=CHAIN_CONTEXT):
    """Cross-entropy in bits per bar of ``w * lookup + (1-w) * chain``."""
    tot = 0.0
    for g in grids:
        p = w * lookup_p(lookup, total, g) + (1.0 - w) * 2.0 ** chain_logp(counts, g, context)
        tot -= math.log2(max(p, 1e-300))
    return tot / max(1, len(grids))


def fit_mixture(train, val, context=CHAIN_CONTEXT, steps=200):
    """Fits the mixture weight on the validation split and returns (w, counts, lookup, total)."""
    counts = chain_counts(train, context)
    lookup = lookup_counts(train)
    total = sum(lookup.values())
    best_w, best = 0.0, float("inf")
    for i in range(steps + 1):
        w = i / steps
        v = mixture_xent(lookup, total, counts, val, w, context)
        if v < best:
            best, best_w = v, w
    return best_w, counts, lookup, total, best


def uniform_backoff_xent(lookup, total, grids, w):
    """The pitch round's baseline: the lookup backed off to a uniform distribution over 2^16 bars."""
    tot = 0.0
    for g in grids:
        p = w * lookup_p(lookup, total, g) + (1.0 - w) * 2.0 ** -16.0
        tot -= math.log2(max(p, 1e-300))
    return tot / max(1, len(grids))


# ---------------------------------------------------------------------------------------- the table

def build_tables(root="M:/Midi", seed=12345, packs=None):
    """The counts that go into ``CorpusTables.cpp``, fitted on the **training split only**.

    Fitted on the training split and not on everything, so that every number this module prints and
    every number a later round measures against it come from the same split: a table fitted on the
    test bars would make its own cross-entropy meaningless.

    @return a dict with ``patterns`` (a sorted list of (mask, count) over the clean subspace),
            ``chain`` (a sorted list of (key, miss, hit)), ``weight`` (the fitted mixture weight in
            per-mille), ``bars`` and ``lines``.
    """
    bass = _import_bass()
    recs = bass.load(root, packs)
    tr, va, _te = bass.build_split(recs, seed=seed)
    gtr = [g for g in bars_of(tr) if clean(mask_of(g))]
    gva = [g for g in bars_of(va) if clean(mask_of(g))]
    w, counts, lookup, total, _v = fit_mixture(gtr, gva)
    patterns = sorted(lookup.items())
    chain = sorted((key_code(k), c[0], c[1]) for k, c in counts.items())
    return {"patterns": patterns, "chain": chain, "weight": int(round(w * 1000.0)),
            "bars": len(gtr), "lines": len(tr), "total": total}


def repetition_stats(recs):
    """How much a corpus line repeats itself: the two numbers the phrase generator is built on.

    @return (home share, repeat-previous share): how often a bar equals its line's most common bar,
            and how often it equals the bar before it. Only lines of at least two bars count.
    """
    lines = [l for l in lines_of(recs) if len(l) >= 2]
    nbars = sum(len(l) for l in lines)
    pairs = sum(len(l) - 1 for l in lines)
    modal = sum(Counter(l).most_common(1)[0][1] for l in lines)
    same = sum(sum(1 for i in range(1, len(l)) if l[i] == l[i - 1]) for l in lines)
    return modal / max(1, nbars), same / max(1, pairs)


def emit_lines(root="M:/Midi", seed=12345, packs=None):
    """The C++ block ``build_corpus.write_tables`` splices into ``Core/src/CorpusTables.cpp``."""
    bass = _import_bass()
    t = build_tables(root, seed, packs)
    recs = bass.load(root, packs)
    home, repeat = repetition_stats(recs)
    pat = ", ".join(f"{{{m},{n}}}" for m, n in t["patterns"])
    ch = ", ".join(f"{{{k},{a},{b}}}" for k, a, b in t["chain"])
    return [
        "",
        "// ---------------------------------------------------------------------------- bass rhythm",
        "// The bass onset model of 16.09.2026 (Tools/corpus/bass_rhythm.py). Two count tables and one",
        "// mixture weight, fitted on the TRAINING split of the bass corpus alone -- so the held-out",
        "// numbers in docs/PLAN.md describe the table that is in this binary, not a cousin of it -- and",
        "// only over bars with no onset on a kick step (Corpus.h, BassRhythm).",
        "namespace {",
        f"const CorpusBassBar k_bass_bars[] = {{ {pat or '{0,0}'} }};",
        f"const CorpusBassStep k_bass_steps[] = {{ {ch or '{0,0,0}'} }};",
        "} // namespace",
        "",
        "const CorpusBassBar* const kCorpusBassBars = k_bass_bars;",
        f"const int kNumCorpusBassBars = {len(t['patterns'])};",
        f"const uint32_t kCorpusBassBarTotal = {t['total']};",
        "const CorpusBassStep* const kCorpusBassSteps = k_bass_steps;",
        f"const int kNumCorpusBassSteps = {len(t['chain'])};",
        f"const int kCorpusBassMixPerMille = {t['weight']};",
        f"const int kCorpusBassHomePerMille = {int(round(home * 1000.0))};",
        f"const int kCorpusBassRepeatPerMille = {int(round(repeat * 1000.0))};",
        f"const int kCorpusBassLines = {t['lines']};",
        f"const int kCorpusBassTrainBars = {t['bars']};",
    ]


def key_code(key):
    """Packs a chain context into one integer: step + 16 * (d1 + 3 * (d2 + 3 * (...))).

    Base three per neighbour, because a neighbour is absent (2), silent (0) or struck (1). The
    largest code is 16 * 3^5 - 1 = 3887, so the key fits in a ``uint16_t`` and the C++ side can find
    it by binary search, exactly as ``CorpusGram`` is found.
    """
    code = 0
    for d in reversed(key[1:]):
        code = code * 3 + d
    return code * 16 + key[0]


# -------------------------------------------------------------------------------------- the report

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--generated", help="folder of phos_render --midi exports")
    ap.add_argument("--seed", type=int, default=12345)
    a = ap.parse_args()
    bass = _import_bass()

    recs = bass.load(a.root)
    allbars = bars_of(recs)
    allmask = [mask_of(g) for g in allbars]
    c = Counter(allmask)
    print("=" * 100)
    print("1  WHAT THE CORPUS PLAYS")
    print("=" * 100)
    print(f"bass: {len(recs)} lines, {len(allbars)} bars, {len(c)} distinct bar patterns, "
          f"plug-in entropy {entropy_bits(c):.3f} bit/bar")
    for m, n in c.most_common(12):
        print(f"     {show(m)}  {100 * n / len(allmask):6.2f} %{'   (clean)' if clean(m) else ''}")
    print(f"  bars with no onset on a kick step: {100 * sum(1 for m in allmask if clean(m)) / len(allmask):.1f} %")
    cc = Counter(m for m in allmask if clean(m))
    print(f"  the clean subspace alone: {len(cc)} patterns, plug-in entropy {entropy_bits(cc):.3f} bit/bar")

    import dataset                                             # noqa: E402
    mel = dataset.load(a.root, dataset.PSY_PACKS)
    print()
    print("  the same measurement per role (the corpus carries no style label -- MODEL_FORMAT 3):")
    print(f"  {'role':6s} {'bars':>7s} {'patterns':>9s} {'plug-in H':>10s} {'density':>8s} {'clean':>7s}")
    for r, name in enumerate(dataset.ROLES):
        g = melodic_grids(mel, r)
        if not g:
            continue
        mk = [mask_of(x) for x in g]
        print(f"  {name:6s} {len(g):7d} {len(set(mk)):9d} {entropy_bits(Counter(mk)):10.3f} "
              f"{sum(sum(x) for x in g) / (16.0 * len(g)):8.3f} {100 * sum(1 for m in mk if clean(m)) / len(mk):6.1f}%")
    print(f"  {'bass':6s} {len(allbars):7d} {len(c):9d} {entropy_bits(c):10.3f} "
          f"{sum(sum(x) for x in allbars) / (16.0 * len(allbars)):8.3f} "
          f"{100 * sum(1 for m in allmask if clean(m)) / len(allmask):6.1f}%")
    print()
    print("  per pack (the nearest thing to a style the corpus has):")
    by_pack = defaultdict(list)
    for r in recs:
        p = r["pack"]
        name = bass.BASS_PACKS[p] if isinstance(p, int) and 0 <= p < len(bass.BASS_PACKS) else str(p)
        for b in range(r["bars"]):
            by_pack[name].append(mask_of(r["grid"][b * 16:(b + 1) * 16]))
    for pack, mks in sorted(by_pack.items(), key=lambda kv: -len(kv[1])):
        print(f"    {pack[:44]:44s} {len(mks):6d} bars  {len(set(mks)):5d} patterns  "
              f"H {entropy_bits(Counter(mks)):5.3f}  top {show(Counter(mks).most_common(1)[0][0])}")

    print()
    print("2  HOW MUCH A LINE REPEATS ITSELF (why the generator must draw a figure, not a bar)")
    print("=" * 100)
    lines = [l for l in lines_of(recs) if len(l) >= 2]
    dist = [len(set(l)) for l in lines]
    same_prev = sum(sum(1 for i in range(1, len(l)) if l[i] == l[i - 1]) for l in lines)
    pairs = sum(len(l) - 1 for l in lines)
    modal = sum(Counter(l).most_common(1)[0][1] for l in lines)
    nbars = sum(len(l) for l in lines)
    dist.sort()
    print(f"  {len(lines)} lines of at least two bars, {nbars} bars")
    print(f"  distinct bar patterns per line: mean {sum(dist) / len(dist):.2f}, "
          f"median {dist[len(dist) // 2]}, 90th percentile {dist[int(0.9 * len(dist))]}")
    print(f"  a bar equals the bar before it in {100 * same_prev / pairs:.1f} % of the pairs")
    print(f"  a bar equals its line's most common bar in {100 * modal / nbars:.1f} % of the bars")

    print()
    print("3  HELD-OUT CROSS-ENTROPY OF THE CANDIDATES (bits per bar, honest split over loop groups)")
    print("=" * 100)
    tr, va, te = bass.build_split(recs, seed=a.seed)
    gtr, gva, gte = bars_of(tr), bars_of(va), bars_of(te)
    print(f"  split: {len(gtr)} train / {len(gva)} val / {len(gte)} test bars")
    for label, (Tr, Va, Te) in (("all bars", (gtr, gva, gte)),
                                ("clean subspace only", ([g for g in gtr if clean(mask_of(g))],
                                                         [g for g in gva if clean(mask_of(g))],
                                                         [g for g in gte if clean(mask_of(g))]))):
        print()
        print(f"  --- {label}: {len(Tr)} / {len(Va)} / {len(Te)} bars")
        lk = lookup_counts(Tr)
        tot = sum(lk.values())
        unseen = sum(1 for g in Te if mask_of(g) not in lk) / max(1, len(Te))
        best_w, best = 0.0, float("inf")
        for i in range(101):
            w = i / 100.0
            v = uniform_backoff_xent(lk, tot, Va, w)
            if v < best:
                best, best_w = v, w
        print(f"    uniform over 2^16                                    16.000")
        print(f"    bar-pattern lookup + uniform back-off (w={best_w:.2f})       "
              f"{uniform_backoff_xent(lk, tot, Te, best_w):6.3f}   ({len(lk)} patterns, "
              f"{100 * unseen:.1f} % of test bars never seen)")
        for ctx in ((1,), (1, 2, 3), (1, 4), (1, 2, 3, 4, 8)):
            cn = chain_counts(Tr, ctx)
            x = -sum(chain_logp(cn, g, ctx) for g in Te) / max(1, len(Te))
            print(f"    parametric chain, step + {str(ctx):16s}            {x:6.3f}   ({len(cn)} contexts)")
        w, cn, lk2, tot2, valx = fit_mixture(Tr, Va)
        print(f"    HYBRID lookup + parametric chain (w={w:.3f})           "
              f"{mixture_xent(lk2, tot2, cn, Te, w):6.3f}   (val {valx:.3f})")

    if not a.generated:
        return 0

    print()
    print("4  WHAT THE GENERATOR PLAYS")
    print("=" * 100)
    sys.path.insert(0, _TRAIN)
    import bass_stats                                          # noqa: E402
    ggrids, _gsyms, _kg = bass_stats.generated_bars(a.generated)
    gm = [mask_of(g) for g in ggrids]
    gc = Counter(gm)
    print(f"  {len(ggrids)} generated bars, {len(gc)} distinct patterns, plug-in entropy "
          f"{entropy_bits(gc):.3f} bit/bar, density {sum(sum(g) for g in ggrids) / (16.0 * len(ggrids)):.3f}")
    for m, n in gc.most_common(10):
        print(f"     {show(m)}  {100 * n / len(gm):6.2f} %")
    lk = lookup_counts(ggrids)
    tot = sum(lk.values())
    reach = sum(1 for g in gte if mask_of(g) in lk) / max(1, len(gte))
    print(f"  the generator can produce {100 * reach:.1f} % of the {len(gte)} held-out corpus bars")
    print(f"  {100 * sum(1 for m in gm if clean(m)) / len(gm):.1f} % of the generated bars are clean")
    # The generator as a probability model of *real* bass bars. The back-off weight is fitted on the
    # corpus **validation** split and the number is read off the corpus test split: fitting it on a
    # second half of the generator's own output, as an earlier draft did, makes the lookup look
    # perfect on material it has by construction seen and then charges it 1e-300 for every test bar,
    # which is not a measurement of anything.
    best_w, best = 0.0, float("inf")
    for i in range(101):
        w = i / 100.0
        v = uniform_backoff_xent(lk, tot, gva, w)
        if v < best:
            best, best_w = v, w
    print(f"  as a model of held-out corpus bars, + uniform back-off (w={best_w:.2f} on the corpus "
          f"validation split): {uniform_backoff_xent(lk, tot, gte, best_w):.3f} bit/bar")

    # Does the roll still roll? One export at a time, so that "a track's own bars" means one render
    # and not the whole folder. The corpus number this is held against is section 2's 86.8 %.
    import shutil
    import tempfile
    modal, files = 0.0, 0
    by_file = {}
    for f in sorted(os.listdir(a.generated)):
        if not f.lower().endswith(".mid"):
            continue
        tmp = tempfile.mkdtemp()
        try:
            shutil.copy(os.path.join(a.generated, f), tmp)
            one, _s, _k = bass_stats.generated_bars(tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        if not one:
            continue
        c1 = Counter(mask_of(g) for g in one)
        by_file[f] = (len(one), len(c1), c1.most_common(1)[0][1] / len(one))
        modal += by_file[f][2]
        files += 1
    if files:
        print(f"  per render: {modal / files * 100:.1f} % of a render's bars are its own commonest bar "
              f"(corpus lines: 86.8 %), {sum(v[1] for v in by_file.values()) / files:.1f} patterns per render")
    return 0


if __name__ == "__main__":
    sys.exit(main())
