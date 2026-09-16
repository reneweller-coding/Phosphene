"""What is worth learning about a psytrance bass -- measured before anything is trained.

The question this module answers is not "can a bass be learned" but **"what would a learned bass
know that the pattern families of `Core/src/Composer.cpp` do not?"**. It makes three measurements,
all on the same footing, all reported as **held-out cross-entropy** rather than as a plug-in entropy
of the corpus itself:

1. **Pitch.** The entropy of the symbol stream (the stage-A alphabet, interval to the tonic) of the
   bass, against acid, lead and arp measured the same way, plus the share of notes that are the
   root. If the bass pitch stream is nearly deterministic, a pitch model buys nearly nothing.
2. **Onsets.** Which of the sixteen steps carry a note, as a binary sequence, and how much of that
   is explained by the kick alone. In this genre the kick is four on the floor and the bass lives in
   its gaps, so the conditional entropy given the kick is the honest measure of what is left to
   learn.
3. **The generator we already have.** The same two measurements on a few thousand bars that
   `Composer::composeBars` produced across the five style profiles, and -- the decisive number --
   the cross-entropy of **held-out corpus bass** under the generator read as a probability model.

**Why held-out cross-entropy and not entropy.** A plug-in entropy of a distribution over 2^16 bar
patterns estimated from ten thousand bars is biased downwards by roughly (K-1)/(2N ln 2) bits, with
K the number of patterns that ever occur; at K = 645 and N = 12 547 that is 0.037 bits, small here
but not obviously small for a richer model, and the bias grows exactly when the model gets more
expressive (Miller, "Note on the bias of information estimates", 1955; Paninski, "Estimation of
entropy and mutual information", Neural Computation 15(6), 2003). A cross-entropy measured on lines
that were never fitted has no such bias and is the number a model is actually judged by, so every
figure below that compares models is one of those, on the same held-out split ``bass.build_split``
makes.

**How the existing generator is turned into a probability model.** ``Composer::composeBars`` is a
sampler, not a density: it draws a pattern family, a figure and the form's per-beat mask. Its
density over bar patterns is therefore *measured* -- by exporting a few thousand bars of MIDI with
``phos_render --midi`` and counting -- and then smoothed, because an unsmoothed empirical
distribution gives probability zero to every corpus bar the generator cannot make and its
cross-entropy would be infinite. The smoothing is a mixture with a per-step independent model fitted
on the generator's own bars, at the mixture weight that is best **for the baseline** on the
validation split. That is deliberately generous: the baseline is handed its best case, and the
share of held-out bars it cannot produce at all is reported separately, because that share is the
honest statement of what it cannot do.

Usage:
    python Tools/train/bass_stats.py --generated <folder of phos_render MIDI exports>
"""
import argparse
import math
import os
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import bass                                                   # noqa: E402
import dataset                                                # noqa: E402

bc = dataset.bc
LOG2E = 1.0 / math.log(2.0)


# ------------------------------------------------------------------------------------ small helpers

def entropy_bits(counter):
    """Plug-in entropy of a count table, in bits. Used only where the text says plug-in."""
    n = sum(counter.values())
    if n <= 0:
        return 0.0
    return -sum((c / n) * math.log2(c / n) for c in counter.values() if c > 0)


def binary_entropy(p):
    if p <= 0.0 or p >= 1.0:
        return 0.0
    return -(p * math.log2(p) + (1 - p) * math.log2(1 - p))


def quantiles(xs):
    s = sorted(xs)
    if not s:
        return (0.0, 0.0, 0.0, 0.0)
    q = lambda f: s[min(len(s) - 1, int(f * len(s)))]          # noqa: E731
    return (sum(s) / len(s), q(0.1), q(0.5), q(0.9))


# ------------------------------------------------------------------------- reading a generated export

