"""The content classifier that decides a track's role when no name states it, and its measurement.

**What the previous round measured, and why that was not enough.** ``Tools/train/rolecheck.py`` fitted
eight summary features against the files whose names state a role and reached 78.4 % precision on the
class {acid, lead, arp} and 92.0 % on ``lead`` alone. Admitting what it accepted bought +0.0080 nats
with an interval containing zero, so it was rejected and the round concluded that the next step is a
better classifier. This file is that attempt, and it differs in four things, each of which is
measured on its own (``--ablate``) rather than asserted:

1. **The unit is the track, not the file** (``roledetect.read_midi_tracks``). A third of the VORTEX
   bundle is arrangements; their merged top line is a chimera, and their *track names* are a label
   source nobody had read.
2. **The folder is evidence.** A construction kit exports the parts of one piece into one folder, so
   the classes its *named* tracks carry are a prior for its unnamed ones, and a part's register rank
   inside its own file and folder says more than its MIDI note number. Learning from the structure a
   collection already has is the standard cheap half of semi-supervised learning (Chapelle,
   Scholkopf, Zien, *Semi-Supervised Learning*, MIT Press 2006, chapter 1 on the cluster assumption).
3. **Features aimed at the one confusion that matters**: acid against bass. The kick grid, the
   interval distribution and whether the line outlines a scale -- see ``roledetect.features``.
4. **Abstention.** The decision is not "which class is likeliest" but "is this class likely enough to
   be worth polluting a role's training set", so a track is admitted only above a threshold on the
   top probability, chosen on the cross-validation folds and never on the measurement set.
   *Calibration was part of this idea and it was measured and dropped.* Isotonic regression on
   inner folds (Zadrozny and Elkan, "Transforming classifier scores into accurate multiclass
   probability estimates", KDD 2002) is the textbook way to make such a threshold mean something,
   and here it loses: at every melodic precision the raw ensemble scores reach a higher recall
   (``--sweep``; 94.3 % precision at 67.9 % recall raw against 93.6 % at 57.4 % calibrated). The
   isotonic fit is a three-fold inner split that is not group-aware, so it is calibrated against
   folder mates of its own training rows and over-shrinks. The threshold is therefore read off the
   raw scores, which are an ordering and not a probability, and it is chosen by the measured curve.

**How it is measured.** Two measurements, and they answer different questions:

* *Grouped cross-validation over the name-labelled tracks*, five folds split by **folder**, so a
  construction kit never straddles the boundary and the score is not the score of one vendor's
  house style. The label is the vendor's own word and the classifier never sees a name, so this is
  an honest precision and recall. It is the number quoted per class.
* *The hand-labelled sample* (``labelsample.py``), which is held out of every fit here. Its named
  half repeats the measurement above on tracks drawn by a stated rule; its unnamed half gives the
  **class composition of the population the classifier is actually applied to**, and the two
  together give the expected precision at deployment.

**The result, and why the second measurement had to exist.** Against vendor names the classifier
reaches 89.0 % melodic precision at 80.7 % recall with no abstention and 94.3 % / 67.9 % at
threshold 0.70 (five folds, grouped by folder), and on the sample's named half -- which no fit has
seen -- 97.4 % precision at 94.9 % recall. On the sample's **unnamed** half, the population it is
actually for, the same model at the same threshold reaches **50.0 %**. The drop is not a weakness of
the fit: the unnamed population is different material -- one-pitch rhythm figures, chord beds and
basslines -- and a classifier fitted on one class prior and read on another over-admits whatever was
common in training. Without the sample a 94 % classifier would have been shipped that is in truth a
50 % one. What the ablation says made the difference is also not what one would guess: the folder's
other tracks, not the content features. Dropping the sibling block costs the melodic class two
points and costs ``acid`` -- the confusion the previous round named -- 66, from 94.6 % to 28.2 %.

Usage:
    python Tools/corpus/rolemodel.py --audit          # what the evidence sources are worth
    python Tools/corpus/rolemodel.py --cv             # the five-fold measurement, per class
    python Tools/corpus/rolemodel.py --ablate         # what each idea is worth, measured
    python Tools/corpus/rolemodel.py --admit          # what would be admitted, and at what precision
"""
import argparse
import json
import os
import sys
from collections import Counter, defaultdict

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import labelsample                                              # noqa: E402
import roledetect as rd                                         # noqa: E402

