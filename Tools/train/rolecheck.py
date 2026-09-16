"""Decides by content whether a file with an ambiguous name is a melodic line of a known role.

The Star Samples psytrance folder holds 3266 flat files whose role sits in the name. The name-based
detector of ``build_corpus.role_of`` accepts 558 of them and rejects 2708, and about 474 of the
rejected ones are called "synth loop" -- a word that says nothing about whether the file is a lead
melody, a chord bed, a stab or a drone. Mapping the word to a role would be a guess, and a wrong
guess pollutes the role split of every model trained afterwards, so the class is decided by
**content**, measured against the lines whose role the name already gives away.

Features, all read off the raw note list before the top-line reduction (a chord bed reduces to a
perfectly respectable top line, which is exactly why the reduction cannot be the judge):

* ``chord_share``   -- share of onset times at which two or more notes sound. A lead is close to 0,
  a chord or stab file close to 1. This is the feature that carries the decision.
* ``poly``          -- mean number of notes per onset time.
* ``span``          -- highest minus lowest pitch in semitones.
* ``density``       -- onsets per sixteen sixteenths.
* ``med_len``       -- median note length in sixteenths (a pad holds, a lead does not).
* ``move``          -- share of consecutive top-line onsets that change pitch (a drone is near 0).
* ``mean_pitch``    -- bass material sits low and nothing else does.
* ``distinct_pc``   -- distinct pitch classes.

The labels for fitting come from the file names of *other* files in the same folder, and they are
kept to the three classes the names state unambiguously: **melodic** (what ``role_of`` accepts:
acid, lead, arp), **bass** (bassline, sub bassline, top bassline, psy bass, kit bass) and **padlike**
(pad, pads, drone, chord, stab). A first attempt lumped bass and padlike into one "not melodic"
class; that made the question "melody or bassline?", because 1765 of the 1815 negatives are bass,
and the resulting classifier had 100 % precision at 7.7 % recall -- a number that says nothing. The
three-class form is the well-posed one, and the confusion matrix is measured on a **held-out third**
of the labelled files that the fit never saw.

One thing the features settle immediately: this folder is monophonic throughout. ``chord_share`` has
median 0.00 and ``poly`` median 1.00 in every class, melodic and non-melodic alike. The files are
single-line stems, so "synth loop" cannot be a chord bed in the first place -- the question is
whether it is a melody or a bassline.

**Superseded, and kept for the record.** The round of 16.09.2026 replaced this with
``Tools/corpus/rolemodel.py``, which asks the same question over the whole corpus rather than one
folder, decides per *track* rather than per file, and adds the evidence this file never looked at:
the MIDI track name, and the classes of the other tracks of the same folder. It reaches 97.4 %
melodic precision against vendor names where this file reached 78.4 %. The conclusion of this file
nevertheless survived that round: content-admitted lines were measured again, on a hand-labelled
sample of the population they are actually drawn from, at 50 % precision, and adding them to the
training set made the held-out NLL worse. This file stays so that the first measurement can still be
reproduced next to the second.

Usage:
    python Tools/train/rolecheck.py --report
    python Tools/train/rolecheck.py --report --dump-threshold 0.9
"""
import argparse
import math
import os
import re
import struct
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dataset                                                # noqa: E402

bc = dataset.bc

STAR = os.path.join("Star Samples", "Psy Trance Midis")
BASS_WORDS = ("bass",)
PADLIKE_WORDS = ("pad", "drone", "chord", "stab")
DRUM_WORDS = ("drum", "kick", "perc", "hat", "snare", "clap")
AMBIGUOUS = ("synth loop", "synth line", "synth ", " synth")
FEATURES = ("chord_share", "poly", "span", "density", "med_len", "move", "mean_pitch", "distinct_pc")


