"""The hand-labelled sample the role classifier is measured against.

**Why this exists before the classifier.** The previous corpus round built a content classifier,
measured it against the files whose names state a role, and rejected it. That measurement answers
"how well does content reproduce a name?" -- but the classifier is *applied* to files that have no
name, and those are not a random sample of the ones that do. A vendor writes "Bass 07" on a bass and
"Loop 12" on whatever did not fit a word; the unnamed population has its own class composition, and
precision on a population depends on that composition as much as on the classifier. Without a sample
of the unnamed files there is no way to know the deployment precision, only the one on names.

**The sampling rule, so that the number means something.** The population is every note-carrying
track of the four sources (``roledetect.load``), 19 307 of them. It is cut into eight strata --
four sources x {the path or the track name states a class, neither does} -- and 50 tracks are drawn
from each stratum by simple random sampling without replacement, from a fixed shuffle of the
stratum's track ids under seed 20260916. That is 400 tracks, a proportion of 2.1 % overall but
deliberately *not* proportional between strata: the unnamed strata are the ones the classifier is
for and they get half the sample, while the trance bundle would otherwise swallow it whole (13 997
of the 19 307 tracks). Every number computed from the sample that is meant to describe the whole
corpus is therefore reweighted by the stratum's true size (:func:`stratum_weights`).

**Where the labels come from, and what each half is worth.**

* *Named strata (200 tracks).* The label is the vendor's own word, from the file name or the track
  name. It is ground truth of the only kind available without listening, and it is **independent of
  every feature the classifier sees**, because the classifier is never shown a name. This half is
  the honest measurement of precision and recall.
* *Unnamed strata (200 tracks).* There is no name, so the label is mine, read off an evidence card
  (``--cards``) under the rule written out in :data:`LABEL_RULE` and stored in ``labels.json``. A
  label read from register, polyphony, note length and the kick grid is **not independent** of a
  classifier that reads the same things, so agreement here is not a precision score and is not
  reported as one. What this half is for is the *class composition* of the unnamed population --
  how much of it is bass, pad, drums or unusable -- which is what turns the named-stratum result
  into an expected deployment precision, and which nothing else can supply.

**Nothing of the packs is committed.** ``labels.json`` holds the sha1 of ``relative path|track
index``, the stratum, the label and the numbers the label was read from. No path, no file name, no
note. The sample can be redrawn and rejoined on this machine and nowhere else, which is the point.

Usage:
    python Tools/corpus/labelsample.py --draw            # the sample, and what it is made of
    python Tools/corpus/labelsample.py --cards           # the evidence cards of the unnamed strata
    python Tools/corpus/labelsample.py --status          # how many are labelled, and as what
"""
import argparse
import json
import os
import random
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

import roledetect as rd                                         # noqa: E402

LABELS_PATH = os.path.join(_HERE, "labels.json")
SEED = 20260916
PER_STRATUM = 50

#: The label vocabulary of the sample. The first six are ``roledetect.CLASSES``. ``rhythm`` is the
#: honest answer for a loop of one or two distinct pitches -- ``build_corpus.analyse`` already calls
#: those "a rhythm, not a melody" and counts only their onsets -- and it is a large part of what the
#: unnamed psytrance material actually is. ``unsure`` is what the card did not decide, and it is a
#: real answer: a sample with no unsure in it would mean the labeller guessed.
LABELS = ("acid", "lead", "arp", "bass", "pad", "drum", "rhythm", "unsure")