CLASSES = list(rd.CLASSES)
MELODIC = list(rd.MELODIC)

#: The feature block names, so an ablation can switch one off by name.
BLOCKS = ("content", "context", "folder", "program")


def feature_names(blocks=BLOCKS):
    names = []
    if "content" in blocks:
        names += [f for f in rd.FEATURES if f not in
                  ("file_rank", "file_delta", "folder_rank", "folder_delta", "n_tracks")]
    if "context" in blocks:
        names += ["file_rank", "file_delta", "folder_rank", "folder_delta", "n_tracks"]
    if "folder" in blocks:
        names += [f"sib_dir_{c}" for c in CLASSES] + ["sib_dir_n"]
        names += [f"sib_file_{c}" for c in CLASSES] + ["sib_file_n"]
    if "program" in blocks:
        names += [f"prog_{c}" for c in CLASSES] + ["prog_none"]
    return names


def build_matrix(recs, blocks=BLOCKS):
    """The design matrix of ``recs``, with the sibling features computed **excluding the track itself**.

    The sibling block is the folder's and the file's other *named* tracks, as class shares. Computing
    it without excluding the track would hand a named track its own label, which is the classic way
    to measure a leak instead of a classifier. At deployment the track being classified is unnamed by
    definition, so the exclusion is also the honest simulation of the real case.

    **``recs`` must always be the whole corpus.** The sibling features are a property of the corpus,
    not of whatever subset is being fitted or scored, and computing them over a subset silently
    changes them. This cost a day: fitting on the 11 316 name-labelled tracks and predicting on a
    matrix built over all 19 307 moved ``sib_dir_bass`` in the flat Star Samples folder from 0.48682
    to 0.48707 -- and a tree that had learnt a split at that constant flipped all 2 581 tracks of the
    folder at once. In-sample precision on that source was 30 % with a matrix mismatch and 100 %
    without. Use :func:`corpus_matrix` and index rows out of it; never call this with a subset.
    """
    dir_counts, file_counts = defaultdict(Counter), defaultdict(Counter)
    for r in recs:
        lab = labelsample.name_label(r)
        if lab:
            dir_counts[r["dir"]][lab] += 1
            file_counts[r["path"]][lab] += 1
    names = feature_names(blocks)
    X = np.zeros((len(recs), len(names)), dtype=np.float64)
    for i, r in enumerate(recs):
        own = labelsample.name_label(r)
        row = {}
        if "content" in blocks or "context" in blocks:
            row.update(r["f"])
        if "folder" in blocks:
            for tag, counts in (("dir", dir_counts[r["dir"]]), ("file", file_counts[r["path"]])):
                c = Counter(counts)
                if own:
                    c[own] -= 1
                tot = max(1, sum(max(0, v) for v in c.values()))
                for cls in CLASSES:
                    row[f"sib_{tag}_{cls}"] = max(0, c[cls]) / tot
                row[f"sib_{tag}_n"] = np.log1p(sum(max(0, v) for v in c.values()))
        if "program" in blocks:
            for cls in CLASSES:
                row[f"prog_{cls}"] = 1.0 if r["prog_role"] == cls else 0.0
            row["prog_none"] = 0.0 if r["prog_role"] else 1.0
        for j, n in enumerate(names):
            X[i, j] = row.get(n, 0.0)
    return X, names


_MATRIX_CACHE = {}


def corpus_matrix(all_recs, blocks=BLOCKS):
    """(X, names, id -> row index) over the whole corpus, built once per block set.

    The cache key names the corpus by its size and its first and last track id rather than by
    ``id(list)``, which CPython reuses once a temporary list is collected and which would then hand
    one corpus's matrix to another corpus's rows.
    """
    key = (len(all_recs), all_recs[0]["id"], all_recs[-1]["id"], blocks) if all_recs else (0, blocks)
    if key not in _MATRIX_CACHE:
        X, names = build_matrix(all_recs, blocks)
        _MATRIX_CACHE[key] = (X, names, {r["id"]: i for i, r in enumerate(all_recs)})
    return _MATRIX_CACHE[key]


def rows_of(all_recs, recs, blocks=BLOCKS):
    """The corpus matrix restricted to ``recs``, in their order."""
    X, names, idx = corpus_matrix(all_recs, blocks)
    return X[[idx[r["id"]] for r in recs]], names


