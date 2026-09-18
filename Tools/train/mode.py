"""Estimates the **mode** of a corpus line, and measures how much of that estimate is noise.

Why an estimate and not a label. A corpus line is already relative to its tonic: ``build_corpus.py``
runs a Krumhansl-Schmuckler key finding (Krumhansl, *Cognitive Foundations of Musical Pitch*, Oxford
University Press 1990) against the Krumhansl-Kessler **minor** profile, takes the winning pitch class
as the tonic and writes every note as its interval to that tonic. What the file never says, and what
no file name in the three psytrance packs says either (1 264 files scanned on 16.09.2026: six say
"min" or "minor", none says Phrygian, Dorian, harmonic, Hijaz or any other mode), is **which of the
six modes of ``phos::kScaleSteps``** that minor is. So a mode label for training has to be inferred
from the notes, and the size of the inference error is part of the finding rather than a footnote.

**The estimator** is the Bayesian key-profile model of Temperley (*Music and Probability*, MIT Press
2007, chapter 4) with the profile reduced to scale membership: a mode is a distribution over the
twelve pitch classes that puts mass ``1 - eps`` uniformly on its seven degrees and ``eps`` uniformly
on the twelve, and the log likelihood of a line is the sum over its note onsets of the log
probability of the note's pitch class. All six modes have exactly seven degrees, so the comparison is
between models of equal capacity and the likelihood is not a disguised count of scale size. The
posterior over the six modes under a flat prior is what :func:`estimate` returns, and its largest
entry is the **confidence** -- a line whose notes are the tonic, the fourth and the fifth is
consistent with all six modes, and the posterior says so instead of picking one.

**Why not a correlation with a mode profile.** Krumhansl-Kessler gives two profiles, major and minor,
measured in probe-tone experiments on common-practice listeners; there is no published profile for
Phrygian dominant or double harmonic, and inventing one would make the label an artefact of the
invention. Scale membership is the one thing the six modes of ``Harmony.h`` really differ in, and it
is what the composer's constraint masks act on, so it is what the label should measure.

**What the estimate is worth** is measured three ways by ``--report``, and the numbers are in the
docstring of :func:`report`: the posterior confidence over the corpus, a split-half agreement (the
mode from a line's odd notes against the mode from its even notes), and a recovery experiment on
synthetic lines whose mode is known by construction.

Usage:
    python Tools/train/mode.py --root M:/Midi --report
"""
import argparse
import math
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dataset                                                # noqa: E402

#: The six modes of ``phos::kScaleSteps`` (Core/include/phos/Harmony.h), in that order. Mirrored, not
#: imported: this file is the training side and the header is the inference side, and the self test
#: checks that the two agree.
SCALES = (
    (0, 2, 3, 5, 7, 8, 10),      # 0 Aeolian
    (0, 1, 3, 5, 7, 8, 10),      # 1 Phrygian
    (0, 2, 3, 5, 7, 8, 11),      # 2 Harmonic minor
    (0, 1, 4, 5, 7, 8, 10),      # 3 Phrygian dominant
    (0, 1, 4, 5, 7, 8, 11),      # 4 Double harmonic
    (0, 2, 3, 5, 7, 9, 10),      # 5 Dorian
)
SCALE_NAMES = ("Aeolian", "Phrygian", "HarmonicMinor", "PhrygianDominant", "DoubleHarmonic", "Dorian")
N_MODE = len(SCALES)

#: How much probability a mode keeps for the five pitch classes outside it. Not a free knob: it is
#: the share of corpus notes that fall outside the line's own best-fitting mode, which is what a
#: chromatic floor is supposed to model. Measured on the three psytrance packs
#: (:func:`chromatic_floor`, 16.09.2026): 1.1 %. The estimate is almost insensitive to it -- the same
#: labels come out for every floor between 0.011 and 0.085 and the split-half agreement moves by less
#: than 0.1 point -- because what decides a line is scale *membership* and not the weight inside the
#: scale. Reported rather than tuned.
EPS = 0.011


def colour_tones(scale):
    """Pitch classes of @p scale that ``phos::isColourTone`` calls the genre's colour (Harmony.h).

    The flat second, and the upper note of any augmented second between neighbouring degrees -- the
    Hijaz interval. Computed exactly as the header computes it, so the two cannot drift apart.
    """
    steps = SCALES[scale]
    out = {1} if 1 in steps else set()
    for d in range(1, 7):
        if steps[d] - steps[d - 1] == 3:
            out.add(steps[d] % 12)
    return out


def is_colour_tone(scale, pc):
    """Whether pitch class @p pc (relative to the tonic) is a colour tone of @p scale.

    ``phos::isColourTone`` answers for the flat second **whatever the mode**, because the flat second
    is the genre's signature interval and a mode that does not contain it will never be asked. This
    mirrors that: pc 1 is always a colour tone, an augmented-second upper note is one when the mode
    has that augmented second.
    """
    pc %= 12
    if pc == 1:
        return True
    return pc in colour_tones(scale)