#: The rule the unnamed strata are labelled under, written down before the labelling began and
#: applied to the evidence cards in order, first match wins. It is the rule a producer would use with
#: the arrangement open and the sound off. It is applied by :func:`rule_label` and then every one of
#: the 200 cards was read against its own rule answer; the overrides are in ``labels.json`` under
#: ``by = "review"`` and counted in the report, because a rule nobody checked is not a hand label.
LABEL_RULE = """
1. drum   -- a drum map written on a melodic channel: median pitch <= 46, the pitches do not fit any
             seven-note scale (scale_fit <= 0.80) and they crawl by semitones (step_share >= 0.55),
             which is what a General MIDI drum map looks like when it is read as pitch.
2. pad    -- the track holds or stacks: median note length >= 4 sixteenths, mean polyphony >= 1.6, or
             a chord under at least 35 % of the onsets. Pad, chord bed, stab and strings are one
             label here, because the model has no role for any of them. A single note held for four
             bars is a drone and belongs here too, which is why this test comes before the next one.
3. one or two pitch classes -- no melodic role can use it (build_corpus.analyse calls it "a rhythm,
             not a melody" and counts only its onsets). Low (median pitch <= 50) it is a bassline,
             which is what a rolling figure on the root is; higher up it is a gate or a stab pattern
             and the label is `rhythm`.
4. bass   -- low (median pitch <= 50) and behaving like a bass: avoiding the kick steps
             (kick_share <= 0.35, the rolling figure between the kicks) or hammering one pitch
             (rep_share >= 0.45).
5. arp    -- a fast even figure that leaps through a chord: density >= 11 onsets per bar, note length
             one sixteenth, span >= 12 semitones, at least 45 % of intervals a leap of a fifth or
             more, and it turns (turn_share >= 0.30) rather than running away.
6. acid   -- the hard case, and the one the previous round could not separate from the bass: middle
             or low register (median pitch <= 62), a sixteenth grid (density >= 8), strong repetition
             (rep_share >= 0.20) but running a scale (distinct_pc >= 5, scale_fit >= 0.90). The scale
             is what a bassline does not do.
7. lead   -- what is left when the track is melodic: it moves (move >= 0.40), is monophonic and does
             not hold.
8. unsure -- everything else: the card does not decide.

The rule was written first, applied to the 400 drawn tracks, and then every one of the 200 cards of
the unnamed strata was read against its own answer. That reading changed the rule once -- rules 2 and
3 were the other way round and in one piece, which called a note held for four bars a "rhythm" and a
one-pitch figure at MIDI 41 the same thing as one at MIDI 74 -- and left 14 individual disagreements,
which are in :data:`OVERRIDES` with the reason. Everything else stands as the rule decided it.
"""

#: The cards where the reader disagreed with :func:`rule_label`, with the reason. Fourteen of 200.
OVERRIDES = {
    # Register or span outside anything a part of this music has: an FX line at MIDI 102, a "lead"
    # spanning 51 semitones in a file of eight drum tracks -- the card does not decide these.
    "efe4c935ff61b309": ("unsure", "median pitch 102, four onsets a bar: an effect, not a part"),
    "8fb271954c329750": ("unsure", "span 51 semitones over two onsets a bar in a drum-heavy file"),
    # Melodic by the letter of rule 7 (it moves, it is monophonic, it does not hold) but far too
    # sparse or too narrow to be a line: two onsets a bar, or a span of four semitones.
    "a422cd3d7e92c7b8": ("unsure", "two onsets a bar over five semitones"),
    "8c108faefaff4e67": ("unsure", "five semitones, scale_fit 0.69: chromatic, and not a melody"),
    "b3929fcf8411c8c7": ("unsure", "three pitch classes over eight semitones, four onsets a bar"),
    "bc217b15f53bda89": ("unsure", "two onsets a bar over four semitones"),
    "c261531ed0f69c6c": ("unsure", "span of two semitones: an ornament or a trill, not a line"),
    "33edb55706692b69": ("unsure", "chord_share 0.38 with every interval a leap: a doubled arp or a "
                                  "chord bed, and the card does not say which"),
    # Chordal by rule 2 but at bass register: an octave-doubled bassline, not a pad.
    "a3411fa9da31c2e1": ("bass", "polyphony 1.71 at median pitch 43, 81 % of intervals octaves"),
    "d771ca795ae8b35a": ("bass", "polyphony 1.67 at median pitch 38, the lower of two tracks"),
    # The EMP acid folder: the named siblings of the same folder are acid seven times over, and these
    # four have the shape (dense sixteenths, mid register, repetitive, scale-fitting) with too few
    # pitch classes or too high a median for rule 6 to fire.
    "6f7f19493677a6dc": ("acid", "acid folder, dense sixteenths, mid register"),
    "a0af95cdaf23d86e": ("acid", "acid folder, median pitch 63 just past rule 6"),
    "fb302889d2aa4e71": ("acid", "acid folder, four pitch classes just short of rule 6"),
    "d3116d737aebb650": ("acid", "acid folder, three pitch classes just short of rule 6"),
}