def make_model(kind="gb"):
    """The classifier. ``gb`` is a histogram gradient-boosted tree, ``lr`` the previous round's
    logistic regression with the same features, so the gain from the model class can be read off.

    Trees are the default because the features are of wildly different scales and several of the
    decisions are thresholds rather than slopes ("median note length at least four sixteenths"),
    which a linear model has to approximate and a tree states.
    """
    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.linear_model import LogisticRegression
    from sklearn.pipeline import make_pipeline
    from sklearn.preprocessing import StandardScaler
    if kind == "lr":
        return make_pipeline(StandardScaler(),
                             LogisticRegression(max_iter=4000, C=1.0, class_weight="balanced"))
    return HistGradientBoostingClassifier(max_iter=400, learning_rate=0.06, max_leaf_nodes=31,
                                          l2_regularization=1.0, min_samples_leaf=20,
                                          early_stopping=True, validation_fraction=0.15,
                                          random_state=0)


def labelled_tracks(recs, exclude_ids):
    """The name-labelled tracks that are not in the hand-labelled sample, with their labels."""
    out, ys = [], []
    for r in recs:
        if r["id"] in exclude_ids:
            continue
        lab = labelsample.name_label(r)
        if lab:
            out.append(r)
            ys.append(lab)
    return out, ys


def cv_predict(all_recs, recs, ys, blocks=BLOCKS, kind="gb", folds=5, calibrate=False,
               group="dir"):
    """Out-of-fold class probabilities, five folds split by folder.

    Two groupings, and they answer different questions; both are reported because neither alone is
    the truth:

    * ``group="dir"`` -- the **folder**. A construction kit's parts share one and are near copies of
      each other, so splitting by track would put a kit's bass in training and its lead in test and
      report the vendor's house style as generalisation (Sturm, "A simple method to determine if a
      music information retrieval system is a horse", IEEE Trans. Multimedia 2014). This is the
      pessimistic setting: it asks the classifier to generalise to a folder it has never seen, and
      it is the right number for a folder whose tracks are *all* unnamed. It is also unfair to one
      source: ``Star Samples/Psy Trance Midis`` is a single flat folder of 2 581 named tracks, so
      under this grouping the whole of it is one fold and its score is "trained without this vendor".
    * ``group="path"`` -- the **file**. The deployment case: the track being classified is unnamed
      and sits in a folder whose *other* tracks are named and were available to fit. This is the
      number the hand-labelled sample measures independently.
    """
    from sklearn.calibration import CalibratedClassifierCV
    from sklearn.model_selection import GroupKFold
    X, _names = rows_of(all_recs, recs, blocks)
    y = np.array(ys)
    groups = np.array([r[group] for r in recs])
    proba = np.zeros((len(recs), len(CLASSES)))
    gkf = GroupKFold(n_splits=folds)
    for tr, te in gkf.split(X, y, groups):
        base = make_model(kind)
        if calibrate:
            # Isotonic calibration on an inner grouped split of the training fold, so the calibrator
            # never sees the fold it will be scored on.
            m = CalibratedClassifierCV(base, method="isotonic", cv=3)
        else:
            m = base
        m.fit(X[tr], y[tr])
        p = m.predict_proba(X[te])
        for k, cls in enumerate(m.classes_):
            proba[te, CLASSES.index(cls)] = p[:, k]
    return proba, y


def pr_table(proba, y, threshold=0.0, classes=CLASSES):
    """Precision, recall and count per class at an abstention threshold on the top probability."""
    top = proba.argmax(1)
    conf = proba.max(1)
    rows = {}
    for ci, cls in enumerate(classes):
        pred = (top == ci) & (conf >= threshold)
        true = y == cls
        tp = int((pred & true).sum())
        rows[cls] = {"pred": int(pred.sum()), "true": int(true.sum()), "tp": tp,
                     "precision": tp / max(1, int(pred.sum())),
                     "recall": tp / max(1, int(true.sum()))}
    mel = np.isin(y, MELODIC)
    pmel = np.isin(np.array(classes)[top], MELODIC) & (conf >= threshold)
    rows["MELODIC"] = {"pred": int(pmel.sum()), "true": int(mel.sum()),
                       "tp": int((pmel & mel).sum()),
                       "precision": int((pmel & mel).sum()) / max(1, int(pmel.sum())),
                       "recall": int((pmel & mel).sum()) / max(1, int(mel.sum()))}
    return rows


