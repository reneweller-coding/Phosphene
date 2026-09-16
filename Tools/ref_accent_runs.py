"""Do accents cluster? The conditional probability of an accent after an accent, in the MIDI corpus.

**Why.** Phosphene draws the acid accent independently per step (``Core/src/Melody.cpp``,
``makeAcid``: 0.32 on the third sixteenth, 0.12 on the downbeat, 0.2 elsewhere). The accent of a
TB-303 charges a capacitor with a time constant of about 150 ms (``Core/src/Acid.cpp``,
``kSweepTau``), so a *single* accent charges the sweep and lets it discharge again, while accents
that follow one another charge it before it has discharged and their sweeps climb -- the "wow" of an
accented run. Independent draws produce that run only by chance. Before changing the draw, measure
whether the bought loops actually place accents in runs, and by how much.

**What is measured.** For every melodic loop of the psytrance packs, on the sixteenth grid of
``build_corpus.top_line``:

* the marginal accent rate ``P(A)``,
* ``P(A at s+1 | A at s)`` and ``P(A at s+2 | A at s)``, counted over the *onsets* at those steps
  (a step with no note is neither an accent nor a non-accent),
* the same two probabilities given a *non*-accent at s, so the lift is a ratio of two measured
  numbers and not a ratio against an assumption,
* ``P(A at next onset | A at this onset)`` -- the same question asked in note index rather than in
  grid steps, because a line that plays every other sixteenth has no onset at s+1 at all.

Everything is computed per loop first and then pooled by *counts*, never by averaging per-loop
ratios: a loop with three onsets would otherwise weigh as much as one with sixty. Beside every
pooled number the tool prints the **median over loops** of the same conditional and how many loops
that median is taken over, because a pooled number can be one arrangement with sixty thousand notes.

**Loops, not arrangements.** ``--max-steps`` drops anything longer than the given number of
sixteenths (128 = eight bars by default in the runs of 16.09.2026). A whole arrangement has *section*
dynamics -- a quiet A part and a loud B part -- and a threshold placed between them declares every
note of the B part an accent, which produces an autocorrelation near 1 that has nothing to do with
accenting a step. In the run of 16.09.2026 the unfiltered arp figure was a lift of 63; that number
measures the arrangement, not the accent.

**Two accent definitions, because one of them is known to be blind.** ``build_corpus.analyse`` calls
a note accented when its velocity reaches the loop's median plus 10. Under that rule the acid table
of ``Core/src/CorpusTables.cpp`` counts *zero* accents in 542 acid loops, which is a statement about
the threshold as much as about the loops. This tool therefore also reports

* ``spread``: how many loops have any velocity variation at all, and how large it is, and
* a per-loop **two-means split** (Lloyd's algorithm on the loop's velocities, seeded at the minimum
  and the maximum, run to convergence): the upper cluster is the accent group. It is scale-free, so
  a loop whose accents sit 6 velocity steps above the rest is not missed. A loop is only split when
  its velocities actually separate -- at least ``--sep`` velocity units between the cluster means and
  at least two notes in each cluster -- otherwise it counts as "flat" and contributes no accents.

Only counts and probabilities leave this tool; no MIDI is copied.

Usage:
    python Tools/ref_accent_runs.py --root M:/Midi
    python Tools/ref_accent_runs.py --root M:/Midi --view track      # roles from the MIDI track name
    python Tools/ref_accent_runs.py --root M:/Midi --json out.json

Needs numpy only for nothing at all -- it is pure Python, so it runs with any interpreter.
"""
import argparse
import importlib.util
import math
import json
import os
import struct
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_CORPUS = os.path.normpath(os.path.join(_HERE, "corpus"))


def _load(name, path):
    """Imports a file as a module (Tools/corpus is not on a package path)."""
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bc = _load("build_corpus", os.path.join(_CORPUS, "build_corpus.py"))

#: The fourth psytrance source of Tools/train/dataset.py: flat files whose role sits in the name.
EXTRA_PACKS = ["Midi Klowd"]