def n_colour_tones(scale):
    """How many of its own seven degrees a mode gives to the colour (``phos::scaleColourTones``)."""
    return sum(1 for d in SCALES[scale] if is_colour_tone(scale, d))


# ------------------------------------------------------------------------------------------ estimate

def _log_profiles(eps=EPS):
    """log p(pitch class | mode) for the six modes: 1-eps spread over the degrees, eps over twelve."""
    out = []
    for steps in SCALES:
        inside = (1.0 - eps) / 7.0 + eps / 12.0
        outside = eps / 12.0
        row = [math.log(inside if pc in steps else outside) for pc in range(12)]
        out.append(row)
    return out


_LOGP = _log_profiles()


def histogram(syms):
    """Pitch-class counts of a line's symbols (which are ``rel - REL_MIN``, i.e. tonic-relative)."""
    h = [0] * 12
    for s in syms:
        h[(s + dataset.REL_MIN) % 12] += 1
    return h


def posterior(syms, logp=None):
    """Posterior over the six modes of a line, under a flat prior."""
    logp = logp or _LOGP
    h = histogram(syms)
    ll = [sum(c * logp[m][pc] for pc, c in enumerate(h) if c) for m in range(N_MODE)]
    top = max(ll)
    e = [math.exp(x - top) for x in ll]
    z = sum(e)
    return [x / z for x in e]


#: A posterior at or below this counts as undecided: the line's notes fit two or more modes equally.
DECIDED = 0.9


def estimate(syms):
    """(mode, confidence): the most probable mode and its posterior probability.

    **The tie-break matters and is not an accident.** Two thirds of the corpus's lines leave two to
    four modes *exactly* equally likely, because they never play a degree that tells those modes
    apart -- 22 % of lines ever play the natural second, 3 % the natural sixth, 5 % the major
    seventh. ``max`` takes the first such mode, and ``SCALES`` is ordered so that the first is the
    least colourful one: Aeolian before Phrygian, Phrygian before Phrygian dominant, and
    ``n_colour_tones`` is 0, 1, 1, 2, 3, 0 down the list. An undecided line is therefore labelled
    with the mode that claims the *fewest* colour tones among those that fit it. The label can
    under-call the colour of a line; it can never claim a colour the line does not play, which is the
    direction an error has to point in for this round -- a mode row trained on lines that do not use
    its colour would teach the model to avoid the colour all over again.
    """
    p = posterior(syms)
    m = max(range(N_MODE), key=lambda i: p[i])
    return m, p[m]


def chromatic_floor(records):
    """Share of notes a line's best-fitting mode leaves outside itself -- what ``EPS`` models.

    Fitted with a flat floor first (eps = 1/12 would make every mode equal, so the fit uses plain
    scale membership: the mode containing the most of the line's notes) and then reported as the
    weighted mean over lines. Circular only in the sense every smoothing parameter is: it is the
    residual of the model it smooths, which is what a floor is.
    """
    inside = total = 0
    for rec in records:
        h = histogram(rec["syms"])
        best = max(sum(h[pc] for pc in steps) for steps in SCALES)
        inside += best
        total += sum(h)
    return 1.0 - inside / max(1, total)


# ------------------------------------------------------------------------------- colour-tone share

def colour_share(records, modes=None, only_colourful=True):
    """(share, notes, lines): how much of the corpus's melodic weight sits on the colour tones.

    ``modes[i]`` is the estimated mode of ``records[i]``; without it every line is estimated here.
    With ``only_colourful`` the count is restricted to lines whose mode has a colour tone at all --
    a line in Aeolian or Dorian has none by construction and would only dilute the target with
    zeros. This is the target the composer's own colour-tone share is held to, per role, in the
    self test's section "the mode's colour" (Tests/selftest.cpp).
    """
    hits = total = lines = 0
    for i, rec in enumerate(records):
        m = modes[i] if modes is not None else estimate(rec["syms"])[0]
        if only_colourful and n_colour_tones(m) == 0:
            continue
        lines += 1
        for s in rec["syms"]:
            total += 1
            if is_colour_tone(m, s + dataset.REL_MIN):
                hits += 1
    return hits / max(1, total), total, lines