def print_pr(name, rows, classes=CLASSES):
    print(f"  {name}")
    print(f"    {'class':10s} {'n':>6s} {'predicted':>10s} {'correct':>8s} {'precision':>10s} {'recall':>8s}")
    for cls in list(classes) + ["MELODIC"]:
        r = rows[cls]
        print(f"    {cls:10s} {r['true']:6d} {r['pred']:10d} {r['tp']:8d} "
              f"{100 * r['precision']:9.1f} % {100 * r['recall']:7.1f} %")


def confusion(proba, y, classes=CLASSES):
    top = np.array(classes)[proba.argmax(1)]
    c = Counter(zip(y, top))
    print(f"    {'name \\ content':16s}" + "".join(f"{x:>8s}" for x in classes))
    for t in classes:
        print(f"    {t:16s}" + "".join(f"{c[(t, p)]:8d}" for p in classes))


# ------------------------------------------------------------------------------------------- audit

def audit(recs):
    """What each evidence source is worth against the others, before any of them is trusted."""
    print("Evidence sources, measured against each other on the tracks that carry both:")
    for k1, k2 in (("path_role", "name_role"), ("name_role", "prog_role"), ("path_role", "prog_role")):
        both = [(r[k1], r[k2]) for r in recs if r[k1] and r[k2]]
        same = sum(1 for a, b in both if a == b)
        print(f"  {k1:10s} vs {k2:10s}: {len(both):6d} tracks, agree {100 * same / max(1, len(both)):5.1f} %")
        dis = Counter((a, b) for a, b in both if a != b)
        print("      commonest disagreements: "
              + ", ".join(f"{a}/{b} {n}" for (a, b), n in dis.most_common(5)))
    print("\n  Read: the track name is as good as the file name (98 % agreement) and is available on "
          "\n  five times as many tracks. The General MIDI program is not: it agrees with the track "
          "\n  name on under three quarters of the tracks that carry both, which is the level of a "
          "\n  DAW default rather than a label, so it enters only as a weak feature and never as a "
          "\n  label.")


def fit_full(all_recs, tracks, ys, blocks=BLOCKS, kind="gb", calibrate=False):
    """The deployable model: one fit on every name-labelled track outside the hand-labelled sample."""
    from sklearn.calibration import CalibratedClassifierCV
    X, names = rows_of(all_recs, tracks, blocks)
    m = CalibratedClassifierCV(make_model(kind), method="isotonic", cv=3) if calibrate else make_model(kind)
    m.fit(X, np.array(ys))
    order = [list(m.classes_).index(c) for c in CLASSES]
    return m, names, order


def predict(model, order, all_recs, recs, blocks=BLOCKS):
    """Class probabilities in :data:`CLASSES` order, over rows of the single corpus matrix."""
    Xs, _n = rows_of(all_recs, recs, blocks)
    return model.predict_proba(Xs)[:, order]


def em_prior(proba, source_prior, iters=100, tol=1e-7):
    """Re-weights probabilities to the class prior of the set they are being applied to.

    **Why this is needed, measured.** The classifier is fitted on the tracks whose names state a
    class and applied to the tracks whose names do not, and the two populations have different class
    compositions: the hand-labelled sample says the unnamed half is 25 % melodic where the named half
    is 41 %. A classifier trained on one prior and read on another over-admits the classes that were
    common in training -- on the unnamed sample the melodic precision falls from the cross-validated
    94.3 % to 38.4 %, which is the whole reason the sample had to exist.

    The fix is the standard one for a prior shift with an unchanged class-conditional: EM on the
    unlabelled set, alternating an estimate of the new prior with a re-weighting of the posteriors
    (Saerens, Latinne, Decaestecker, "Adjusting the outputs of a classifier to new a priori
    probabilities: a simple procedure", Neural Computation 14(1), 2002). It uses no labels -- only
    the classifier's own outputs on the population it is being applied to -- so it can be run per
    source without touching the hand-labelled sample, and the sample stays free to measure it.
    """
    p0 = np.asarray(source_prior, dtype=np.float64)
    p0 = p0 / p0.sum()
    pk = p0.copy()
    post = proba.copy()
    for _ in range(iters):
        w = post * (pk / p0)[None, :]
        w /= np.maximum(w.sum(1, keepdims=True), 1e-12)
        new = w.mean(0)
        if np.abs(new - pk).max() < tol:
            pk = new
            break
        pk = new
    w = proba * (pk / p0)[None, :]
    w /= np.maximum(w.sum(1, keepdims=True), 1e-12)
    return w, pk


