"""Builds the stage-B training set of Phosphene from the local MIDI corpus.

Phase 8 of PLAN 6.9 replaces only the prediction model, so this module must produce *exactly* the
symbols stage A produces: the interval of each note onset to the tonic, clamped to -12..+24, one
symbol per onset, per role (acid, lead, arp). It therefore imports the parsing of
``Tools/corpus/build_corpus.py`` -- ``read_midi``, ``role_of``, ``estimate_key``, ``top_line`` and
the role and alphabet constants -- rather than reimplementing it; the extraction below is the
melodic branch of ``build_corpus.analyse`` line for line, with the difference that it keeps the
sequences and the identity of the file they came from instead of folding them into n-gram counts.

**Why the split is by content group and not by file.** The packs sell the same loop several times:
the same riff in two keys, "Loop 07 A"/"Loop 07 B", a kit's melody exported once per construction
kit. A held-out set drawn at random over patterns puts a copy of a training loop into the test set,
and the measured NLL is then a memorisation readout, not a generalisation one -- the trap the
splitting literature calls the "duplicate leak" (Sturm, "A simple method to determine if a music
information retrieval system is a horse", IEEE Trans. Multimedia 2014, on evaluations that measure
the corpus rather than the model). The group key here is the symbol sequence itself, which is
transposition-invariant by construction, so every copy of a loop -- in any key -- falls into one
group, and a group is never split. ``--split file`` reproduces the dishonest variant so the size of
the effect can be measured rather than asserted.

**Augmentation.** Of the three augmentations PLAN 6.9 lists only one survives this representation.
Octave shift is a no-op: the reference pitch is the tonic at or below the median, so moving the
whole line an octave moves the reference with it and every symbol stays put. Diatonic shift inside
the scale is not admissible at all, because the symbols *are* intervals to the tonic and shifting
them changes the distribution the model is supposed to learn. Cyclic rotation by whole bars is
valid (a loop repeats, which is also why ``build_corpus`` counts its n-grams cyclically) and it is
the only one implemented. It is applied to the training side only, after the split.

**Where the lines come from.** Two collectors, and the second is the subject of the role-detection
round of 16.09.2026:

* :func:`collect` walks a pack, asks ``build_corpus.role_of`` for the role of each *file*, and
  extracts the top voice of the whole file. This is what the held-out set is built from and it does
  not change, because changing it would change the test set and make every number of this project
  incomparable with the ones before it.
* :func:`collect_tracks` walks the same material **per track** (``Tools/corpus/roledetect.py``) and
  takes the role from the file name *or the MIDI track name*. The track name was never read before,
  and it carries a role on 9 530 of the corpus's 19 307 note-carrying tracks where the path carries
  one on 5 671; on the 3 685 that carry both, the two agree 98.2 % of the time. For the third of the
  VORTEX bundle that is whole arrangements rather than loops this is also a correctness fix, not
  only a quantity one: merging an arrangement's tracks and taking the top voice yields a line that
  jumps between the pad and the lead as they cross, and no instrument ever played it. New material
  still enters on the **training side only** (``train.py --extra tracks,trancetracks``).

Usage:
    python Tools/train/dataset.py --root M:/Midi --report
    python Tools/train/dataset.py --root M:/Midi --packs all --report
"""
import argparse
import hashlib
import importlib.util
import json
import os
import random
import re
import struct
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
_CORPUS = os.path.normpath(os.path.join(_HERE, "..", "corpus"))