def colour_interval(records, modes=None, resamples=10000, seed=7, only_colourful=True):
    """(lo, hi): a 95 % percentile bootstrap of :func:`colour_share`, resampling **lines**.

    The resampling unit is the line and not the note, for the reason ``Tools/train/confidence.py``
    gives at length: the notes of one loop are not independent draws, and a bootstrap over notes
    would report an interval several times too narrow.
    """
    import random as _random                                     # noqa: PLC0415
    per = []
    for i, rec in enumerate(records):
        m = modes[i] if modes is not None else estimate(rec["syms"])[0]
        if only_colourful and n_colour_tones(m) == 0:
            continue
        hits = sum(1 for s in rec["syms"] if is_colour_tone(m, s + dataset.REL_MIN))
        per.append((hits, len(rec["syms"])))
    if not per:
        return 0.0, 0.0
    rng = _random.Random(seed)
    draws = []
    for _ in range(resamples):
        h = t = 0
        for _ in range(len(per)):
            a, b = per[rng.randrange(len(per))]
            h += a
            t += b
        draws.append(h / max(1, t))
    draws.sort()
    return draws[int(0.025 * resamples)], draws[int(0.975 * resamples)]


# ------------------------------------------------------------------------------------- reliability

def split_half(rec):
    """(mode of the odd notes, mode of the even notes, whether they agree).

    The split is by note index and not by bar, so both halves see the whole loop: a mode is a
    property of the whole line, and splitting by bar would measure whether the loop modulates.
    """
    a = rec["syms"][0::2]
    b = rec["syms"][1::2]
    if len(a) < 4 or len(b) < 4:
        return None
    ma, mb = estimate(a)[0], estimate(b)[0]
    return ma, mb, ma == mb


def recolour(syms, target):
    """The same line moved into mode @p target: every note to the nearest degree of that mode.

    The recovery experiment needs lines whose mode is known by construction but whose *rhythm, note
    count and degree usage* are those of real psytrance, because the estimator's power depends on
    exactly those. Mapping a real line onto another mode's degrees keeps all three; drawing notes
    from a uniform scale would measure a corpus nobody plays. Ties (a note exactly between two
    degrees) go to the lower degree, which is deterministic and is the same rule in both directions.
    """
    steps = SCALES[target]
    out = []
    for s in syms:
        rel = s + dataset.REL_MIN
        oct_, pc = divmod(rel, 12)
        best = min(steps, key=lambda d: (min((d - pc) % 12, (pc - d) % 12), d))
        # Keep the note near where it was: the chosen degree may be an octave away after the modulo.
        cand = [oct_ * 12 + best, oct_ * 12 + best - 12, oct_ * 12 + best + 12]
        rel2 = min(cand, key=lambda c: (abs(c - rel), c))
        out.append(max(dataset.REL_MIN, min(dataset.REL_MAX, rel2)) - dataset.REL_MIN)
    return out


def recovery(records, limit=0):
    """(all, decided) confusion matrices of the estimator on synthetic lines of known mode.

    ``conf[true][got]`` counts lines that were written into mode ``true`` and estimated as ``got``;
    the second matrix counts only the lines the estimator called with a posterior above
    :data:`DECIDED`. Note that recolouring is **lossy on purpose**: a line whose original mode had no
    flat second gains one only where a note really lands there, so a recoloured Phrygian line uses
    its flat second as often as the source line used the tone nearest to it. That is the honest
    difficulty -- a real Phrygian loop that never plays its flat second is not recognisable as
    Phrygian either, by this estimator or by a listener.
    """
    conf = [[0] * N_MODE for _ in range(N_MODE)]
    dec = [[0] * N_MODE for _ in range(N_MODE)]
    src = records[:limit] if limit else records
    for rec in src:
        for m in range(N_MODE):
            got, c = estimate(recolour(rec["syms"], m))
            conf[m][got] += 1
            if c > DECIDED:
                dec[m][got] += 1
    return conf, dec


