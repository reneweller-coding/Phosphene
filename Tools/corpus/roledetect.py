"""Evidence and content features for deciding what role a MIDI track plays.

**The bottleneck this file attacks.** Phosphene learns its melodic roles (acid, lead, arp) and its
bass from bought MIDI packs. Until now the role of a file came from one place: an instrument word in
its path, ``build_corpus.role_of``. Of 28 121 ``.mid`` files walked, 6 623 lines reached the models;
the loss is almost entirely the detector. 5 901 of the VORTEX bundle's 6 870 files carry no role in
the path, 4 877 of them no instrument word at all.

Three sources of evidence, in the order of how much they can be trusted, and the first two were
simply never read:

1. **The track name inside the file** (MIDI meta event 0x03). The VORTEX bundle is a third whole
   arrangements rather than loops, and those arrangements name their tracks. Over the four sources,
   **9 530** of 19 307 note-carrying tracks have an instrument word in the track name against
   **5 671** in the path, and in the trance bundle alone it is 6 888 against 1 935. A track name is
   the same kind of evidence as a file name -- the vendor's own word for what the part is -- and
   the two agree on **98.2 %** of the 3 685 tracks that carry both (``rolemodel.py --audit``), so it
   is ground truth to the same degree.
2. **The General MIDI program change** (status 0xC0). Programs 32..39 are the bass family, 80..87
   synth lead, 88..95 synth pad (General MIDI System Level 1, MMA 1991), and 5 381 tracks select
   one. **It is not a label, and that is measured, not assumed**: it agrees with the track name on
   only 73.8 % of the 2 512 tracks that carry both, which is the level of a DAW that never touched
   the instrument. It enters as a weak feature and the ablation says it is worth nothing there either.
3. **The content** -- the features below -- for what the first two leave unlabelled.

**Why the track and not the file is the unit.** ``build_corpus.read_midi`` merges every track of a
file and ``top_line`` keeps the highest note of each step. For a loop that is right. For an
arrangement with bass, lead, pad and drums in one file it produces a line no instrument ever played:
it jumps between the pad's top note and the lead as they cross. 2 652 VORTEX files are multi-track,
so this is a data *quality* defect and not only a quantity one.

**The features.** Summary statistics of one track, chosen for the question the previous round named
as the weak spot -- acid against bass, 78.4 % precision when acid was in the melodic class against
92.0 % for lead alone. An acid line legitimately sits low and repeats, so register and movement
cannot separate it from a bassline. What can:

* *the kick grid*. Psytrance is four on the floor and its bass sits in the gaps between the kicks
  (Solberg and Dibben, "Peak experiences with electronic dance music", Music Perception 36(4), 2019,
  on the kick-bass interlock as the genre's defining groove). ``kick_share`` -- the share of onsets
  on a sixteenth divisible by four -- is the direct reading of that: a rolling bass avoids those
  steps, a lead has no reason to.
* *the interval distribution*. A bassline hammers one pitch and leaps by fifths and octaves when it
  moves; a melodic line moves by step. ``step_share`` (intervals of one or two semitones) against
  ``leap_share`` (seven or more) separates the two shapes even when both sit low.
* *whether the line outlines a scale*. ``scale_fit`` is the largest share of notes covered by any
  seven-note diatonic set and ``pc_entropy`` how evenly the pitch classes are used. A root-hammering
  bass scores low on both; an acid line, which is where the previous round failed, runs a scale.
* *the register of the track inside its own file and its own folder*, not in absolute terms. A
  construction kit exports one instrument per file into one folder, so the folder is a set of parts
  of the same piece and a part's rank within it says more than its MIDI note number: a bass is the
  lowest part of its kit whatever key the kit is in.

Usage:
    python Tools/corpus/roledetect.py --report
    python Tools/corpus/roledetect.py --report --source trance
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
import re
import struct
import sys
from collections import Counter, defaultdict

_HERE = os.path.dirname(os.path.abspath(__file__))
CACHE_DIR = os.path.join(_HERE, "cache")
FEATURE_VERSION = 4


def _load_build_corpus():
    """Imports build_corpus.py as a module (it is not on a package path)."""
    spec = importlib.util.spec_from_file_location("build_corpus", os.path.join(_HERE, "build_corpus.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


bc = _load_build_corpus()

#: The classes the detector decides between. ``acid``, ``lead`` and ``arp`` are the melodic roles of
#: ``phos::CorpusRoleId``; ``bass`` is the fourth learned role of phase 8; ``pad`` collects everything
#: harmonic and held (pad, chord, stab, strings, choir); ``drum`` is percussion. Anything else is
#: ``None`` -- unknown -- and unknown is a first-class answer, not a failure.
CLASSES = ("acid", "lead", "arp", "bass", "pad", "drum")
MELODIC = ("acid", "lead", "arp")

# ------------------------------------------------------------------------------------- name rules
#
# **Whole tokens, never substrings.** The previous corpus round found that "psy" as a substring
# matches Gypsy, Psycho and Psynap, and twelve of its 34 directory hits were wrong. The same trap is
# waiting one level down and it was measured here before the rule was written: with substring
# matching, ``daft_punk__around_the_world`` is a lead (the "ld" of *world*), ``kyau_vs_albert__kiksu``
# is a drum part (*kiksu*), ``falkon__breakaway`` is a drum part (*breakaway*) and ``ultrabeat`` is
# one too. So a name is split into tokens and a token counts only when it *starts with* one of the
# prefixes below or *equals* one of the exact words. Prefixes carry the inflections that matter --
# bass/basses/bassline, arp/arps/arpeggio, melod/melody/melodies -- while everything short or
# common enough to hide inside another word is exact-only.
#
# The lists themselves are audited rather than clever: every entry was read off the token census of
# the four sources (``--report --vocabulary``). A name that hits two classes is answered ``None``,
# and that is the common case for the genuinely ambiguous names ("Bass-Lead", "Chord Lead", "Sub
# Melody"): precision costs recall here on purpose, because a mislabelled line poisons a role's
# training set for good.

_WORDS = (
    # class    token prefixes                                                exact tokens
    ("drum", ("kick", "drum", "perc", "hihat", "snare", "clap", "cymbal", "shaker", "conga",
              "bongo", "tabla", "rimshot", "crash", "bassdrum"),
             ("hat", "hats", "hh", "bd", "sd", "kik", "rim", "tom", "toms", "ride", "tamb")),
    ("arp",  ("arp",), ("grid",)),
    ("bass", ("bass", "bsl", "subbass"), ("bs", "sub", "bassi", "basso")),
    ("pad",  ("pad", "chord", "stab", "drone", "string", "choir", "atmo", "texture", "organ",
              "piano", "brass", "flute", "guitar", "harpsi", "orchestr", "ensemble", "vocal",
              "sax", "accord"), ("keys", "vox", "voice", "bell", "bells", "rhodes", "harp")),
    ("acid", ("acid", "riff", "squelch", "psyline"), ("303", "seq", "seqs", "sequence", "sequences")),
    ("lead", ("lead", "melod", "melo", "pluck", "hook"), ("ld", "topline", "mel")),
)

#: Names that contain a class word but mean something else. Each was found in the corpus; the entry
#: says which class word it disarms, so the list can be read against the packs rather than trusted.
#: Tested as phrases on the normalised name, because that is what they are.
_NOT = {
    "drum": ("drum bass", "drum and bass", "drum n bass", "drumstep"),   # a genre, not a drum part
    "bass": ("bass drum", "bassdrum", "bass kick", "bass chord", "bass pad", "bass stab"),
}

#: General MIDI System Level 1 (MMA, 1991) program families, as the classes above.
_GM = [(32, 40, "bass"), (80, 88, "lead"), (88, 96, "pad"), (40, 56, "pad"), (56, 64, "pad"),
       (16, 24, "pad"), (24, 32, "pad"), (0, 8, "pad"), (104, 112, "pad"), (112, 120, "drum")]


def normalise(text):
    """A name reduced to spaced lowercase words, with digits dropped.

    Digits are noise here ("Bass 07", "lead_12") and they are what makes two names of the same part
    look different. The leading and trailing space let a word list test whole words with ``in``.
    """
    s = re.sub(r"[_\-.+/\\]+", " ", str(text).lower())
    s = re.sub(r"\d+", " ", s)
    return " " + re.sub(r"\s+", " ", s).strip() + " "


def role_from_name(text):
    """The class a name states, or None when it states none or states two.

    Returns ``(cls, hits)`` where ``hits`` is every class the name matched, so a caller can tell
    "no word at all" from "two words that disagree" -- the second is much more common than it looks
    ("Bass-Lead", "Chord Lead", "Sub Melody") and answering it at all is where a name rule loses its
    precision.
    """
    s = normalise(text)
    toks = s.split()
    hits = []
    for cls, prefixes, exact in _WORDS:
        if any(bad in s for bad in _NOT.get(cls, ())):
            continue
        if any(t in exact or t.startswith(prefixes) for t in toks):
            hits.append(cls)
    if len(hits) == 1:
        return hits[0], hits
    return None, hits


def role_from_program(programs):
    """The class the General MIDI program numbers state, or None when they disagree or are absent.

    Program 0 (Acoustic Grand Piano) is excluded: it is the value a DAW leaves behind when nobody
    chose an instrument, so reading it as "piano, therefore pad" would label most of the corpus from
    a default. ``rolemodel.py --audit`` measures how often the remaining programs agree with the
    track name before this is used for anything.
    """
    got = set()
    for p in programs:
        if p == 0:
            continue
        for lo, hi, cls in _GM:
            if lo <= p < hi:
                got.add(cls)
                break
    return got.pop() if len(got) == 1 else None


# ---------------------------------------------------------------------------------------- features

KICK_STEPS = (0, 4, 8, 12)
#: The seven-note diatonic sets, as pitch-class masks: the major scale at each of the twelve roots.
#: Every mode of a major scale is one of these sets, so this covers natural minor, Phrygian and
#: Dorian too -- the modes psytrance actually uses -- without twelve more entries.
_SCALES = [frozenset((r + i) % 12 for i in (0, 2, 4, 5, 7, 9, 11)) for r in range(12)]


def _entropy(counts):
    tot = sum(counts)
    if tot <= 0:
        return 0.0
    return -sum((c / tot) * math.log(c / tot) for c in counts if c > 0)


FEATURES = (
    "n_notes", "bars", "median_pitch", "span", "pitch_std", "distinct_pc", "pc_entropy",
    "poly", "chord_share", "density", "med_len", "long_share", "legato", "move",
    "rep_share", "step_share", "third_share", "leap_share", "octave_share", "mean_abs_int",
    "turn_share", "mode_share", "mode_pc_share", "scale_fit",
    "kick_share", "off8_share", "odd16_share", "grid_entropy", "offgrid_share",
    "vel_std", "accent_share", "low_octave_share",
    "file_rank", "file_delta", "folder_rank", "folder_delta", "n_tracks",
)


def features(ppq, notes):
    """The summary features of one track's note list, or None when it is not usable.

    ``notes`` are ``(start, end, pitch, velocity)`` as :func:`build_corpus.read_midi_tracks` yields
    them. Everything is computed on a sixteenth grid derived from the file's division, because that
    is the grid the corpus is written on and the grid every downstream extraction uses.
    """
    if not ppq or len(notes) < 4:
        return None
    step = ppq / 4.0
    by_start = defaultdict(list)
    for s, e, p, v in notes:
        by_start[int(round(s / step))].append((p, max(1, e - s), v))
    onsets = sorted(by_start)
    if len(onsets) < 4:
        return None
    pitches = [p for _s, _e, p, _v in notes]
    vels = [v for _s, _e, _p, v in notes]
    poly = [len(by_start[k]) for k in onsets]
    lengths = sorted(max(1, int(round(d / step))) for k in onsets for _p, d, _v in by_start[k])
    top = [max(p for p, _d, _v in by_start[k]) for k in onsets]
    ints = [top[i] - top[i - 1] for i in range(1, len(top))]
    absi = [abs(x) for x in ints]
    total_steps = max(1, onsets[-1] + 1)
    pc = Counter(p % 12 for p in pitches)
    modal = Counter(top).most_common(1)[0][1]
    modal_pc = Counter(p % 12 for p in top).most_common(1)[0][1]
    turns = sum(1 for i in range(1, len(ints)) if ints[i] * ints[i - 1] < 0)
    med_v = sorted(vels)[len(vels) // 2]
    fit = max(sum(c for k, c in pc.items() if k in sc) for sc in _SCALES) / max(1, len(pitches))
    med_p = sorted(pitches)[len(pitches) // 2]
    return {
        "n_notes": float(len(notes)),
        "bars": float(max(1, round(total_steps / 16.0))),
        "median_pitch": float(med_p),
        "span": float(max(pitches) - min(pitches)),
        "pitch_std": float(math.sqrt(sum((p - sum(pitches) / len(pitches)) ** 2 for p in pitches) / len(pitches))),
        "distinct_pc": float(len(pc)),
        "pc_entropy": _entropy(list(pc.values())),
        "poly": sum(poly) / len(poly),
        "chord_share": sum(1 for x in poly if x >= 2) / len(poly),
        "density": 16.0 * len(onsets) / total_steps,
        "med_len": float(lengths[len(lengths) // 2]),
        "long_share": sum(1 for x in lengths if x >= 4) / len(lengths),
        "legato": sum(1 for k, nx in zip(onsets, onsets[1:])
                      for _p, d, _v in by_start[k] if k * step + d > nx * step + 1) / max(1, len(notes)),
        "move": sum(1 for x in ints if x != 0) / max(1, len(ints)),
        "rep_share": sum(1 for x in absi if x == 0) / max(1, len(absi)),
        "step_share": sum(1 for x in absi if 1 <= x <= 2) / max(1, len(absi)),
        "third_share": sum(1 for x in absi if 3 <= x <= 6) / max(1, len(absi)),
        "leap_share": sum(1 for x in absi if x >= 7) / max(1, len(absi)),
        "octave_share": sum(1 for x in absi if x % 12 == 0 and x > 0) / max(1, len(absi)),
        "mean_abs_int": sum(absi) / max(1, len(absi)),
        "turn_share": turns / max(1, len(ints) - 1),
        "mode_share": modal / len(top),
        "mode_pc_share": modal_pc / len(top),
        "scale_fit": fit,
        "kick_share": sum(1 for k in onsets if k % 4 == 0) / len(onsets),
        "off8_share": sum(1 for k in onsets if k % 4 == 2) / len(onsets),
        "odd16_share": sum(1 for k in onsets if k % 2 == 1) / len(onsets),
        "grid_entropy": _entropy([sum(1 for k in onsets if k % 16 == q) for q in range(16)]),
        "offgrid_share": sum(1 for s, _e, _p, _v in notes if abs(s / step - round(s / step)) > 0.15) / len(notes),
        "vel_std": math.sqrt(sum((v - sum(vels) / len(vels)) ** 2 for v in vels) / len(vels)),
        "accent_share": sum(1 for v in vels if v >= med_v + 10) / len(vels),
        "low_octave_share": sum(1 for p in pitches if p < 48) / len(pitches),
        # Filled in by :func:`add_context` once every track of the file and folder is known.
        "file_rank": 0.5, "file_delta": 0.0, "folder_rank": 0.5, "folder_delta": 0.0, "n_tracks": 1.0,
    }


def add_context(records):
    """Fills the four context features: where a track sits among its file's and its folder's parts.

    A construction kit puts the parts of one piece in one folder, so "is this the lowest part here?"
    is available without knowing the key, the tuning or the vendor. ``*_rank`` is the track's
    percentile by median pitch among its siblings (0 = the lowest part), ``*_delta`` its median pitch
    minus the sibling median in semitones. With one sibling both are the neutral 0.5 and 0.0, which
    is what a flat folder of unrelated loops deserves: no evidence.
    """
    for key, rank, delta in (("path", "file_rank", "file_delta"), ("dir", "folder_rank", "folder_delta")):
        groups = defaultdict(list)
        for r in records:
            groups[r[key]].append(r)
        for rs in groups.values():
            meds = sorted(r["f"]["median_pitch"] for r in rs)
            mid = meds[len(meds) // 2]
            for r in rs:
                if len(rs) < 2:
                    continue
                below = sum(1 for m in meds if m < r["f"]["median_pitch"])
                r["f"][rank] = below / (len(rs) - 1)
                r["f"][delta] = r["f"]["median_pitch"] - mid
    for r in records:
        r["f"]["n_tracks"] = float(min(r["tracks_in_file"], 16))
    return records


# -------------------------------------------------------------------------------------- collection

def track_records(path, rel):
    """Every note-carrying track of one file, with its evidence and its features.

    A track is skipped when it is percussion (more channel-10 note-ons than melodic ones) or when it
    is too short to describe. ``name_role`` is the class the *track* name states, ``path_role`` the
    class the file name and its own folder state; the two are kept apart because they disagree often
    enough to be worth measuring (``rolemodel.py --audit``).
    """
    try:
        ppq, tracks = bc.read_midi_tracks(path)
    except (IndexError, struct.error, OSError):
        return []
    if not ppq:
        return []
    stem = os.path.splitext(os.path.basename(rel))[0]
    own_dir = os.path.basename(os.path.dirname(rel))
    usable = [t for t in tracks if len(t["notes"]) >= 4]
    # The file name names a *part* only when the file is a part. A multi-track file is an
    # arrangement and its name is the title of the piece -- "Daft Punk - Around The World",
    # "Falkon - Breakaway" -- which is why substring matching read roles out of song titles. For
    # those files the path carries no role evidence at all and the track name carries it instead.
    if len(usable) > 1:
        path_role, path_hits = None, []
    else:
        path_role, path_hits = role_from_name(stem + " " + own_dir)
    out = []
    for ti, t in enumerate(tracks):
        if len(t["notes"]) < 4 or t["drum_notes"] > len(t["notes"]):
            continue
        f = features(ppq, t["notes"])
        if f is None:
            continue
        name_role, name_hits = role_from_name(t["name"]) if t["name"].strip() else (None, [])
        out.append({
            "rel": rel, "path": rel, "dir": os.path.dirname(rel), "track": ti,
            "id": hashlib.sha1(f"{rel}|{ti}".encode("utf-8", "replace")).hexdigest()[:16],
            "name_role": name_role, "name_hits": name_hits, "has_name": bool(t["name"].strip()),
            "path_role": path_role, "path_hits": path_hits,
            "prog_role": role_from_program(t["programs"]),
            "tracks_in_file": len(usable), "f": f,
        })
    return out


def scan(root, packs, source):
    """Every track of every ``.mid`` under ``packs``, as :func:`track_records` describes them."""
    out = []
    for pack in packs:
        base = os.path.join(root, pack)
        if not os.path.isdir(base):
            print(f"  (pack not found, skipped: {pack})", file=sys.stderr)
            continue
        for dirpath, _d, files in os.walk(base):
            for fn in sorted(files):
                if not fn.lower().endswith(".mid"):
                    continue
                full = os.path.join(dirpath, fn)
                for r in track_records(full, os.path.relpath(full, root)):
                    r["source"] = source
                    out.append(r)
    return out


#: The four sources of the corpus, as ``dataset.py`` names them. ``superpsy`` is resolved at scan
#: time because it is a name search over a reseller's directory tree.
SOURCES = ("psy", "trance", "star", "superpsy")


def packs_for(source, root):
    sys.path.insert(0, os.path.normpath(os.path.join(_HERE, "..", "train")))
    import dataset                                              # noqa: PLC0415
    return dataset.packs_for(source, root)


def load(root="M:/Midi", sources=SOURCES, refresh=False):
    """Scans the sources, with a cache in Tools/corpus/cache (gitignored; M: is a slow drive).

    The cache holds features and evidence, never note data: no loop can be read back out of it. It
    is still gitignored, because it holds the vendors' file names and those are theirs.
    """
    os.makedirs(CACHE_DIR, exist_ok=True)
    out = []
    for src in sources:
        tag = hashlib.sha1(f"{src}|{root}|v{FEATURE_VERSION}".encode()).hexdigest()[:12]
        cp = os.path.join(CACHE_DIR, f"tracks_{src}_{tag}.json")
        if os.path.exists(cp) and not refresh:
            with open(cp, "r", encoding="utf-8") as fh:
                out += json.load(fh)
            continue
        recs = add_context(scan(root, packs_for(src, root), src))
        with open(cp, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(recs, fh)
        out += recs
    return out


# ------------------------------------------------------------------------------------------- report

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--source", default="all", choices=("all",) + SOURCES)
    ap.add_argument("--refresh", action="store_true")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--vocabulary", action="store_true", help="the token census the word lists are built from")
    a = ap.parse_args()
    srcs = SOURCES if a.source == "all" else (a.source,)
    recs = load(a.root, srcs, a.refresh)
    print(f"{len(recs)} note-carrying tracks in {len({r['path'] for r in recs})} files")
    print(f"{'source':10s} {'tracks':>7s} {'files':>6s} {'multi':>6s} {'path role':>10s} "
          f"{'track name':>11s} {'program':>8s} {'any':>6s} {'none':>6s}")
    for src in srcs:
        rs = [r for r in recs if r["source"] == src]
        if not rs:
            continue
        files = {r["path"] for r in rs}
        multi = len({r["path"] for r in rs if r["tracks_in_file"] > 1})
        pr = sum(1 for r in rs if r["path_role"])
        nr = sum(1 for r in rs if r["name_role"])
        gr = sum(1 for r in rs if r["prog_role"])
        anyr = sum(1 for r in rs if r["path_role"] or r["name_role"] or r["prog_role"])
        print(f"{src:10s} {len(rs):7d} {len(files):6d} {multi:6d} {pr:10d} {nr:11d} {gr:8d} "
              f"{anyr:6d} {len(rs) - anyr:6d}")
    if a.report:
        print("\nclass a track name states (the new evidence), by source:")
        for src in srcs:
            c = Counter(r["name_role"] for r in recs if r["source"] == src and r["name_role"])
            print(f"  {src:10s} " + ", ".join(f"{k} {c[k]}" for k in CLASSES if c[k]))
        print("\nnames that hit two class words and are therefore answered 'unknown':")
        two = Counter()
        for r in recs:
            if len(r["name_hits"]) > 1:
                two["+".join(sorted(r["name_hits"]))] += 1
        print("  " + ", ".join(f"{k} {v}" for k, v in two.most_common(12)))
    if a.vocabulary:
        toks = Counter()
        for r in recs:
            toks.update(t for t in normalise(os.path.basename(r["rel"])).split() if len(t) >= 2)
        print("\nfile-name tokens:", ", ".join(f"{t}:{c}" for t, c in toks.most_common(60)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