def rule_label(r):
    """:data:`LABEL_RULE` as code, first match wins. Returns one of :data:`LABELS`."""
    f = r["f"]
    if f["median_pitch"] <= 46 and f["scale_fit"] <= 0.80 and f["step_share"] >= 0.55:
        return "drum"
    if f["med_len"] >= 4 or f["poly"] >= 1.6 or f["chord_share"] >= 0.35:
        return "pad"
    if f["distinct_pc"] <= 2:
        return "bass" if f["median_pitch"] <= 50 else "rhythm"
    if f["median_pitch"] <= 50 and (f["kick_share"] <= 0.35 or f["rep_share"] >= 0.45):
        return "bass"
    if (f["density"] >= 11 and f["med_len"] <= 1 and f["span"] >= 12
            and f["leap_share"] >= 0.45 and f["turn_share"] >= 0.30):
        return "arp"
    if (f["median_pitch"] <= 62 and f["density"] >= 8 and f["rep_share"] >= 0.20
            and f["distinct_pc"] >= 5 and f["scale_fit"] >= 0.90):
        return "acid"
    if f["move"] >= 0.40 and f["poly"] < 1.6 and f["med_len"] < 4:
        return "lead"
    return "unsure"


def hand_label(r):
    """The label of an unnamed track: the rule, then the reader's override where there is one."""
    if r["id"] in OVERRIDES:
        return OVERRIDES[r["id"]][0], "review"
    return rule_label(r), "rule"


def strata(recs):
    """The eight strata: source x whether a name -- path or track -- states a class.

    The program change is deliberately not part of the definition. It was measured against the track
    name on 2 512 tracks that carry both and agrees on 73.8 % of them, so it is not a name-grade
    label source and a stratum built on it would be built on noise (``rolemodel.py --audit``).
    """
    out = defaultdict(list)
    for r in recs:
        named = bool(r["path_role"] or r["name_role"])
        out[(r["source"], "named" if named else "unnamed")].append(r)
    return out


def draw(recs, per_stratum=PER_STRATUM, seed=SEED):
    """The stratified sample: ``per_stratum`` tracks from each stratum, without replacement.

    The draw is over the sorted track ids, so it depends on the corpus and the seed and on nothing
    about the order a directory walk happened to return.
    """
    out = {}
    for key, rs in sorted(strata(recs).items()):
        ids = sorted(r["id"] for r in rs)
        rng = random.Random(f"{seed}|{key[0]}|{key[1]}")
        rng.shuffle(ids)
        out[key] = ids[:min(per_stratum, len(ids))]
    return out


def stratum_weights(recs, per_stratum=PER_STRATUM):
    """(stratum -> population size / sample size), for reweighting a sample statistic to the corpus."""
    return {k: len(rs) / max(1, min(per_stratum, len(rs))) for k, rs in strata(recs).items()}


def load_labels():
    if not os.path.exists(LABELS_PATH):
        return {}
    with open(LABELS_PATH, "r", encoding="utf-8") as fh:
        return json.load(fh)["labels"]


def save_labels(labels, note=""):
    with open(LABELS_PATH, "w", encoding="utf-8", newline="\n") as fh:
        json.dump({"seed": SEED, "per_stratum": PER_STRATUM, "rule": LABEL_RULE.strip(),
                   "note": note, "labels": labels}, fh, indent=1, sort_keys=True)
    print(f"written {LABELS_PATH}: {len(labels)} labels")


def name_label(r):
    """The class the vendor's own words state for a named track: the track name wins over the path.

    The track name is preferred because it is written on the part and the path is written on the
    file, and where a file is one part the two agree on 98.2 % of the 3 685 tracks that carry both
    (``rolemodel.py --audit``). The 66 that disagree are parts that are genuinely two things at once
    -- a "progressive psy stab" whose track is called "lead" -- and picking the inner name for those
    is a choice, not a measurement.
    """
    return r["name_role"] or r["path_role"]


def card(r, sib_file, sib_dir):
    """One line of evidence for a track whose name says nothing, for labelling by hand.

    Everything on the card is a number or a class word of a *sibling*; the track's own name and path
    are not on it, because they say nothing by construction and a folder name would leak the vendor's
    genre into a judgement that is supposed to be about the notes.
    """
    f = r["f"]
    sf = ", ".join(f"{c}@{int(p)}" for c, p in sorted(sib_file, key=lambda t: t[1])) or "-"
    sd = ", ".join(f"{c}x{n}" for c, n in sorted(sib_dir.items())) or "-"
    return (f"{r['id']} {rule_label(r):6s} {r['source'][:5]:5s} trk{r['track']:<2d}/{int(f['n_tracks']):<2d} "
            f"pit{int(f['median_pitch']):3d} spn{int(f['span']):3d} pc{int(f['distinct_pc']):2d} "
            f"fr{f['file_rank']:.2f} dr{f['folder_rank']:.2f} "
            f"poly{f['poly']:.2f} ch{f['chord_share']:.2f} len{f['med_len']:.0f} lng{f['long_share']:.2f} "
            f"den{f['density']:5.1f} kick{f['kick_share']:.2f} mov{f['move']:.2f} rep{f['rep_share']:.2f} "
            f"stp{f['step_share']:.2f} lep{f['leap_share']:.2f} trn{f['turn_share']:.2f} "
            f"sc{f['scale_fit']:.2f} ent{f['pc_entropy']:.2f} | in-file: {sf} | folder: {sd}")