def report(records, log=sys.stdout):
    """Prints everything ``--report`` measures; the numbers of 16.09.2026 are in the PLAN block.

    Four blocks: the chromatic floor the smoothing is set from, the distribution of the estimated
    mode and its confidence, the split-half agreement, and the recovery confusion matrix.
    """
    print(f"{len(records)} lines, {sum(len(r['syms']) for r in records)} notes", file=log)
    print(f"  chromatic floor (notes outside the best-fitting mode): {100 * chromatic_floor(records):.1f} %", file=log)

    est = [estimate(r["syms"]) for r in records]
    dist = Counter(m for m, _ in est)
    conf = [c for _, c in est]
    conf_sorted = sorted(conf)
    print("  estimated mode: " + ", ".join(
        f"{SCALE_NAMES[m]} {100 * dist.get(m, 0) / len(records):.1f} %" for m in range(N_MODE)), file=log)
    print(f"  posterior confidence: median {conf_sorted[len(conf) // 2]:.3f}, "
          f"decided (above {DECIDED}) in {100 * sum(1 for c in conf if c > DECIDED) / len(conf):.1f} % "
          f"of lines; the rest is an exact tie between two to four modes", file=log)
    for m in range(N_MODE):
        sel = [c for mm, c in est if mm == m]
        if not sel:
            continue
        sel.sort()
        print(f"    {SCALE_NAMES[m]:<18s} {len(sel):4d} lines, median confidence {sel[len(sel) // 2]:.3f}, "
              f"decided {100 * sum(1 for c in sel if c > DECIDED) / len(sel):.0f} %", file=log)

    sh = [split_half(r) for r in records]
    sh = [x for x in sh if x]
    agree = sum(1 for _, _, ok in sh if ok) / max(1, len(sh))
    # Chance agreement of two draws from the marginal the estimator produces on the halves.
    marg = Counter()
    for a, b, _ in sh:
        marg[a] += 1
        marg[b] += 1
    n = sum(marg.values())
    chance = sum((c / n) ** 2 for c in marg.values())
    kappa = (agree - chance) / (1.0 - chance) if chance < 1.0 else 0.0
    print(f"  split-half agreement {100 * agree:.1f} % over {len(sh)} lines "
          f"(chance {100 * chance:.1f} %, Cohen's kappa {kappa:.3f})", file=log)
    # How often each pitch class is played at all: this is what decides a mode, and why two thirds
    # of the lines cannot be decided -- the degrees that tell the modes apart are rarely there.
    used = Counter()
    for rec in records:
        for pc, c in enumerate(histogram(rec["syms"])):
            if c:
                used[pc] += 1
    print("  lines that ever play each degree: " + " ".join(
        f"{pc}:{100 * used[pc] / len(records):.0f}%" for pc in range(12)), file=log)

    conf_m, dec_m = recovery(records)
    n_all = sum(sum(r) for r in conf_m)
    n_dec = sum(sum(r) for r in dec_m)
    tot = sum(conf_m[m][m] for m in range(N_MODE)) / max(1, n_all)
    tot_dec = sum(dec_m[m][m] for m in range(N_MODE)) / max(1, n_dec)
    print(f"  recovery on {n_all} synthetic lines of known mode: {100 * tot:.1f} % correct overall, "
          f"{100 * tot_dec:.1f} % on the {n_dec} the estimator called decided", file=log)
    print("    true \\ got   " + " ".join(f"{s[:5]:>6s}" for s in SCALE_NAMES), file=log)
    for m in range(N_MODE):
        row = conf_m[m]
        s = sum(row) or 1
        print(f"    {SCALE_NAMES[m][:12]:<12s} " + " ".join(f"{100 * x / s:6.1f}" for x in row), file=log)

    # The target the composer's own lines are held to, with the interval that makes it a measurement
    # rather than a number: a percentile bootstrap over **lines**, never over notes -- the notes of
    # one loop are anything but independent (Koehn, "Statistical significance tests for machine
    # translation evaluation", EMNLP 2004, on exactly this mistake). Per role as well as overall,
    # because an aggregate can be met by one role running past it while another sits at zero.
    share, notes, lines = colour_share(records, [m for m, _ in est])
    lo, hi = colour_interval(records, [m for m, _ in est])
    print(f"  colour-tone share of the lines in a colourful mode: {share:.4f} "
          f"[{lo:.4f}, {hi:.4f}] ({notes} notes of {lines} lines)", file=log)
    for role, name in enumerate(dataset.ROLES):
        sel = [(r, mm) for r, (mm, _) in zip(records, est) if r["role"] == role]
        if not sel:
            continue
        rs, rn, rl = colour_share([r for r, _ in sel], [mm for _, mm in sel])
        rlo, rhi = colour_interval([r for r, _ in sel], [mm for _, mm in sel])
        print(f"    {name:<18s} {rs:.4f} [{rlo:.4f}, {rhi:.4f}]  ({rn} notes of {rl} lines)", file=log)
    # The same count without any mode condition, over every line: what a mode-blind composer
    # reproduces, and the floor the in-mode target has to be read against.
    blind = sum(1 for rec, (mm, _) in zip(records, est)
                for s in rec["syms"] if is_colour_tone(mm, s + dataset.REL_MIN))
    blind_n = sum(len(r["syms"]) for r in records)
    print(f"  the same count over ALL lines, colourful mode or not: {blind / max(1, blind_n):.4f} "
          f"({blind_n} notes) -- the mode-blind reference", file=log)
    for m in range(N_MODE):
        if n_colour_tones(m) == 0:
            continue
        sel = [r for r, (mm, _) in zip(records, est) if mm == m]
        if not sel:
            continue
        s, nn, _ = colour_share(sel, [m] * len(sel))
        print(f"    {SCALE_NAMES[m]:<18s} {s:.4f}  ({nn} notes, colour tones "
              f"{sorted(colour_tones(m))})", file=log)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy")
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    recs = dataset.load(a.root, dataset.packs_for(a.packs))
    report(recs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