def two_means(vels, sep):
    """Splits a loop's velocities into a low and a high group; returns the threshold or None.

    Lloyd's algorithm with two centres seeded at the minimum and the maximum velocity, iterated to a
    fixed point (at most 20 rounds; with two centres on integers it converges in a handful). Returns
    the smallest velocity of the upper cluster, or None when the loop does not separate: fewer than
    two notes on either side, or cluster means closer together than `sep`.
    """
    if len(vels) < 4:
        return None
    lo, hi = float(min(vels)), float(max(vels))
    if hi - lo < sep:
        return None
    for _ in range(20):
        a = [v for v in vels if abs(v - lo) <= abs(v - hi)]
        b = [v for v in vels if abs(v - lo) > abs(v - hi)]
        if not a or not b:
            return None
        na, nb = sum(a) / len(a), sum(b) / len(b)
        if na == lo and nb == hi:
            break
        lo, hi = na, nb
    if len(a) < 2 or len(b) < 2 or hi - lo < sep:
        return None
    return min(b)


def wilson(k, n, z=1.96):
    """Wilson score interval for k successes in n trials -- the usual 95 % band at z = 1.96.

    The plain normal interval is useless here: the acid role contributes a few dozen transitions, and
    at p near 0.2 with n = 69 the normal approximation runs outside [0, 1]. Wilson's interval
    (Wilson, "Probable inference, the law of succession, and statistical inference", JASA 22, 1927)
    stays inside it and is the standard small-sample choice.
    """
    if not n:
        return None, None
    p = k / n
    den = 1.0 + z * z / n
    centre = (p + z * z / (2 * n)) / den
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / den
    return centre - half, centre + half