def features(path):
    """The eight content features of one file, or None when it is not readable as a melodic line."""
    try:
        ppq, notes = bc.read_midi(path)
    except (IndexError, struct.error, TypeError):
        return None
    if not ppq or len(notes) < 4:
        return None
    step = ppq / 4.0
    by_start = defaultdict(list)
    for s, e, p, v in notes:
        by_start[int(round(s / step))].append((p, e - s))
    onsets = sorted(by_start)
    if len(onsets) < 4:
        return None
    poly = [len(by_start[k]) for k in onsets]
    pitches = [p for _s, _e, p, _v in notes]
    lengths = sorted(max(1, int(round(d / step))) for k in onsets for _p, d in by_start[k])
    top = [max(p for p, _d in by_start[k]) for k in onsets]
    moves = sum(1 for i in range(1, len(top)) if top[i] != top[i - 1])
    total_steps = max(1, onsets[-1] + 1)
    return {
        "chord_share": sum(1 for x in poly if x >= 2) / len(poly),
        "poly": sum(poly) / len(poly),
        "span": max(pitches) - min(pitches),
        "density": 16.0 * len(onsets) / total_steps,
        "med_len": lengths[len(lengths) // 2],
        "move": moves / max(1, len(top) - 1),
        "mean_pitch": sum(pitches) / len(pitches),
        "distinct_pc": len({p % 12 for p in pitches}),
    }


def stem(name):
    s = os.path.splitext(os.path.basename(name))[0].lower()
    s = re.sub(r"\d+", "", s)
    s = re.sub(r"[_\-]+", " ", s)
    return re.sub(r"\s+", " ", s).strip()


MELODIC_ROLES = ["acid", "lead", "arp"]


def label_of(path):
    """'melodic' / 'bass' / 'padlike' from the name, or None when the name does not say."""
    r = bc.role_of(path)
    if r is not None:
        return "melodic" if r in MELODIC_ROLES else None
    s = " " + stem(path) + " "
    if any(w in s for w in DRUM_WORDS):
        return None
    if any(w in s for w in PADLIKE_WORDS):
        return "padlike"
    if any(w in s for w in BASS_WORDS):
        return "bass"
    return None


def scan(root):
    """(labelled, ambiguous) lists of (path, features, label)."""
    labelled, ambiguous = [], []
    for f in sorted(os.listdir(root)):
        if not f.lower().endswith(".mid"):
            continue
        path = os.path.join(root, f)
        fe = features(path)
        if fe is None:
            continue
        lab = label_of(path)
        s = " " + stem(path) + " "
        if lab is None and any(w in s for w in AMBIGUOUS):
            ambiguous.append((path, fe, None))
        elif lab is not None:
            labelled.append((path, fe, lab))
    return labelled, ambiguous


# ------------------------------------------------------------------- a logistic regression by hand

def standardise(rows):
    mu = {k: sum(r[k] for r in rows) / len(rows) for k in FEATURES}
    sd = {k: max(1e-6, math.sqrt(sum((r[k] - mu[k]) ** 2 for r in rows) / len(rows))) for k in FEATURES}
    return mu, sd


def fit(rows, ys, mu, sd, epochs=6000, lr=1.0):
    """Full-batch gradient descent in NumPy. The first version ran 400 pure-Python epochs, which is
    nowhere near convergence on 1500 examples and produced a classifier that confused melodic lines
    with basslines 39 % of the time; the fix is arithmetic, not modelling."""
    import numpy as np
    X = np.array([[(r[k] - mu[k]) / sd[k] for k in FEATURES] for r in rows], dtype=np.float64)
    y = np.array(ys, dtype=np.float64)
    w = np.zeros(len(FEATURES))
    b = 0.0
    for _ in range(epochs):
        z = np.clip(X @ w + b, -30.0, 30.0)
        d = 1.0 / (1.0 + np.exp(-z)) - y
        w -= lr * (X.T @ d) / len(y)
        b -= lr * d.mean()
    return {k: float(w[i]) for i, k in enumerate(FEATURES)}, float(b)


def predict(r, w, b, mu, sd):
    z = b + sum(w[k] * (r[k] - mu[k]) / sd[k] for k in FEATURES)
    return 1.0 / (1.0 + math.exp(-max(-30.0, min(30.0, z))))


def summarise(name, rows):
    if not rows:
        print(f"  {name:26s} (none)")
        return
    out = []
    for k in FEATURES:
        v = sorted(r[k] for r in rows)
        out.append(f"{k} {v[len(v)//2]:.2f}")
    print(f"  {name:26s} n={len(rows):5d}  " + "  ".join(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--folder", default=STAR)
    ap.add_argument("--threshold", type=float, default=0.9)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--melodic-roles", default="acid,lead,arp",
                    help="which known roles count as the melodic class; 'lead' alone asks the "
                         "narrower question, because an acid line legitimately sits low and repeats "
                         "and is therefore hard to tell from a bassline by content")
    a = ap.parse_args()
    global MELODIC_ROLES
    MELODIC_ROLES = a.melodic_roles.split(",")

    root = os.path.join(a.root, a.folder)
    labelled, ambiguous = scan(root)
    classes = ("melodic", "bass", "padlike")
    by_class = {c: [f for _p, f, l in labelled if l == c] for c in classes}
    print(f"{root}")
    print("  labelled by name: " + ", ".join(f"{c} {len(by_class[c])}" for c in classes)
          + f"; ambiguous ('synth ...'): {len(ambiguous)}")

    # One-vs-rest logistic regression per class, each fitted on two thirds of the labelled files and
    # scored on the third the fit never saw. The predicted class is the highest of the three scores.
    rows = [(f, l) for _p, f, l in labelled]
    rows.sort(key=lambda t: (t[1], t[0]["mean_pitch"], t[0]["span"]))
    train = [t for i, t in enumerate(rows) if i % 3 != 0]
    test = [t for i, t in enumerate(rows) if i % 3 == 0]
    mu, sd = standardise([r for r, _l in train])
    heads = {c: fit([r for r, _l in train], [1 if l == c else 0 for _r, l in train], mu, sd)
             for c in classes}

    def classify(r):
        s = {c: predict(r, w, b, mu, sd) for c, (w, b) in heads.items()}
        return max(s, key=s.get), s

    conf = Counter()
    for r, l in test:
        conf[(l, classify(r)[0])] += 1
    print(f"  held-out third, {len(test)} files. Confusion (rows = name, columns = content):")
    print("      " + "".join(f"{c:>10s}" for c in classes) + "      precision of the melodic column")
    for tru in classes:
        print(f"  {tru:>10s}" + "".join(f"{conf[(tru, pre)]:10d}" for pre in classes))
    mel_pred = sum(conf[(t, 'melodic')] for t in classes)
    mel_true = sum(conf[('melodic', p)] for p in classes)
    print(f"  precision of 'melodic' {100 * conf[('melodic', 'melodic')] / max(1, mel_pred):.1f} %, "
          f"recall {100 * conf[('melodic', 'melodic')] / max(1, mel_true):.1f} %")
    print("  melodic-head weights:", ", ".join(f"{k} {heads['melodic'][0][k]:+.2f}" for k in FEATURES))

    if a.report:
        print("\n  medians of the eight features:")
        for c in classes:
            summarise(f"name says {c}", by_class[c])
        summarise("ambiguous 'synth ...'", [f for _p, f, _l in ambiguous])
        by_suffix = defaultdict(list)
        for p, f, _l in ambiguous:
            m = re.search(r"(synth|drone|pad|pads|bass)s? layer", stem(p))
            by_suffix[m.group(0) if m else "no layer word"].append(f)
        for k in sorted(by_suffix, key=lambda x: -len(by_suffix[x])):
            summarise("  " + k, by_suffix[k])

    got = Counter(classify(f)[0] for _p, f, _l in ambiguous)
    print(f"\n  the {len(ambiguous)} ambiguous files classify as: "
          + ", ".join(f"{c} {got[c]} ({100 * got[c] / max(1, len(ambiguous)):.1f} %)" for c in classes))
    margin = sorted(sorted(classify(f)[1].values())[-1] - sorted(classify(f)[1].values())[-2]
                    for _p, f, _l in ambiguous)
    print(f"  median margin between the best and the second class: {margin[len(margin)//2]:.3f}")

    # The transparent rule, next to the fitted one. Two features carry the whole decision and both
    # are readable: a pad or chord file is polyphonic and holds its notes, a bassline barely moves.
    # A rule that can be read off the medians is worth more here than a classifier that cannot,
    # because it is the rule that would go into build_corpus.role_of if the class were admitted.
    def rule(r):
        if r["poly"] >= 1.5 or r["med_len"] >= 4 or r["chord_share"] >= 0.3:
            return "padlike"
        if r["move"] < 0.45 or r["mean_pitch"] < 56.0:
            return "bass"
        return "melodic"

    rconf = Counter()
    for r, l in test:
        rconf[(l, rule(r))] += 1
    print("\n  the same held-out third under the readable rule:")
    print("      " + "".join(f"{c:>10s}" for c in classes))
    for tru in classes:
        print(f"  {tru:>10s}" + "".join(f"{rconf[(tru, pre)]:10d}" for pre in classes))
    rp = sum(rconf[(t, "melodic")] for t in classes)
    print(f"  precision of 'melodic' {100 * rconf[('melodic', 'melodic')] / max(1, rp):.1f} %, "
          f"recall {100 * rconf[('melodic', 'melodic')] / max(1, mel_true):.1f} %")
    rgot = Counter(rule(f) for _p, f, _l in ambiguous)
    print("  the ambiguous files under the rule: "
          + ", ".join(f"{c} {rgot[c]} ({100 * rgot[c] / max(1, len(ambiguous)):.1f} %)" for c in classes))
    both = sum(1 for _p, f, _l in ambiguous if rule(f) == "melodic" and classify(f)[0] == "melodic")
    print(f"  admitted by BOTH the rule and the classifier: {both} "
          f"({100 * both / max(1, len(ambiguous)):.1f} %)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