def siblings(recs):
    """(by track id) the named classes of the other tracks of the same file, and of the same folder."""
    by_file, by_dir = defaultdict(list), defaultdict(Counter)
    for r in recs:
        lab = name_label(r)
        if lab:
            by_file[r["path"]].append((r["id"], lab, r["f"]["median_pitch"]))
            by_dir[r["dir"]][lab] += 1
    sf, sd = {}, {}
    for r in recs:
        sf[r["id"]] = [(c, p) for i, c, p in by_file.get(r["path"], []) if i != r["id"]]
        d = Counter(by_dir.get(r["dir"], {}))
        own = name_label(r)
        if own:
            d[own] -= 1
            if d[own] <= 0:
                del d[own]
        sd[r["id"]] = d
    return sf, sd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--draw", action="store_true")
    ap.add_argument("--cards", action="store_true", help="the evidence cards of the unnamed strata")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--write", action="store_true", help="write labels.json from the names and the rule")
    ap.add_argument("--stratum", default="", help="restrict --cards to one source")
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--limit", type=int, default=1000)
    a = ap.parse_args()

    recs = rd.load(a.root)
    by_id = {r["id"]: r for r in recs}
    sample = draw(recs)
    w = stratum_weights(recs)

    if a.draw or a.status:
        print(f"{len(recs)} note-carrying tracks; {sum(len(v) for v in sample.values())} sampled")
        print(f"{'stratum':22s} {'population':>10s} {'drawn':>6s} {'weight':>7s}")
        for k in sorted(sample):
            print(f"{k[0] + '/' + k[1]:22s} {int(round(w[k] * len(sample[k]))):10d} "
                  f"{len(sample[k]):6d} {w[k]:7.1f}")

    if a.cards:
        sf, sd = siblings(recs)
        keys = [k for k in sorted(sample) if k[1] == "unnamed" and (not a.stratum or k[0] == a.stratum)]
        shown = 0
        for k in keys:
            print(f"\n--- {k[0]}/{k[1]} ---")
            for tid in sample[k]:
                if shown < a.offset:
                    shown += 1
                    continue
                if shown >= a.offset + a.limit:
                    return 0
                print(card(by_id[tid], sf[tid], sd[tid]))
                shown += 1

    if a.write:
        labels = {}
        for (src, kind), ids in sorted(sample.items()):
            for tid in ids:
                r = by_id[tid]
                if kind == "named":
                    lab, by = name_label(r), "name"
                else:
                    lab, by = hand_label(r)
                labels[tid] = {"source": src, "stratum": kind, "label": lab, "by": by,
                               # The numbers the label was read from, so the decision is auditable
                               # without the file: no path, no name, no note.
                               "evidence": {k: round(r["f"][k], 3) for k in
                                            ("median_pitch", "span", "distinct_pc", "poly",
                                             "chord_share", "med_len", "density", "kick_share",
                                             "move", "rep_share", "step_share", "leap_share",
                                             "turn_share", "scale_fit", "file_rank", "n_tracks")}}
                if tid in OVERRIDES:
                    labels[tid]["reason"] = OVERRIDES[tid][1]
        save_labels(labels, "named strata: the vendor's own word. unnamed strata: LABEL_RULE, then "
                            "the reader's overrides after reading all 200 cards.")

    if a.status:
        labels = load_labels()
        print(f"\n{len(labels)} labels stored")
        for k in sorted(sample):
            got = [labels[t]["label"] for t in sample[k] if t in labels]
            c = Counter(got)
            src = "vendor name" if k[1] == "named" else "evidence card"
            print(f"  {k[0] + '/' + k[1]:22s} {len(got):3d}/{len(sample[k]):3d} from the {src:13s}: "
                  + ", ".join(f"{x} {c[x]}" for x in sorted(c)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