class Tally:
    """Counts for one role: marginal, the two step lags, and the note-index lag."""

    def __init__(self):
        self.loops = 0            # loops that contributed accents
        self.flat = 0             # loops with no usable accent split
        self.onsets = 0
        self.accents = 0
        # [given accent at s][accent at s+lag] for lag 1 and 2, over onsets present at s+lag
        self.lag = {1: [[0, 0], [0, 0]], 2: [[0, 0], [0, 0]]}
        self.nxt = [[0, 0], [0, 0]]   # same, in note index
        self.spread = Counter()       # max-min velocity per loop, bucketed
        self.perLoop = []             # per-loop (P(A), P(A at next onset | A)) where both are defined

    def add_line(self, line, thr):
        """Folds one loop into the counts. `line` is build_corpus.top_line, `thr` the accent floor."""
        self.loops += 1
        acc = [None] * len(line)
        for k, s in enumerate(line):
            if s:
                acc[k] = 1 if s[1] >= thr else 0
        self.onsets += sum(1 for a in acc if a is not None)
        self.accents += sum(1 for a in acc if a == 1)
        for k, a in enumerate(acc):
            if a is None:
                continue
            for lag in (1, 2):
                if k + lag < len(acc) and acc[k + lag] is not None:
                    self.lag[lag][a][acc[k + lag]] += 1
        idx = [k for k, a in enumerate(acc) if a is not None]
        loop = [0, 0]
        for i in range(len(idx) - 1):
            self.nxt[acc[idx[i]]][acc[idx[i + 1]]] += 1
            if acc[idx[i]] == 1:
                loop[acc[idx[i + 1]]] += 1
        if loop[0] + loop[1] >= 3 and idx:
            marg = sum(1 for a in acc if a == 1) / len(idx)
            self.perLoop.append((marg, loop[1] / (loop[0] + loop[1])))

    def report(self):
        """The measured probabilities as a dict, with the raw counts beside every one of them."""
        def p(row):
            n = row[0] + row[1]
            return (row[1] / n if n else None, n)

        marg = self.accents / self.onsets if self.onsets else None
        out = {"loops": self.loops, "flat": self.flat, "onsets": self.onsets,
               "accents": self.accents, "p_accent": marg}
        for lag in (1, 2):
            pa, na = p(self.lag[lag][1])
            pn, nn = p(self.lag[lag][0])
            out[f"p_lag{lag}_given_accent"] = pa
            out[f"n_lag{lag}_given_accent"] = na
            out[f"p_lag{lag}_given_plain"] = pn
            out[f"n_lag{lag}_given_plain"] = nn
            out[f"lift_lag{lag}"] = (pa / pn) if (pa and pn) else None
        pa, na = p(self.nxt[1])
        pn, nn = p(self.nxt[0])
        out["p_next_given_accent"], out["n_next_given_accent"] = pa, na
        out["p_next_given_plain"], out["n_next_given_plain"] = pn, nn
        out["ci_next_given_accent"] = wilson(self.nxt[1][1], na)
        out["ci_next_given_plain"] = wilson(self.nxt[0][1], nn)
        out["lift_next"] = (pa / pn) if (pa and pn) else None
        if self.perLoop:
            mn = sorted(v for _, v in self.perLoop)
            mm = sorted(v for v, _ in self.perLoop)
            out["loops_with_runs"] = len(mn)
            out["median_p_next_given_accent"] = mn[len(mn) // 2]
            out["median_p_accent"] = mm[len(mm) // 2]
            out["quartiles_p_next_given_accent"] = [mn[len(mn) // 4], mn[3 * len(mn) // 4]]
        return out


def walk(root, packs):
    """Every .mid file under the given packs, in a stable order."""
    for pack in packs:
        base = os.path.join(root, pack)
        if not os.path.isdir(base):
            continue
        for dirpath, _, files in os.walk(base):
            for f in sorted(files):
                if f.lower().endswith(".mid"):
                    yield pack, os.path.join(dirpath, f)


def lines_of(path, view):
    """(role, line) pairs of a file: one for the file view, one per named track for the track view."""
    if view == "file":
        role = bc.role_of(path)
        if role is None:
            return []
        ppq, notes = bc.read_midi(path)
        if not ppq or len(notes) < 4:
            return []
        line = bc.top_line(ppq, notes)
        return [(role, line)] if line else []
    ppq, tracks = bc.read_midi_tracks(path)
    if not ppq:
        return []
    out = []
    for tr in tracks:
        if len(tr["notes"]) < 4:
            continue
        name = (tr["name"] or "").lower()
        role = None
        for word, r in (("acid", "acid"), ("303", "acid"), ("seq", "acid"), ("riff", "acid"),
                        ("arp", "arp"), ("lead", "lead"), ("melod", "lead")):
            if word in name:
                role = r
                break
        if role is None:
            role = bc.role_of(path)
        if role is None:
            continue
        line = bc.top_line(ppq, sorted(tr["notes"]))
        if line:
            out.append((role, line))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--view", choices=("file", "track"), default="file")
    ap.add_argument("--rule", choices=("corpus", "twomeans", "both"), default="both")
    ap.add_argument("--sep", type=float, default=6.0, help="least distance of the two cluster means")
    ap.add_argument("--extra", action="store_true", help="add the fourth pack (Midi Klowd)")
    ap.add_argument("--max-steps", type=int, default=128, help="drop lines longer than this (0 = keep all)")
    ap.add_argument("--per-pack", action="store_true")
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    packs = list(bc.PACKS) + (EXTRA_PACKS if a.extra else [])
    rules = ["corpus", "twomeans"] if a.rule == "both" else [a.rule]
    tallies = {rule: {r: Tally() for r in bc.ROLES} for rule in rules}
    per_pack = {rule: {} for rule in rules}
    files = 0
    for pack, path in walk(a.root, packs):
        try:
            pairs = lines_of(path, a.view)
        except (IndexError, struct.error, ValueError, OSError):
            continue
        for role, line in pairs:
            if a.max_steps and len(line) > a.max_steps:
                continue
            vels = [s[1] for s in line if s]
            if len(vels) < 4:
                continue
            pitches = [s[0] for s in line if s]
            if len(set(pitches)) < 3:      # a rhythm, not a melody: build_corpus skips it too
                continue
            files += 1
            span = max(vels) - min(vels)
            srt = sorted(vels)
            med = srt[len(srt) // 2]
            for rule in rules:
                thr = (med + 10) if rule == "corpus" else two_means(vels, a.sep)
                t = tallies[rule][role]
                t.spread[min(span, 60) // 5 * 5] += 1
                if thr is None or max(vels) < thr:
                    t.flat += 1
                    continue
                t.add_line(line, thr)
                if a.per_pack:
                    pp = per_pack[rule].setdefault(pack, {r: Tally() for r in bc.ROLES})[role]
                    pp.add_line(line, thr)

    print(f"{files} melodic loops, view={a.view}, root={a.root}")
    out = {"files": files, "view": a.view, "rules": {}}
    for rule in rules:
        print(f"\n=== accent rule: {rule} ===")
        out["rules"][rule] = {}
        for role in bc.ROLES:
            t = tallies[rule][role]
            rep = t.report()
            out["rules"][rule][role] = rep
            sp = ", ".join(f"{k}-{k+4}:{v}" for k, v in sorted(t.spread.items()))
            print(f"[{role}] loops with accents {rep['loops']}, flat {rep['flat']}, "
                  f"onsets {rep['onsets']}, accents {rep['accents']}")
            print(f"        velocity spread per loop: {sp}")
            if not rep["onsets"]:
                continue
            print(f"        P(A)                 = {rep['p_accent']:.4f}")
            for lag in (1, 2):
                pa, na = rep[f"p_lag{lag}_given_accent"], rep[f"n_lag{lag}_given_accent"]
                pn, nn = rep[f"p_lag{lag}_given_plain"], rep[f"n_lag{lag}_given_plain"]
                lf = rep[f"lift_lag{lag}"]
                if pa is None or pn is None:
                    continue
                print(f"        P(A at s+{lag} | A at s) = {pa:.4f}  (n={na})   "
                      f"P(A at s+{lag} | plain) = {pn:.4f}  (n={nn})   lift {lf:.2f}")
            if rep["p_next_given_accent"] is not None and rep["p_next_given_plain"] is not None:
                print(f"        P(A at next onset | A) = {rep['p_next_given_accent']:.4f} "
                      f"(n={rep['n_next_given_accent']})   given plain "
                      f"{rep['p_next_given_plain']:.4f} (n={rep['n_next_given_plain']})   "
                      f"lift {rep['lift_next']:.2f}")
                ca, cp = rep["ci_next_given_accent"], rep["ci_next_given_plain"]
                if ca[0] is not None and cp[0] is not None:
                    print(f"        95 % (Wilson): given accent [{ca[0]:.3f}, {ca[1]:.3f}], "
                          f"given plain [{cp[0]:.3f}, {cp[1]:.3f}] -> lift in "
                          f"[{ca[0] / cp[1]:.2f}, {ca[1] / cp[0]:.2f}]")
            if "median_p_next_given_accent" in rep:
                q = rep["quartiles_p_next_given_accent"]
                print(f"        per loop ({rep['loops_with_runs']} loops with >= 3 accented onsets): "
                      f"median P(A at next | A) = {rep['median_p_next_given_accent']:.4f} "
                      f"[q1 {q[0]:.3f}, q3 {q[1]:.3f}], median P(A) = {rep['median_p_accent']:.4f}")
        if a.per_pack:
            for pack, roles in sorted(per_pack[rule].items()):
                for role in bc.ROLES:
                    rep = roles[role].report()
                    if not rep["onsets"]:
                        continue
                    print(f"  [{pack} / {role}] P(A)={rep['p_accent']:.3f} "
                          f"P(A|A,s+1)={rep['p_lag1_given_accent']} "
                          f"P(A|A,next)={rep['p_next_given_accent']} n={rep['n_next_given_accent']}")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
        print(f"\nwritten to {a.json}")


if __name__ == "__main__":
    main()