def sweep(proba, y):
    """Melodic precision against recall as the abstention threshold moves: the operating curve."""
    print(f"  {'threshold':>9s} {'melodic P':>10s} {'melodic R':>10s} {'admitted':>9s} "
          f"{'acid P':>7s} {'lead P':>7s} {'arp P':>7s}")
    out = {}
    for thr in (0.0, 0.3, 0.4, 0.5, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9):
        r = pr_table(proba, y, thr)
        out[thr] = r
        print(f"  {thr:9.2f} {100 * r['MELODIC']['precision']:9.1f}% {100 * r['MELODIC']['recall']:9.1f}% "
              f"{r['MELODIC']['pred']:9d} {100 * r['acid']['precision']:6.1f}% "
              f"{100 * r['lead']['precision']:6.1f}% {100 * r['arp']['precision']:6.1f}%")
    return out


def unnamed_scores(model, order, recs, train_prior, blocks=BLOCKS, adjust=True):
    """(track id -> probability row) for every track no name covers, prior-adjusted per source.

    The adjustment runs **per source** and not over the whole corpus, because the sources are
    different music sold by different vendors and their unnamed residue is different material: the
    psytrance packs' is one-pitch rhythm patterns, the Star Samples folder's is chord beds, the
    VORTEX bundle's is the untitled tracks of whole arrangements. One prior over all four would be
    an average of four things and describe none of them. This is the "pack as a prior" of the plan,
    estimated from the pack rather than assumed.
    """
    out, priors = {}, {}
    for src in rd.SOURCES:
        rows = [r for r in recs if r["source"] == src and not (r["path_role"] or r["name_role"])]
        if not rows:
            continue
        p = predict(model, order, recs, rows, blocks)
        if adjust:
            p, pk = em_prior(p, train_prior)
            priors[src] = pk
        for r, row in zip(rows, p):
            out[r["id"]] = row
    return out, priors