def read_midi_tracks(path):
    """(ppq, {track name: [(start, end, pitch, velocity)]}) -- ``read_midi`` keeps no track identity.

    ``build_corpus.read_midi`` merges every track and drops channel 10; a Phosphene export needs the
    opposite, because its parts are one track each and named after ``phos::kPartNames``. The parsing
    below is that function's, split per track and without the channel filter (the kick is on channel
    10 and is wanted here).
    """
    d = open(path, "rb").read()
    if d[:4] != b"MThd":
        return None, {}
    _fmt, ntrk, div = struct.unpack(">HHH", d[8:14])
    if div & 0x8000:
        return None, {}
    i = 8 + struct.unpack(">I", d[4:8])[0]
    out = {}
    for _ in range(ntrk):
        if d[i:i + 4] != b"MTrk":
            break
        ln = struct.unpack(">I", d[i + 4:i + 8])[0]
        j, end, t, run, name = i + 8, i + 8 + ln, 0, None, ""
        notes, open_notes = [], {}
        while j < end:
            dt, j = bc.read_vlq(d, j)
            t += dt
            st = d[j]
            if st == 0xFF:
                ty = d[j + 1]
                l, j2 = bc.read_vlq(d, j + 2)
                if ty == 0x03 and not name:
                    name = d[j2:j2 + l].decode("latin1")
                j = j2 + l
                continue
            if st in (0xF0, 0xF7):
                l, j2 = bc.read_vlq(d, j + 1)
                j = j2 + l
                continue
            if st & 0x80:
                run = st
                j += 1
            st = run
            hi = st & 0xF0
            n = 1 if hi in (0xC0, 0xD0) else 2
            a = d[j]
            b = d[j + 1] if n == 2 else 0
            j += n
            if hi == 0x90 and b > 0:
                open_notes.setdefault(a, []).append((t, b))
            elif hi == 0x80 or (hi == 0x90 and b == 0):
                if open_notes.get(a):
                    s, v = open_notes[a].pop(0)
                    notes.append((s, t, a, v))
        if notes:
            out.setdefault(name, []).extend(sorted(notes))
        i = end
    return div, out


