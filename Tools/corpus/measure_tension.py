"""Measures the tension curve of the MIDI corpus: harmonic stability against position in the phrase.

**Why this script exists.** A review of 16.09.2026 proposed a schedule for the melodic layer over an
eight-bar phrase -- bars 1 to 4 stable, bars 5 and 6 rising, bar 7 the peak, bar 8 resolving onto the
tonic or the fifth -- and grounded it in Lerdahl's stability hierarchy (*Tonal Pitch Space*, Oxford
University Press 2001). The hierarchy is a theory of pitch space and is not in question here. The
schedule is a design assertion about *psytrance*, and it is measurable: the corpus holds thousands of
melodic lines whose symbols are already intervals to the tonic (Tools/corpus/build_corpus.py,
Tools/train/dataset.py), so the distribution of stability against position in the phrase can simply
be counted. This script counts it. What Phosphene then implements is the measured curve, not the
proposed one (docs/rounds/2026-09.md, the block of 16.09.2026).

**The stability measure.** Lerdahl's basic space for a minor tonic is a set of nested levels: the
octave level holds the tonic, the fifth level adds the fifth, the triadic level adds the third, the
diatonic level adds the remaining scale degrees and the chromatic level holds all twelve pitch
classes. A pitch class's *depth* is the number of levels that contain it, and its instability is
``5 - depth``: 0 for the tonic, 1 for the fifth, 2 for the minor third, 3 for the other diatonic
degrees, 4 for a chromatic tone -- the flat second, the tritone and the raised second among them.
That is a strict refinement of the three levels the review names (its level 0 = {0, 7} here 0 and 1,
its level 1 = {2, 3} here 2 and 3, its level 2 = {1, 6, aug 2} here 4), so the two agree on order and
this one has finer resolution. Both are reported.

**Positions.** A line's onset step is a sixteenth from the start of the loop, so the bar is
``step // 16``, the bar within a four- or eight-bar phrase is that modulo 4 or 8, the beat within the
bar is ``(step % 16) // 4`` and the sixteenth within the beat is ``step % 4``.

**Duplicates.** The packs resell the same loop many times over; counting a loop once per copy would
report the vendor's catalogue rather than the genre. Every measurement below runs on the deduplicated
corpus (``dataset.dedupe``, union-find over near duplicates up to transposition), and the confidence
intervals bootstrap over *lines*, not over notes, because the notes of one loop are not independent.

Usage:
    python Tools/corpus/measure_tension.py --root M:/Midi
    python Tools/corpus/measure_tension.py --root M:/Midi --json out.json
"""
import argparse
import importlib.util
import json
import math
import os
import random
import sys
from collections import defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_TRAIN = os.path.normpath(os.path.join(_HERE, "..", "train"))


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


ds = _load("phos_dataset", os.path.join(_TRAIN, "dataset.py"))

# Lerdahl's basic space over a minor tonic: which levels contain each pitch class.
_OCTAVE = {0}
_FIFTH = {0, 7}
_TRIADIC = {0, 3, 7}
_DIATONIC = {0, 2, 3, 5, 7, 8, 10}
_CHROMATIC = set(range(12))
LEVELS = [_OCTAVE, _FIFTH, _TRIADIC, _DIATONIC, _CHROMATIC]


def instability(pc):
    """Lerdahl instability of a pitch class above the tonic: 5 minus its depth in the basic space."""
    return 5 - sum(1 for lv in LEVELS if pc % 12 in lv)


def review_level(pc):
    """The three-level reading the review proposed: 0 tonic/fifth, 1 minor third/second, 2 the rest."""
    pc %= 12
    if pc in (0, 7):
        return 0
    if pc in (2, 3):
        return 1
    return 2


def mean(xs):
    return sum(xs) / len(xs) if xs else float("nan")