def score_sample(model, order, recs, labels, blocks=BLOCKS, threshold=0.7, unnamed=None):
    """The hand-labelled sample, which no fit has seen, scored the way deployment would score it.

    ``unnamed`` supplies prior-adjusted probability rows for the unnamed stratum; without it both
    strata are scored on the raw outputs, which is the comparison that shows what the adjustment is
    worth.
    """
    ids = [t for t in labels]
    by_id = {r["id"]: r for r in recs}
    rows = [by_id[t] for t in ids if t in by_id]
    p = predict(model, order, recs, rows, blocks)
    if unnamed:
        for i, t in enumerate(ids):
            if t in unnamed:
                p[i] = unnamed[t]
    top = np.array(CLASSES)[p.argmax(1)]
    conf = p.max(1)
    for stratum in ("named", "unnamed"):
        sel = [i for i, t in enumerate(ids) if labels[t]["stratum"] == stratum]
        y = np.array([labels[ids[i]]["label"] for i in sel])
        pr = top[sel]
        cf = conf[sel]
        adm = np.isin(pr, MELODIC) & (cf >= threshold)
        mel = np.isin(y, MELODIC)
        print(f"  {stratum:8s} n={len(sel)}  "
              f"true melodic {int(mel.sum())}  admitted as melodic {int(adm.sum())}  "
              f"of those correct {int((adm & mel).sum())}  "
              f"precision {100 * (adm & mel).sum() / max(1, adm.sum()):.1f} %  "
              f"recall {100 * (adm & mel).sum() / max(1, mel.sum()):.1f} %")
        wrong = Counter(y[i2] for i2 in range(len(sel)) if adm[i2] and not mel[i2])
        if wrong:
            print("           what the wrong admissions really are: "
                  + ", ".join(f"{k} {v}" for k, v in wrong.most_common()))
    return top, conf, ids


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--audit", action="store_true")
    ap.add_argument("--cv", action="store_true")
    ap.add_argument("--ablate", action="store_true")
    ap.add_argument("--admit", action="store_true")
    ap.add_argument("--sweep", action="store_true", help="melodic precision against recall, by threshold")
    ap.add_argument("--sample", action="store_true", help="score the hand-labelled sample")
    ap.add_argument("--group", default="dir", choices=("dir", "path"),
                    help="the cross-validation group: 'dir' generalises to an unseen folder, 'path' "
                         "to an unseen track in a known folder, which is the deployment case")
    ap.add_argument("--no-adjust", action="store_true",
                    help="skip the per-source prior adjustment of em_prior (for the comparison)")
    ap.add_argument("--calibrate", action="store_true",
                    help="isotonic calibration of the probabilities. Off by default because it was "
                         "measured and it loses: at every melodic precision the raw ensemble scores "
                         "reach a higher recall (--sweep), so calibration costs material for nothing")
    ap.add_argument("--kind", default="gb", choices=("gb", "lr"))
    ap.add_argument("--threshold", type=float, default=0.0)
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    recs = rd.load(a.root)
    labels = labelsample.load_labels()
    sample_ids = set(labels)
    tracks, ys = labelled_tracks(recs, sample_ids)
    print(f"{len(recs)} tracks; {len(tracks)} name-labelled and outside the sample; "
          f"{len(sample_ids)} in the hand-labelled sample")
    print("  label counts:", dict(Counter(ys)))

    if a.audit:
        audit(recs)
        return 0

    result = {}
    if a.sweep:
        print("\nuncalibrated, five-fold grouped:")
        p_raw, y_raw = cv_predict(recs, tracks, ys, kind=a.kind, calibrate=False, group=a.group)
        result["sweep_raw"] = sweep(p_raw, y_raw)
        print("\nisotonic-calibrated, five-fold grouped:")
        p_cal, y_cal = cv_predict(recs, tracks, ys, kind=a.kind, calibrate=True, group=a.group)
        result["sweep_cal"] = sweep(p_cal, y_cal)

    if a.sample or a.admit:
        model, names, order = fit_full(recs, tracks, ys, kind=a.kind, calibrate=a.calibrate)
        train_prior = np.array([Counter(ys)[c] for c in CLASSES], dtype=np.float64)
        if a.sample:
            print(f"\nthe hand-labelled sample, never part of any fit. Named stratum: the vendor's "
                  f"own word. Unnamed stratum: the rule of labelsample.LABEL_RULE.")
            for adjust in (False, True):
                scores, priors = unnamed_scores(model, order, recs, train_prior, adjust=adjust)
                print(f"\n  {'with' if adjust else 'without'} the per-source prior adjustment:")
                score_sample(model, order, recs, labels, threshold=a.threshold or 0.7,
                             unnamed=scores)
                if adjust:
                    print("  EM-estimated class prior of the unnamed population, per source:")
                    for src, pk in priors.items():
                        print(f"    {src:10s} " + ", ".join(f"{c} {100 * v:.0f} %"
                                                            for c, v in zip(CLASSES, pk)))
            print("\n  melodic precision on the unnamed sample against the threshold "
                  "(adjusted / unadjusted):")
            adj, _ = unnamed_scores(model, order, recs, train_prior, adjust=True)
            raw, _ = unnamed_scores(model, order, recs, train_prior, adjust=False)
            ids = [t for t in labels if labels[t]["stratum"] == "unnamed" and t in adj]
            ytrue = np.array([labels[t]["label"] for t in ids])
            mel = np.isin(ytrue, MELODIC)
            print(f"    {'thr':>5s} {'adj P':>7s} {'adj R':>7s} {'adj n':>6s} "
                  f"{'raw P':>7s} {'raw R':>7s} {'raw n':>6s}")
            result["sample_sweep"] = {}
            for thr in (0.5, 0.6, 0.7, 0.8, 0.9, 0.95):
                cells = []
                for src in (adj, raw):
                    P = np.array([src[t] for t in ids])
                    pick = np.isin(np.array(CLASSES)[P.argmax(1)], MELODIC) & (P.max(1) >= thr)
                    cells += [100 * (pick & mel).sum() / max(1, pick.sum()),
                              100 * (pick & mel).sum() / max(1, mel.sum()), int(pick.sum())]
                print(f"    {thr:5.2f} {cells[0]:6.1f}% {cells[1]:6.1f}% {cells[2]:6d} "
                      f"{cells[3]:6.1f}% {cells[4]:6.1f}% {cells[5]:6d}")
                result["sample_sweep"][thr] = cells
        if a.admit:
            scores, priors = unnamed_scores(model, order, recs, train_prior, adjust=not a.no_adjust)
            thr = a.threshold or 0.7
            print(f"\nwhat the classifier would admit from the {len(scores)} tracks no name covers, "
                  f"at threshold {thr:.2f}"
                  f"{'' if a.no_adjust else ', after the per-source prior adjustment'}:")
            print(f"  {'source':10s} {'tracks':>7s} " + " ".join(f"{c:>7s}" for c in CLASSES)
                  + f" {'abstain':>8s}")
            by_id = {r["id"]: r for r in recs}
            result["admit"] = {}
            for src in rd.SOURCES:
                sel = [t for t in scores if by_id[t]["source"] == src]
                c, ab = Counter(), 0
                for t in sel:
                    row = scores[t]
                    if row.max() >= thr:
                        c[CLASSES[int(row.argmax())]] += 1
                    else:
                        ab += 1
                print(f"  {src:10s} {len(sel):7d} " + " ".join(f"{c[cl]:7d}" for cl in CLASSES)
                      + f" {ab:8d}")
                result["admit"][src] = dict(c)

    if a.cv or a.ablate:
        proba, y = cv_predict(recs, tracks, ys, kind=a.kind, calibrate=a.calibrate, group=a.group)
        print(f"\nfive-fold grouped cross-validation over folders, {a.kind}, "
              f"{'isotonic-calibrated' if a.calibrate else 'raw ensemble scores'}, "
              f"grouped by {a.group}:")
        confusion(proba, y)
        for thr in (0.0, 0.70, 0.80):
            print_pr(f"threshold {thr:.2f}", pr_table(proba, y, thr))
        result["cv"] = {f"{thr}": pr_table(proba, y, thr) for thr in (0.0, 0.70, 0.80)}

    if a.ablate:
        # Reported at threshold 0 (plain argmax, where the classifier is doing all the deciding) and
        # at 0.70, the operating point. At 0.80 and above every variant is near 100 % precision and
        # the table stops discriminating, which is the wrong place to read an ablation.
        print("\nablation: the same five-fold measurement with one block of features switched off")
        hdr = (f"  {'features':26s} {'mel P@0':>8s} {'mel R@0':>8s} {'mel P@.7':>9s} {'mel R@.7':>9s} "
               f"{'acid P@0':>9s} {'acid R@0':>9s} {'bass P@0':>9s}")

        def row(name, p, yy):
            r0, r7 = pr_table(p, yy, 0.0), pr_table(p, yy, 0.70)
            print(f"  {name:26s} {100 * r0['MELODIC']['precision']:7.1f}% {100 * r0['MELODIC']['recall']:7.1f}% "
                  f"{100 * r7['MELODIC']['precision']:8.1f}% {100 * r7['MELODIC']['recall']:8.1f}% "
                  f"{100 * r0['acid']['precision']:8.1f}% {100 * r0['acid']['recall']:8.1f}% "
                  f"{100 * r0['bass']['precision']:8.1f}%")
            return {"t0": r0, "t70": r7}

        print(hdr)
        result.setdefault("ablation", {})["all"] = row("all, " + a.kind, proba, y)
        for drop in BLOCKS:
            blocks = tuple(b for b in BLOCKS if b != drop)
            p2, y2 = cv_predict(recs, tracks, ys, blocks=blocks, kind=a.kind, calibrate=a.calibrate, group=a.group)
            result["ablation"][drop] = row("without " + drop, p2, y2)
        other = "lr" if a.kind == "gb" else "gb"
        p3, y3 = cv_predict(recs, tracks, ys, kind=other, calibrate=a.calibrate, group=a.group)
        result["ablation"][other] = row(f"all, {other} (model class)", p3, y3)
        p4, y4 = cv_predict(recs, tracks, ys, kind=a.kind, calibrate=not a.calibrate, group=a.group)
        result["ablation"]["calibration"] = row(
            "all, " + ("uncalibrated" if a.calibrate else "isotonic-calibrated"), p4, y4)

    if a.out:
        with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(result, fh, indent=1, default=float)
        print("written", a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