def generated_bars(folder):
    """Bass bars and symbols of every ``phos_render --midi`` export in ``folder``.

    Returns (grids, syms, kick_grids): one 16-entry list per bar, the symbol of every bass onset
    relative to that track's tonic, and the kick's occupancy of the same bars. The tonic comes from
    the export's own key signature meta events would be the obvious route, but ``Composer`` writes
    one key per track and changes it between tracks, so the reference is estimated exactly as the
    corpus side estimates it (``estimate_key`` on the bass notes of the file), which keeps the two
    sides comparable rather than giving the generator an oracle the corpus does not get.
    """
    grids, syms, kicks = [], [], []
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith(".mid"):
            continue
        ppq, tracks = read_midi_tracks(os.path.join(folder, f))
        if not ppq or "Bass" not in tracks:
            continue
        notes = tracks["Bass"]
        kick = tracks.get("Kick", [])
        step = ppq / 4.0
        tonic, _ = bc.estimate_key(notes, f)
        pitches = [p for _s, _e, p, _v in notes]
        median = sorted(pitches)[len(pitches) // 2]
        ref = median - ((median - tonic) % 12)
        total = int(math.ceil(max(e for _s, e, _p, _v in notes) / step))
        nbars = max(1, total // 16)
        grid = [0] * (nbars * 16)
        kgrid = [0] * (nbars * 16)
        for s, _e, p, _v in notes:
            k = int(round(s / step))
            if 0 <= k < len(grid):
                if not grid[k]:
                    syms.append(max(bass.REL_MIN, min(bass.REL_MAX, p - ref)) - bass.REL_MIN)
                grid[k] = 1
        for s, _e, _p, _v in kick:
            k = int(round(s / step))
            if 0 <= k < len(kgrid):
                kgrid[k] = 1
        for b in range(nbars):
            grids.append(grid[b * 16:(b + 1) * 16])
            kicks.append(kgrid[b * 16:(b + 1) * 16])
    return grids, syms, kicks


# ------------------------------------------------------------------------------ measurement 1: pitch

def pitch_stats(lines):
    """Order-0 statistics of a list of symbol sequences."""
    pooled = Counter()
    per_line, root_share, distinct = [], [], []
    for syms in lines:
        if not syms:
            continue
        c = Counter(syms)
        pooled.update(c)
        per_line.append(entropy_bits(c))
        root_share.append(c[bass.START_SYMBOL] / len(syms))
        distinct.append(len(c))
    return {"notes": sum(pooled.values()), "lines": len(per_line),
            "pooled": entropy_bits(pooled),
            "per_line": quantiles(per_line), "root": quantiles(root_share),
            "distinct": quantiles(distinct),
            "octave_root": sum(pooled[bass.START_SYMBOL + d] for d in (-12, 0, 12, 24)) / max(1, sum(pooled.values()))}


def pitch_cross_entropy(train_lines, test_lines, order, alpha=1.0):
    """Held-out cross-entropy in bits per note of an order-0/1/2 add-alpha model over the alphabet.

    The stage-A model is Witten-Bell interpolated; ``markov.py`` has that one and it is used for the
    model comparison. Here a plain add-alpha chain is enough, because the point of this number is the
    *size* of the pitch entropy of each role, not the last hundredth of a nat.
    """
    ctx = defaultdict(Counter)
    for syms in train_lines:
        for i, s in enumerate(syms):
            key = tuple(syms[max(0, i - order):i]) if order else ()
            ctx[key][s] += 1
    v = bass.ALPHABET
    tot, n = 0.0, 0
    for syms in test_lines:
        for i, s in enumerate(syms):
            key = tuple(syms[max(0, i - order):i]) if order else ()
            c = ctx.get(key)
            if c is None:
                p = 1.0 / v
            else:
                p = (c[s] + alpha) / (sum(c.values()) + alpha * v)
            tot -= math.log2(p)
            n += 1
    return tot / max(1, n)


# ----------------------------------------------------------------------------- measurement 2: onsets

def onset_rates(grids):
    """Per-step onset probability over a list of 16-entry bars."""
    hits = [0] * 16
    for g in grids:
        for k in range(16):
            hits[k] += g[k]
    n = max(1, len(grids))
    return [h / n for h in hits]


def onset_entropies(grids):
    """The three conditionings of measurement 2, in bits per step.

    * ``H`` -- knowing only the overall density;
    * ``H_kick`` -- knowing the kick's occupancy at this step, i.e. the three classes of
      :func:`bass.kick_code` (on the kick, the sixteenth after it, the rest of the gap). This is the
      conditional entropy the plan asks for: the kick is four on the floor, so its pattern is a
      function of the step, and conditioning on the kick means conditioning on those classes;
    * ``H_step`` -- knowing the exact step, the finest position-only conditioning there is;
    * ``H_step_prev`` -- knowing the step and whether the previous step had an onset.
    """
    rates = onset_rates(grids)
    dens = sum(rates) / 16.0
    by_kick = [[0, 0], [0, 0], [0, 0]]
    by_step_prev = [[[0, 0], [0, 0]] for _ in range(16)]
    for g in grids:
        prev = 0
        for k in range(16):
            by_kick[bass.kick_code(k)][g[k]] += 1
            by_step_prev[k][prev][g[k]] += 1
            prev = g[k]
    h_kick = 0.0
    for cls in range(3):
        n = sum(by_kick[cls])
        if n:
            h_kick += (n / (16.0 * max(1, len(grids)))) * binary_entropy(by_kick[cls][1] / n)
    h_step = sum(binary_entropy(p) for p in rates) / 16.0
    h_sp = 0.0
    tot = 16.0 * max(1, len(grids))
    for k in range(16):
        for pv in (0, 1):
            n = sum(by_step_prev[k][pv])
            if n:
                h_sp += (n / tot) * binary_entropy(by_step_prev[k][pv][1] / n)
    return {"density": dens, "H": binary_entropy(dens), "H_kick": h_kick,
            "H_step": h_step, "H_step_prev": h_sp, "rates": rates}


#: Context recipes for :func:`onset_chain`. Each is a list of what the predictor of step ``s`` may
#: look at: ``"step"`` the exact step, ``"kick"`` the kick class of :func:`bass.kick_code`, and an
#: integer ``d`` the occupancy of step ``s-d`` inside the same bar (0 outside it). ``4`` is the same
#: step one beat earlier, which is the periodicity a rolling psytrance figure is made of.
ONSET_CONTEXTS = [
    ("step", ["step"]),
    ("step + kick", ["step", "kick"]),
    ("step, prev", ["step", 1]),
    ("step, prev 2", ["step", 1, 2]),
    ("step, beat before", ["step", 4]),
    ("step, prev, beat before", ["step", 1, 4]),
    ("step, prev 2, beat before", ["step", 1, 2, 4]),
    ("step, prev 3, beat before, 2 beats", ["step", 1, 2, 3, 4, 8]),
]


def onset_chain(train_grids, test_grids, context, alpha=0.5):
    """Held-out cross-entropy in bits per bar of P(onset_s | context), add-alpha per context.

    A *parametric* onset model: it stores one Bernoulli parameter per context and cannot memorise a
    bar, which is what separates it from the bar-pattern lookup. ``alpha = 0.5`` is the
    Krichevsky-Trofimov estimator, the minimax-optimal add-constant for a binary alphabet
    (Krichevsky and Trofimov, "The performance of universal encoding", IEEE Trans. Inf. Theory 27(2),
    1981); it is what keeps an unseen context from costing an infinity.
    """
    def key(grid, s):
        k = []
        for c in context:
            if c == "step":
                k.append(s)
            elif c == "kick":
                k.append(bass.kick_code(s))
            else:
                k.append(grid[s - c] if s - c >= 0 else 2)
        return tuple(k)

    counts = defaultdict(lambda: [0, 0])
    for g in train_grids:
        for s in range(16):
            counts[key(g, s)][g[s]] += 1
    tot = 0.0
    for g in test_grids:
        for s in range(16):
            c = counts.get(key(g, s), [0, 0])
            p = (c[g[s]] + alpha) / (c[0] + c[1] + 2 * alpha)
            tot -= math.log2(p)
    return tot / max(1, len(test_grids))


def mask_of(grid):
    m = 0
    for k in range(16):
        if grid[k]:
            m |= 1 << k
    return m


def step_model_logprob(rates, grid):
    """log2 P(bar) under independent per-step Bernoulli rates, clamped away from 0 and 1."""
    tot = 0.0
    for k in range(16):
        p = min(max(rates[k], 1e-4), 1 - 1e-4)
        tot += math.log2(p if grid[k] else 1 - p)
    return tot


def pattern_model(train_grids, val_grids, test_grids, backoff="uniform"):
    """Cross-entropy in bits per bar of a bar-pattern lookup, backed off so the number is finite.

    ``P(bar) = w * empirical(bar) + (1-w) * backoff(bar)``, with ``w`` chosen on ``val_grids``.

    **Why there has to be a back-off, and why the choice of it is stated rather than hidden.** A
    sampler's empirical distribution gives probability exactly zero to every bar it cannot produce,
    and its cross-entropy on a held-out set that contains such a bar is infinite. Every finite number
    in its place is the result of a smoothing choice, so two are reported and both are named:

    * ``backoff="uniform"`` -- a uniform distribution over all 2^16 bars. This bounds the worst case
      at ``16 - log2(1-w)`` bits and makes no assumption at all about the model being smoothed; it is
      the primary number, because it treats the pattern families and a learned model identically.
    * ``backoff="step"`` -- independent per-step Bernoulli rates fitted on the *same* bars the lookup
      was fitted on. Sharper where the model is right and brutal where it is wrong, because a
      generator that never places a note on the kick step gives that step a rate of zero.

    The share of held-out bars the model never produces is reported beside both, and that share needs
    no smoothing and no defence: it is the honest statement of what a model cannot do.
    """
    counts = Counter(mask_of(g) for g in train_grids)
    n = max(1, sum(counts.values()))
    rates = onset_rates(train_grids)
    uni = -16.0

    def logp_backoff(g):
        return uni if backoff == "uniform" else step_model_logprob(rates, g)

    def xent(grids, w):
        tot = 0.0
        for g in grids:
            pb = 2.0 ** logp_backoff(g)
            pe = counts.get(mask_of(g), 0) / n
            tot -= math.log2(max(w * pe + (1 - w) * pb, 1e-300))
        return tot / max(1, len(grids))

    best_w, best = 0.0, float("inf")
    for i in range(0, 100):
        w = i / 100.0
        v = xent(val_grids, w)
        if v < best:
            best, best_w = v, w
    zero = sum(1 for g in test_grids if mask_of(g) not in counts) / max(1, len(test_grids))
    return {"w": best_w, "val": best, "test": xent(test_grids, best_w),
            "step_only": sum(-step_model_logprob(rates, g) for g in test_grids) / max(1, len(test_grids)),
            "unreachable": zero, "patterns": len(counts)}


# ------------------------------------------------------------------------------------------- report

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--generated", help="folder of phos_render --midi exports (the Bass.cpp baseline)")
    ap.add_argument("--seed", type=int, default=12345)
    a = ap.parse_args()

    print("=" * 96)
    print("MEASUREMENT 1: how much is there to predict in the pitch stream")
    print("=" * 96)
    mel = dataset.load(a.root, dataset.PSY_PACKS)
    bs = bass.load(a.root)
    by_role = {r: [x["syms"] for x in mel if x["role"] == r] for r in range(3)}
    by_role[3] = [x["syms"] for x in bs]
    names = dataset.ROLES + ["bass"]
    print(f"{'role':6s} {'lines':>6s} {'notes':>8s} {'pooled H':>9s} {'per-line H (mean/50%)':>23s} "
          f"{'root share':>11s} {'root+oct':>9s} {'distinct/line':>14s}")
    for r in range(4):
        st = pitch_stats(by_role[r])
        print(f"{names[r]:6s} {st['lines']:6d} {st['notes']:8d} {st['pooled']:9.3f} "
              f"{st['per_line'][0]:11.3f} {st['per_line'][2]:11.3f} "
              f"{st['root'][0]:11.3f} {st['octave_root']:9.3f} {st['distinct'][2]:14.0f}")
    print("  (bits per note; 'pooled' pools every line of the role, 'per-line' is measured inside a line)")

    print()
    print("  held-out cross-entropy of the pitch stream, bits per note, honest split per role:")
    print(f"  {'role':6s} {'test notes':>10s} {'order 0':>8s} {'order 1':>8s} {'order 2':>8s}")
    for r in range(4):
        recs = [x for x in (bs if r == 3 else mel) if x["role"] == r]
        if not recs:
            continue
        tr, va, te = (bass.build_split(recs, seed=a.seed) if r == 3
                      else dataset.build_split(recs, seed=a.seed))
        trl = [x["syms"] for x in tr]
        tel = [x["syms"] for x in te]
        print(f"  {names[r]:6s} {sum(len(x) for x in tel):10d} "
              f"{pitch_cross_entropy(trl, tel, 0):8.3f} {pitch_cross_entropy(trl, tel, 1):8.3f} "
              f"{pitch_cross_entropy(trl, tel, 2):8.3f}")

    print()
    print("=" * 96)
    print("MEASUREMENT 2: how much is there to predict in the onset pattern")
    print("=" * 96)
    grids = []
    for r in bs:
        for b in range(r["bars"]):
            grids.append(r["grid"][b * 16:(b + 1) * 16])
    oe = onset_entropies(grids)
    print(f"corpus bass: {len(grids)} bars, onset density {oe['density']:.3f}")
    print("  onset probability per step:", " ".join(f"{p:.2f}" for p in oe["rates"]))
    print(f"  H(onset)                    {oe['H']:.4f} bit/step  ({16 * oe['H']:.2f} bit/bar)")
    print(f"  H(onset | kick class)       {oe['H_kick']:.4f} bit/step  ({16 * oe['H_kick']:.2f} bit/bar)")
    print(f"  H(onset | step)             {oe['H_step']:.4f} bit/step  ({16 * oe['H_step']:.2f} bit/bar)")
    print(f"  H(onset | step, prev onset) {oe['H_step_prev']:.4f} bit/step  ({16 * oe['H_step_prev']:.2f} bit/bar)")
    print(f"  plug-in entropy of the 16-bit bar pattern: {entropy_bits(Counter(mask_of(g) for g in grids)):.3f} bit/bar "
          f"over {len(set(mask_of(g) for g in grids))} distinct patterns (biased low, see the docstring)")

    trb, vab, teb = bass.build_split(bs, seed=a.seed)
    def bars_of(recs):
        out = []
        for r in recs:
            for b in range(r["bars"]):
                out.append(r["grid"][b * 16:(b + 1) * 16])
        return out
    gtr, gva, gte = bars_of(trb), bars_of(vab), bars_of(teb)
    print(f"  honest split: {len(gtr)} train / {len(gva)} val / {len(gte)} test bars")
    pm = pattern_model(gtr, gva, gte, "uniform")
    pms = pattern_model(gtr, gva, gte, "step")
    print(f"  held-out cross-entropy, bits per bar:")
    print(f"    uniform over 2^16                              16.000")
    print(f"    independent per step (16 parameters)           {pm['step_only']:6.3f}")
    print(f"    bar-pattern lookup + uniform back-off (w={pm['w']:.2f})  {pm['test']:6.3f}")
    print(f"    bar-pattern lookup + per-step back-off (w={pms['w']:.2f}) {pms['test']:6.3f}")
    print(f"    ({pm['patterns']} patterns seen in training, {100 * pm['unreachable']:.1f} % of test bars never seen)")
    print("  parametric onset models (one Bernoulli per context, no bar can be memorised):")
    for name, ctx in ONSET_CONTEXTS:
        print(f"    P(onset | {name:30s})  {onset_chain(gtr, gte, ctx):6.3f}")

    if not a.generated:
        return 0

    print()
    print("=" * 96)
    print("MEASUREMENT 3: what Composer::composeBars already produces")
    print("=" * 96)
    ggrids, gsyms, kgrids = generated_bars(a.generated)
    print(f"{len(ggrids)} generated bars from {a.generated}")
    goe = onset_entropies(ggrids)
    krates = onset_rates(kgrids)
    print("  kick onsets per step:      ", " ".join(f"{p:.2f}" for p in krates))
    print("  bass onsets per step:      ", " ".join(f"{p:.2f}" for p in goe["rates"]))
    print(f"  onset density {goe['density']:.3f} against the corpus's {oe['density']:.3f}")
    print(f"  H(onset)                    {goe['H']:.4f} bit/step")
    print(f"  H(onset | kick class)       {goe['H_kick']:.4f} bit/step")
    print(f"  H(onset | step)             {goe['H_step']:.4f} bit/step")
    print(f"  H(onset | step, prev onset) {goe['H_step_prev']:.4f} bit/step")
    gc = Counter(mask_of(g) for g in ggrids)
    print(f"  {len(gc)} distinct bar patterns, plug-in entropy {entropy_bits(gc):.3f} bit/bar")
    for m, n in gc.most_common(8):
        print("     ", "".join("x" if m >> k & 1 else "." for k in range(16)), f"{100 * n / len(ggrids):5.1f} %")
    gst = pitch_stats([gsyms])
    print(f"  pitch: {gst['notes']} notes, pooled H {gst['pooled']:.3f} bit, root share {gst['root'][0]:.3f}, "
          f"root+octaves {gst['octave_root']:.3f}")

    print()
    print("  THE DECISIVE NUMBER -- held-out corpus bass bars under each model, bits per bar:")
    half = len(ggrids) // 2
    gb = pattern_model(ggrids[:half], ggrids[half:], gte, "uniform")
    gbs = pattern_model(ggrids[:half], ggrids[half:], gte, "step")
    print(f"    Composer::composeBars + uniform back-off (w={gb['w']:.2f})  {gb['test']:6.3f}")
    print(f"    Composer::composeBars + per-step back-off (w={gbs['w']:.2f}) {gbs['test']:6.3f}")
    print(f"    corpus-fitted lookup + uniform back-off (w={pm['w']:.2f})   {pm['test']:6.3f}")
    print(f"    the pattern families never make {100 * gb['unreachable']:.1f} % of the held-out corpus bars "
          f"(they make {gb['patterns']} patterns in all)")
    print(f"    gap, uniform back-off:                          {gb['test'] - pm['test']:6.3f} bit/bar")

    tel = [x["syms"] for x in teb]
    trl = [x["syms"] for x in trb]
    print()
    print("  held-out corpus bass pitch under each model, bits per note:")
    print(f"    Composer::composeBars, order 0                 "
          f"{pitch_cross_entropy([gsyms], tel, 0):6.3f}")
    print(f"    corpus-fitted order 0                          {pitch_cross_entropy(trl, tel, 0):6.3f}")
    print(f"    corpus-fitted order 1                          {pitch_cross_entropy(trl, tel, 1):6.3f}")
    print(f"    corpus-fitted order 2                          {pitch_cross_entropy(trl, tel, 2):6.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