def _load_build_corpus():
    """Imports Tools/corpus/build_corpus.py as a module (it is not on a package path)."""
    spec = importlib.util.spec_from_file_location("build_corpus", os.path.join(_CORPUS, "build_corpus.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bc = _load_build_corpus()

ROLES = bc.ROLES                      # ["acid", "lead", "arp"] -- the order of phos::CorpusRoleId
ALPHABET = bc.ALPHABET                # 37
REL_MIN, REL_MAX = bc.REL_MIN, bc.REL_MAX
PSY_PACKS = list(bc.PACKS)            # the three psytrance packs the stage-A tables are built from
TRANCE_PACKS = ["VORTEX ULTIMATE TRANCE BUNDLE"]
# A fourth psytrance source, found after the first training round: 3266 flat files whose role sits in
# the file name. It is kept OUT of PSY_PACKS on purpose. PSY_PACKS defines the held-out set of every
# measurement made so far; adding a pack to it would change the test set and make the new numbers
# incomparable with the shipped model's 1.3457 and unusable for a paired bootstrap. New material
# therefore enters on the training side only, the way the trance bundle does (train.py --extra).
STAR_PACKS = [os.path.join("Star Samples", "Psy Trance Midis")]
# A vendor collection of collections: 19 794 directories three levels deep, each a resold pack. The
# genre is in the directory name and nowhere else, so the packs are found by name and only those
# directories are walked -- a recursive scan of 370 000 files to find 21 000 would cost minutes of a
# slow drive for nothing.
SUPER_PACK = os.path.join("Star Samples", "Various Midi 370.000 Super Pack")
# "psy" as a substring is a trap: it matches Gypsy, Psycho, Psynap. The rule is therefore a whole
# token ("psy trance", "PsyTrance", "goa"), plus two audited lists -- 34 candidate directories is a
# human-scale list and a curated one is more honest than a regex that pretends to know the genre.
_PSY_TOKENS = ("psy", "goa", "psytrance", "psytrances")
_PSY_ALLOW = ("psyload",)          # Psyload Twisted Reaction: a psytrance pack the token rule misses
_PSY_DENY = ("gypsy", "psycho", "psynap", "money", "trapstep", "psychodelic")
_TRANCE_TOKENS = ("trance",)

START_SYMBOL = 0 - REL_MIN            # 12: the symbol of the interval 0, the sampler's start context
CACHE_DIR = os.path.join(_CORPUS, "cache")
TOKEN_VERSION = 1

# Conditioning table sizes; mirrored in docs/MODEL_FORMAT.md section 3 and in the .phosmdl header.
N_ROLE, N_STYLE, N_BARS, N_STEP, N_BAR, N_GAP, N_IDX = 3, 6, 8, 16, 8, 10, 8


def idx_bucket(t):
    """Bucket of the note index: the first notes of a line behave differently from the hundredth."""
    if t < 4:
        return t
    if t < 6:
        return 4
    if t < 10:
        return 5
    if t < 16:
        return 6
    return 7


def gap_code(steps, i):
    """0 for the last note, 1..8 for the distance to the next onset, 9 for more than eight steps."""
    if i + 1 >= len(steps):
        return 0
    d = steps[i + 1] - steps[i]
    return min(d, 8) if d <= 8 else 9


def extract(path, role):
    """The melodic branch of build_corpus.analyse for one file, keeping the sequence.

    Returns None when the file is not usable as a melody (the same tests build_corpus applies: a
    readable header, at least four notes, a top line, at least three distinct pitches -- a loop of
    one or two pitches is a rhythm and contributes only onset counts there).
    """
    try:
        ppq, notes = bc.read_midi(path)
    except (IndexError, struct.error):
        # The third case this used to catch, a TypeError from a running-status data byte with no
        # status byte before it, is gone: build_corpus.read_midi handles running status properly
        # since the role-detection round and ends such a track instead of raising
        # (Tools/corpus/test_build_corpus.py). The two remaining cases are genuine truncation.
        return None
    return extract_notes(ppq, notes, role, path)


def extract_notes(ppq, notes, role, path):
    """The same extraction from an already-parsed note list, for one track of a file.

    Split out of :func:`extract` when the role detector moved from the file to the track: a third of
    the VORTEX bundle is whole arrangements, and merging their tracks and taking the top voice gives
    a line no instrument played (``Tools/corpus/roledetect.py``). ``path`` is still passed because
    ``estimate_key`` reads a key out of the file name when there is one; the notes are the track's.
    """
    if not ppq or len(notes) < 4:
        return None
    tonic, _src = bc.estimate_key(notes, path)
    line = bc.top_line(ppq, notes)
    if not line:
        return None
    pitches = [s[0] for s in line if s]
    if len(set(pitches)) < 3:
        return None
    median = sorted(pitches)[len(pitches) // 2]
    ref = median - ((median - tonic) % 12)          # the tonic at or below the median
    syms, steps = [], []
    for k, s in enumerate(line):
        if s:
            rel = max(REL_MIN, min(REL_MAX, s[0] - ref))
            syms.append(rel - REL_MIN)
            steps.append(k)
    if len(syms) < 4:
        return None
    return {"role": role, "syms": syms, "steps": steps, "bars": max(1, len(line) // 16)}


def collect(root, packs, limit_per_pack=0):
    """Walks the packs and returns the usable melodic lines, with the pack they came from."""
    out = []
    for pi, pack in enumerate(packs):
        base = os.path.join(root, pack)
        if not os.path.isdir(base):
            print(f"  (pack not found, skipped: {pack})", file=sys.stderr)
            continue
        seen = 0
        for dirpath, _dirs, files in os.walk(base):
            for f in files:
                if not f.lower().endswith(".mid"):
                    continue
                path = os.path.join(dirpath, f)
                role = bc.role_of(path)
                if role is None:
                    continue
                rec = extract(path, ROLES.index(role))
                if rec is None:
                    continue
                rec["pack"] = pi
                rec["file"] = hashlib.sha1(os.path.relpath(path, root).encode("utf-8", "replace")).hexdigest()[:16]
                out.append(rec)
                seen += 1
                if limit_per_pack and seen >= limit_per_pack:
                    break
            if limit_per_pack and seen >= limit_per_pack:
                break
    return out


def group_key(rec):
    """Identity of the loop's content: role plus the symbol sequence, which is key-invariant."""
    return (rec["role"],) + tuple(rec["syms"])


def near_duplicate(a, b, tol=0.9):
    """True when two lines are the same loop up to a constant transposition and a few edited notes.

    Exact content keys catch only literal re-exports. The packs also sell a riff whose key estimate
    came out a semitone away, or with one note moved; those are the copies that make a held-out set
    lie. Same role, same onset grid, same length, and at least ``tol`` of the symbols equal after
    subtracting the commonest difference (the best constant transposition) counts as one loop.
    """
    if a["role"] != b["role"] or len(a["syms"]) != len(b["syms"]) or a["steps"] != b["steps"]:
        return False
    diffs = Counter(x - y for x, y in zip(a["syms"], b["syms"]))
    shift, hits = diffs.most_common(1)[0]
    return hits >= tol * len(a["syms"])


def loop_groups(records):
    """Union-find over ``near_duplicate``: every copy of a loop ends in one group."""
    parent = list(range(len(records)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    # Only lines with the same role, length and onset grid can be near duplicates, so bucket first:
    # the quadratic comparison then runs inside buckets of a handful of lines.
    buckets = {}
    for i, r in enumerate(records):
        buckets.setdefault((r["role"], tuple(r["steps"])), []).append(i)
    for idxs in buckets.values():
        for x in range(len(idxs)):
            for y in range(x + 1, len(idxs)):
                if near_duplicate(records[idxs[x]], records[idxs[y]]):
                    a, b = find(idxs[x]), find(idxs[y])
                    if a != b:
                        parent[a] = b
    out = {}
    for i in range(len(records)):
        out.setdefault(find(i), []).append(i)
    return list(out.values())


def split(records, mode="group", frac_val=0.15, frac_test=0.15, seed=12345):
    """Three-way split over units of ``mode``, stratified by role: (train, val, test) index lists.

    ``group`` -- the honest one -- keeps every copy of a loop (:func:`loop_groups`) on one side.
    ``file`` splits by source file, which lets two exports of the same riff straddle the boundary.
    A third mode, a random split over *augmented* records, can only be made after augmentation and
    lives in :func:`build_split`.

    The split is three-way and not two-way because early stopping is a choice made from data: the
    validation set picks the step and the hyperparameters, the test set is read once per model. With
    a two-way split every reported number would be a best-of-many over the set it is reported on.
    """
    rng = random.Random(seed)
    units = loop_groups(records) if mode == "group" else [[i] for i in range(len(records))]
    by_role = {}
    for idxs in units:
        by_role.setdefault(records[idxs[0]]["role"], []).append(idxs)
    train, val, test = [], [], []
    for role in sorted(by_role):
        groups = sorted(by_role[role], key=lambda g: records[g[0]]["file"])
        rng.shuffle(groups)
        n_val = max(1, int(round(len(groups) * frac_val)))
        n_test = max(1, int(round(len(groups) * frac_test)))
        for g in groups[:n_val]:
            val += g
        for g in groups[n_val:n_val + n_test]:
            test += g
        for g in groups[n_val + n_test:]:
            train += g
    return sorted(train), sorted(val), sorted(test)


def build_split(records, mode="group", frac_val=0.15, frac_test=0.15, seed=12345, rotations=1):
    """Returns (train, val, test) record lists, augmented on the training side only.

    ``mode="naive"`` is the dishonest control the plan asks to be measured: augment everything, then
    split the augmented records at random, so rotations of one loop land on both sides.
    """
    if mode == "naive":
        aug = augment(records, list(range(len(records))), rotations)
        rng = random.Random(seed)
        order = list(range(len(aug)))
        rng.shuffle(order)
        n_val = int(round(len(order) * frac_val))
        n_test = int(round(len(order) * frac_test))
        pick = lambda sl: [aug[i] for i in sl]   # noqa: E731
        return pick(order[n_val + n_test:]), pick(order[:n_val]), pick(order[n_val:n_val + n_test])
    tr, va, te = split(records, mode, frac_val, frac_test, seed)
    return (augment(records, tr, rotations), [records[i] for i in va], [records[i] for i in te])


def assemble_extra(root, extra, holdout, trance_weight=1.0, log=None):
    """The extra training material named by ``extra``, ready to be merged with the psytrance lines.

    Shared by train.py and memorisation.py: the memorisation battery compares generated lines against
    the set the model was *actually* trained on, so the two must build that set the same way or the
    copy rates are measured against the wrong corpus and read too low.

    ``holdout`` is validation plus test; every candidate that is a near duplicate of one of those is
    dropped, which is what keeps extra material from re-introducing the held-out lines.
    """
    psy_side, trance_side = [], []
    for which in [w for w in (extra or "none").split(",") if w and w != "none"]:
        if which == "trance":
            pool = load(root, TRANCE_PACKS)
        elif which == "star":
            pool = load(root, STAR_PACKS)
        elif which == "starsynth":
            pool = collect_synth_loops(root)
        elif which == "superpsy":
            pool = load(root, packs_for("superpsy", root))
        elif which == "supertrance":
            pool = load(root, packs_for("supertrance", root))
        elif which == "tracks":
            pool = collect_tracks(root, TRACK_PSY_SOURCES, "name")
        elif which == "trancetracks":
            pool = collect_tracks(root, TRACK_TRANCE_SOURCES, "name")
        elif which == "clf":
            pool = collect_tracks(root, TRACK_PSY_SOURCES, "clf")
        elif which == "tranceclf":
            pool = collect_tracks(root, TRACK_TRANCE_SOURCES, "clf")
        else:
            raise SystemExit(f"unknown --extra {which}")
        k = exclude_near(pool, holdout)
        if log:
            print(f"  + {len(k)} {which} lines ({len(pool) - len(k)} dropped as near duplicates of a "
                  f"held-out line); val and test stay the psytrance ones", file=log)
        (trance_side if which in ("trance", "supertrance", "trancetracks", "tranceclf")
         else psy_side).extend(k)
    if trance_side:
        weighed = weigh(trance_side, trance_weight)
        if log:
            print(f"  trance class: {len(trance_side)} lines at weight {trance_weight} "
                  f"-> {len(weighed)}", file=log)
        psy_side += weighed
    return psy_side


def dedupe(records):
    """One line per loop group over the **merged** corpus; returns (kept, removed).

    A vendor collection resells the same loop in several packs, so a per-pack duplicate pass misses
    exactly the duplicates that matter. Running the union-find over everything at once also keeps a
    loop that appears in three vendors from counting three times in the training distribution.
    """
    groups = loop_groups(records)
    kept = [records[min(g)] for g in groups]
    return kept, len(records) - len(kept)


def weigh(records, weight, seed=4242):
    """Repeats or subsamples a class of material to give it a weight other than one.

    PLAN 6.9 says trance counts with a lower weight, and once trance outnumbers psytrance ten to one
    that stops being a detail. A weight below 1 keeps that fraction of the lines (drawn once, with a
    fixed seed, so a run is reproducible); a weight above 1 repeats them.
    """
    if weight == 1.0 or not records:
        return list(records)
    rng = random.Random(seed)
    order = list(range(len(records)))
    rng.shuffle(order)
    whole = int(weight)
    out = [records[i] for _ in range(whole) for i in range(len(records))]
    rest = int(round((weight - whole) * len(records)))
    out += [records[i] for i in order[:rest]]
    return out


def exclude_near(candidates, holdout):
    """Candidate lines that are not a near duplicate of anything in ``holdout``.

    Used when extra material (the trance bundle) is added to the training side of a split that was
    made over the psytrance corpus: without this, a trance loop that happens to be the same riff as a
    held-out psy loop would put the test set back into training through the side door.
    """
    buckets = {}
    for r in holdout:
        buckets.setdefault((r["role"], tuple(r["steps"])), []).append(r)
    keep = []
    for c in candidates:
        if not any(near_duplicate(c, h) for h in buckets.get((c["role"], tuple(c["steps"])), ())):
            keep.append(c)
    return keep


def split_seed_of(args):
    """The split seed of a run, from its stored arguments.

    ``--seed`` varies the weight initialisation and the batch order; ``--split-seed`` varies the
    held-out set. They were one knob until the seed-variance measurement needed the split held fixed
    -- otherwise five "seeds" would each be scored on a different test set and the spread would be
    the spread of the corpus, not of the training. Runs made before the split existed as its own
    knob fall back to ``--seed``, which is what they used.
    """
    return args.get("split_seed", args["seed"]) * 7919 + 12345


def sibling_leak(train, test):
    """Share of test lines that have a rotation sibling (same file id) on the training side.

    This is the mechanism behind the naive split, measured rather than asserted: with a split by
    loop group it is 0 by construction, with a random split over augmented records it is not.
    """
    files = {r["file"] for r in train}
    return sum(1 for r in test if r["file"] in files) / max(1, len(test))


def rotate(rec, bars_shift):
    """Cyclic rotation of a loop by whole bars -- the one admissible augmentation (see the docstring)."""
    total = rec["bars"] * 16
    shift = (bars_shift * 16) % total
    order = sorted(range(len(rec["syms"])), key=lambda i: (rec["steps"][i] - shift) % total)
    return {"role": rec["role"], "pack": rec["pack"], "bars": rec["bars"], "file": rec["file"],
            "syms": [rec["syms"][i] for i in order],
            "steps": [(rec["steps"][i] - shift) % total for i in order]}


def augment(records, indices, rotations=1):
    """Returns training records: the originals plus ``rotations`` bar rotations of every multi-bar loop."""
    out = [records[i] for i in indices]
    if rotations <= 0:
        return out
    for i in indices:
        rec = records[i]
        if rec["bars"] < 2:
            continue
        for b in range(1, min(rotations + 1, rec["bars"])):
            out.append(rotate(rec, b))
    return out


def encode(rec, style=None, ctx=256):
    """One record as parallel arrays, as docs/MODEL_FORMAT.md section 3 defines them.

    Returns (tokens, targets, step, bar, gap, idx, role, style, bars) with tokens[t] = s_{t-1}
    (s_{-1} = START_SYMBOL) and targets[t] = s_t.
    """
    syms, steps = rec["syms"][:ctx], rec["steps"][:ctx]
    n = len(syms)
    tok = [START_SYMBOL] + syms[:-1]
    st = [s % 16 for s in steps]
    br = [(s // 16) % 8 for s in steps]
    gp = [gap_code(steps, i) for i in range(n)]
    ix = [idx_bucket(t) for t in range(n)]
    if style is None:
        style = rec.get("style", 0)   # 0 = unknown; the corpus carries no style label (MODEL_FORMAT 3)
    return {"tok": tok, "tgt": syms, "step": st, "bar": br, "gap": gp, "idx": ix,
            "role": rec["role"], "style": style, "bars": min(max(rec["bars"], 1), 8) - 1}


def cache_path(root, packs):
    tag = hashlib.sha1(("|".join(packs) + "|" + root + f"|v{TOKEN_VERSION}").encode()).hexdigest()[:12]
    return os.path.join(CACHE_DIR, f"lines_{tag}.json")


def load(root="M:/Midi", packs=None, refresh=False):
    """Collects the lines, with a cache in Tools/corpus/cache (gitignored; M: is a slow drive)."""
    packs = packs or PSY_PACKS
    os.makedirs(CACHE_DIR, exist_ok=True)
    cp = cache_path(root, packs)
    if os.path.exists(cp) and not refresh:
        with open(cp, "r", encoding="utf-8") as fh:
            return json.load(fh)
    recs = collect(root, packs)
    with open(cp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(recs, fh)
    return recs


def super_dirs(root, kind, depth=3):
    """Directories of the super pack whose *name* says psytrance ("psy"/"goa") or trance.

    Only the topmost match is returned: a pack called "... PsyTrance ..." with a "Psytrance Bass"
    subfolder is one pack, and walking both would count its files twice. A directory that says psy
    or goa is never also counted as trance, so the two kinds do not overlap.
    """
    base = os.path.join(root, SUPER_PACK)
    if not os.path.isdir(base):
        return []
    out, matched = [], []

    def is_psy(low):
        if any(w in low for w in _PSY_DENY):
            return False
        if any(w in low for w in _PSY_ALLOW):
            return True
        toks = set(re.split(r"[^a-z0-9]+", low))
        return bool(toks & set(_PSY_TOKENS))

    def walk(path, level):
        try:
            entries = sorted(os.scandir(path), key=lambda e: e.name)
        except OSError:
            return
        for e in entries:
            if not e.is_dir():
                continue
            low = e.name.lower()
            if any(m == os.path.commonpath([m, e.path]) for m in matched):
                continue                      # inside a pack that already matched
            psy = is_psy(low)
            hit = psy if kind == "psy" else (any(w in low for w in _TRANCE_TOKENS) and not psy)
            if hit:
                matched.append(e.path)
                out.append(os.path.relpath(e.path, root))
                continue
            if level < depth:
                walk(e.path, level + 1)

    walk(base, 1)
    return out


def packs_for(name, root="M:/Midi"):
    if name == "superpsy":
        return super_dirs(root, "psy")
    if name == "supertrance":
        return super_dirs(root, "trance")
    return {"psy": PSY_PACKS, "trance": TRANCE_PACKS, "star": STAR_PACKS,
            "all": PSY_PACKS + TRANCE_PACKS + STAR_PACKS}[name]


#: Which of ``roledetect``'s sources count as psytrance and which as the trance class. The split is
#: the one the previous round measured: trance material helps but saturates, psytrance material is
#: worth several times as much per line, so the two carry different weights (``--trance-weight``).
TRACK_PSY_SOURCES = ("psy", "star", "superpsy")
TRACK_TRANCE_SOURCES = ("trance",)


def _roledetect():
    """Imports Tools/corpus/roledetect.py, which is not on a package path either."""
    spec = importlib.util.spec_from_file_location("roledetect", os.path.join(_CORPUS, "roledetect.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def collect_tracks(root, sources, how="name", threshold=0.70, refresh=False):
    """Melodic lines taken **per track**, with the role from the evidence ``how`` names.

    ``how="name"`` takes the role the file name or the *track* name states -- the track name being
    the evidence source nobody had read, available on 9 530 of the corpus's 19 307 note-carrying
    tracks against 5 671 for the path, and agreeing with the path name on 98.2 % of the tracks that
    carry both (``Tools/corpus/rolemodel.py --audit``). ``how="clf"`` takes what the content
    classifier admits above ``threshold`` from the tracks no name covers.

    The two are kept apart because they are worth different amounts and the difference is the point
    of this round: a track name is the vendor's own word, while the classifier is measured at 97.4 %
    precision against vendor names but only 50 % against the hand-labelled unnamed sample, because
    the unnamed population is mostly rhythm patterns, pads and basslines
    (``Tools/corpus/labelsample.py``). Whether either is worth admitting is decided by the held-out
    NLL and not here.
    """
    rd = _roledetect()
    recs = rd.load(root, tuple(sources), refresh)
    if how == "name":
        want = [(r, (r["name_role"] or r["path_role"])) for r in recs]
        want = [(r, c) for r, c in want if c in rd.MELODIC]
    else:
        import numpy as np                                       # noqa: PLC0415
        if _CORPUS not in sys.path:
            sys.path.insert(0, _CORPUS)
        import labelsample                                       # noqa: PLC0415
        import rolemodel as rm                                   # noqa: PLC0415
        full = rd.load(root)                                     # the classifier sees the whole corpus
        labels = labelsample.load_labels()
        tracks, ys = rm.labelled_tracks(full, set(labels))
        model, _names, order = rm.fit_full(full, tracks, ys)
        prior = np.array([Counter(ys)[c] for c in rm.CLASSES], dtype=float)
        scores, _priors = rd_scores = rm.unnamed_scores(model, order, full, prior, adjust=False)
        del rd_scores
        want = []
        for r in recs:
            row = scores.get(r["id"])
            if row is None or row.max() < threshold:
                continue
            cls = rm.CLASSES[int(row.argmax())]
            if cls in rd.MELODIC:
                want.append((r, cls))
    by_path = defaultdict(list)
    for r, cls in want:
        by_path[r["path"]].append((r["track"], cls, r["source"]))
    out = []
    for rel, wanted in sorted(by_path.items()):
        try:
            ppq, tracks_ = bc.read_midi_tracks(os.path.join(root, rel))
        except (IndexError, struct.error, OSError):
            continue
        if not ppq:
            continue
        for ti, cls, src in wanted:
            if ti >= len(tracks_):
                continue
            rec = extract_notes(ppq, tracks_[ti]["notes"], ROLES.index(cls), os.path.join(root, rel))
            if rec is None:
                continue
            rec["pack"] = list(sources).index(src) if src in sources else 0
            rec["file"] = hashlib.sha1(f"{rel}|{ti}".encode("utf-8", "replace")).hexdigest()[:16]
            out.append(rec)
    return out


def collect_synth_loops(root, packs=None, melodic_roles=("lead",), refresh=False):
    """Files whose name says only "synth ..." and whose **content** says lead, admitted as lead.

    The name gives no role, so ``rolecheck`` decides: a one-vs-rest logistic regression over eight
    content features, fitted on the files of the same folder whose names do state a role, and scored
    on a held-out third before it is used. With the melodic class restricted to ``lead`` it reaches
    92 % precision and 71 % recall on that third; with acid and arp in the melodic class it reaches
    only 78 %, because an acid line legitimately sits low and repeats and is hard to tell from a
    bassline by content. See rolecheck.py.

    Whether the class is worth admitting is not settled by that precision alone -- it is settled by
    training with and without it and reading the held-out NLL, which is what train.py --extra star
    against --extra starsynth measures.
    """
    import rolecheck
    rolecheck.MELODIC_ROLES = list(melodic_roles)
    out = []
    for pi, pack in enumerate(packs or STAR_PACKS):
        base = os.path.join(root, pack)
        if not os.path.isdir(base):
            continue
        labelled, ambiguous = rolecheck.scan(base)
        rows = [(f, l) for _p, f, l in labelled]
        rows.sort(key=lambda t: (t[1], t[0]["mean_pitch"], t[0]["span"]))
        mu, sd = rolecheck.standardise([r for r, _l in rows])
        heads = {c: rolecheck.fit([r for r, _l in rows], [1 if l == c else 0 for _r, l in rows], mu, sd)
                 for c in ("melodic", "bass", "padlike")}
        for path, fe, _l in ambiguous:
            scores = {c: rolecheck.predict(fe, w, b, mu, sd) for c, (w, b) in heads.items()}
            if max(scores, key=scores.get) != "melodic":
                continue
            rec = extract(path, ROLES.index("lead"))
            if rec is None:
                continue
            rec["pack"] = pi
            rec["file"] = hashlib.sha1(os.path.relpath(path, root).encode("utf-8", "replace")).hexdigest()[:16]
            out.append(rec)
    return out


def report(records):
    print(f"{len(records)} melodic lines, {sum(len(r['syms']) for r in records)} notes")
    for ri, role in enumerate(ROLES):
        rs = [r for r in records if r["role"] == ri]
        notes = sum(len(r["syms"]) for r in rs)
        lens = sorted(len(r["syms"]) for r in rs)
        groups = len({group_key(r) for r in rs})
        near = len(loop_groups(rs))
        print(f"  {role:5s}: {len(rs):5d} lines, {notes:7d} notes, {groups:5d} distinct contents "
              f"({len(rs) - groups} exact duplicates), {near} loop groups ({len(rs) - near} near duplicates), "
              f"length median {lens[len(lens)//2] if lens else 0}, max {lens[-1] if lens else 0}")
    for mode in ("group", "file", "naive"):
        tr, va, te = build_split(records, mode)
        print(f"  split {mode:6s}: {len(tr)} train / {len(va)} val / {len(te)} test lines, "
              f"{sum(len(r['syms']) for r in te)} test notes, "
              f"{100 * sibling_leak(tr, te):.1f} % of test lines have a sibling in train")
    c = Counter(r["pack"] for r in records)
    print("  lines per pack:", dict(c))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy", choices=["psy", "trance", "all"])
    ap.add_argument("--refresh", action="store_true")
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    recs = load(a.root, packs_for(a.packs), a.refresh)
    if a.report or True:
        report(recs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