def bootstrap(per_line, key, draws=2000, seed=7):
    """95 % interval of a per-position mean, resampling whole lines (notes inside a loop correlate).

    ``per_line[i][key]`` is (sum, count) of the position for line i; lines that never hit the
    position contribute nothing, which is what makes the interval widen where the position is rare.
    """
    rng = random.Random(seed)
    pool = [rec[key] for rec in per_line if key in rec]
    if len(pool) < 4:
        return (float("nan"), float("nan"))
    n = len(pool)
    out = []
    for _ in range(draws):
        s = c = 0.0
        for _ in range(n):
            a, b = pool[rng.randrange(n)]
            s += a
            c += b
        out.append(s / c if c else float("nan"))
    out.sort()
    return (out[int(0.025 * draws)], out[int(0.975 * draws)])


def measure(records, min_bars=4):
    """Per role: instability by bar in 4, bar in 8, beat, and sixteenth, with per-line sums."""
    out = {}
    for role_id, role in enumerate(ds.ROLES):
        lines = [r for r in records if r["role"] == role_id and r["bars"] >= min_bars]
        per_line = []
        totals = defaultdict(lambda: [0.0, 0])
        rev = defaultdict(lambda: [0.0, 0])
        for rec in lines:
            here = {}
            for sym, step in zip(rec["syms"], rec["steps"]):
                pc = (sym + ds.REL_MIN) % 12
                v = instability(pc)
                keys = [("bar4", (step // 16) % 4), ("bar8", (step // 16) % 8),
                        ("beat", (step % 16) // 4), ("sixteenth", step % 4)]
                for k in keys:
                    totals[k][0] += v
                    totals[k][1] += 1
                    rev[k][0] += review_level(pc)
                    rev[k][1] += 1
                    cell = here.setdefault(k, [0.0, 0])
                    cell[0] += v
                    cell[1] += 1
            if here:
                per_line.append({k: tuple(v) for k, v in here.items()})
        out[role] = {
            "lines": len(lines),
            "notes": sum(v[1] for k, v in totals.items() if k[0] == "bar4"),
            "mean": {f"{k[0]}:{k[1]}": (v[0] / v[1] if v[1] else float("nan")) for k, v in totals.items()},
            "review": {f"{k[0]}:{k[1]}": (v[0] / v[1] if v[1] else float("nan")) for k, v in rev.items()},
            "count": {f"{k[0]}:{k[1]}": v[1] for k, v in totals.items()},
            "ci": {f"{k}:{i}": bootstrap(per_line, (k, i))
                   for k, n in (("bar4", 4), ("bar8", 8), ("beat", 4), ("sixteenth", 4)) for i in range(n)},
        }
    return out


def contrasts(records, min_bars=4, draws=4000, seed=11):
    """The two contrasts the implementation hangs on, each as a **paired** per-line difference.

    A per-position mean with an unpaired interval is the wrong test for "does instability rise
    towards the end of the bar": lines differ enormously in their overall level, and that variance
    swamps the within-line effect. Both contrasts are therefore formed inside each line first and the
    bootstrap resamples lines, which is the paired form and the only one whose interval means what it
    looks like.

      ``bar_parity``  mean instability of the odd bars of a four-bar group minus the even ones
      ``beat_slope``  mean instability of beat 4 minus beat 1 of the bar
    """
    out = {}
    for role_id, role in enumerate(ds.ROLES):
        lines = [r for r in records if r["role"] == role_id and r["bars"] >= min_bars]
        pairs = {"bar_parity": [], "beat_slope": []}
        for rec in lines:
            acc = defaultdict(lambda: [0.0, 0])
            for sym, step in zip(rec["syms"], rec["steps"]):
                v = instability((sym + ds.REL_MIN) % 12)
                acc[("bar", (step // 16) % 2)][0] += v
                acc[("bar", (step // 16) % 2)][1] += 1
                acc[("beat", (step % 16) // 4)][0] += v
                acc[("beat", (step % 16) // 4)][1] += 1
            if acc[("bar", 0)][1] and acc[("bar", 1)][1]:
                pairs["bar_parity"].append(acc[("bar", 1)][0] / acc[("bar", 1)][1]
                                           - acc[("bar", 0)][0] / acc[("bar", 0)][1])
            if acc[("beat", 0)][1] and acc[("beat", 3)][1]:
                pairs["beat_slope"].append(acc[("beat", 3)][0] / acc[("beat", 3)][1]
                                           - acc[("beat", 0)][0] / acc[("beat", 0)][1])
        res = {}
        rng = random.Random(seed)
        for name, xs in pairs.items():
            if len(xs) < 4:
                res[name] = {"mean": float("nan"), "ci": (float("nan"), float("nan")), "n": len(xs)}
                continue
            boots = []
            for _ in range(draws):
                boots.append(mean([xs[rng.randrange(len(xs))] for _ in range(len(xs))]))
            boots.sort()
            res[name] = {"mean": mean(xs), "ci": (boots[int(0.025 * draws)], boots[int(0.975 * draws)]),
                         "n": len(xs)}
        out[role] = res
    return out


def resolution(records, min_bars=4):
    """How often the **last** note of a phrase-closing bar is a tonic or a fifth, against every bar.

    The review's schedule ends on a resolution; this is the part of it that can be tested directly,
    because "resolving onto the tonic or the fifth" is a statement about one note.
    """
    out = {}
    for role_id, role in enumerate(ds.ROLES):
        lines = [r for r in records if r["role"] == role_id and r["bars"] >= min_bars]
        hit = defaultdict(lambda: [0, 0])
        for rec in lines:
            last = {}
            for sym, step in zip(rec["syms"], rec["steps"]):
                last[step // 16] = sym
            for bar, sym in last.items():
                pc = (sym + ds.REL_MIN) % 12
                for k in (("bar4", bar % 4), ("bar8", bar % 8)):
                    hit[k][0] += 1 if pc in (0, 7) else 0
                    hit[k][1] += 1
        out[role] = {f"{k[0]}:{k[1]}": (v[0] / v[1] if v[1] else float("nan"), v[1]) for k, v in hit.items()}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy", choices=["psy", "all"])
    ap.add_argument("--min-bars", type=int, default=4)
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    packs = ds.PSY_PACKS if a.packs == "psy" else ds.PSY_PACKS + ds.STAR_PACKS
    recs = ds.load(a.root, packs)
    kept, removed = ds.dedupe(recs)
    print(f"{len(recs)} lines, {removed} near duplicates dropped, {len(kept)} kept")
    m = measure(kept, a.min_bars)
    r = resolution(kept, a.min_bars)
    c = contrasts(kept, a.min_bars)
    print("\npaired per-line contrasts (Lerdahl instability):")
    for role, d in c.items():
        for name, v in d.items():
            print(f"  {role:5s} {name:11s} {v['mean']:+.3f} [{v['ci'][0]:+.3f}, {v['ci'][1]:+.3f}]  lines={v['n']}")
    for role, d in m.items():
        print(f"\n=== {role}: {d['lines']} lines, {d['notes']} notes (>= {a.min_bars} bars)")
        for family, n in (("bar4", 4), ("bar8", 8), ("beat", 4), ("sixteenth", 4)):
            print(f"  {family}:")
            for i in range(n):
                k = f"{family}:{i}"
                lo, hi = d["ci"][k]
                print(f"    {i}: lerdahl {d['mean'][k]:.3f} [{lo:.3f}, {hi:.3f}]  "
                      f"review {d['review'][k]:.3f}  n={d['count'][k]}")
        print("  last note of a bar is tonic or fifth:")
        for family, n in (("bar4", 4), ("bar8", 8)):
            row = "  ".join(f"{i}: {r[role][f'{family}:{i}'][0]:.3f}" for i in range(n))
            print(f"    {family}  {row}")
    if a.json:
        with open(a.json, "w", encoding="utf-8", newline="\n") as fh:
            json.dump({"measure": m, "resolution": r, "contrasts": c, "lines": len(kept)}, fh, indent=1)
        print(f"\nwritten to {a.json}")


if __name__ == "__main__":
    main()
