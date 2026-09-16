"""The bass corpus of Phosphene: extraction, split and conditioning for the fourth role.

Phase 8 learned the pitch of three melodic roles (acid, lead, arp). The bass was never learned at
all -- neither by stage A (``role_of`` in ``Tools/corpus/build_corpus.py`` returns ``None`` for it)
nor by stage B. This module is the bass half of the same pipeline, and it is deliberately a **new
file**: ``build_corpus.py``, ``dataset.py``, ``models.py``, ``train.py`` and ``export.py`` are
shared with the melodic training round, so everything specific to the bass lives here and only the
pieces that are genuinely generic (``near_duplicate``, ``loop_groups``, ``split``, ``rotate``) are
imported from ``dataset``.

**Why the bass needs its own extraction.** Three differences to a melodic line, each measured rather
than assumed (``Tools/train/bass_stats.py`` prints the numbers):

* *The voice is the bottom one.* ``build_corpus.top_line`` keeps the highest pitch of every step,
  which is right for a lead over a pad and wrong for a bass under an octave doubling. Psytrance bass
  loops are almost always monophonic -- of 1557 parsed files only 30 have two notes starting on the
  same tick -- so the choice changes little, but where it changes anything it changes it the wrong
  way, and the cost of being right is one function.
* *A one-pitch loop is still a bass.* The melodic branch drops every line with fewer than three
  distinct pitches as "a rhythm, not a melody". For the bass that test would throw away the most
  typical material there is: a rolling sixteenth figure on the root. Of the parsed files 337 have
  exactly one pitch and 257 have two. They are kept here, because the rhythm *is* the subject.
* *The kick is part of the input.* A psytrance bass sits in the gaps of a four-on-the-floor kick
  (Solberg and Dibben, "Peak experiences with electronic dance music", Music Perception 36(4), 2019,
  on the kick-bass interlock as the genre's defining groove). The kick is not in these MIDI files --
  ``read_midi`` drops channel 10 and most loops are single-instrument exports -- so it is taken as
  the canonical four-on-the-floor, and ``bass_stats`` checks that assumption against the corpus
  instead of asserting it: the four kick steps carry an onset in 22.5 % of the bars, the twelve
  steps between them in 62.4 %.

**Packs.** The three psytrance packs the stage-A tables come from, plus ``Star Samples/Psy Trance
Midis``, which is psytrance and holds more bass files than the other three together. The VORTEX
trance bundle is available as extra training material under the same rule the melodic round used
(training side only, near duplicates of held-out lines removed first).
"""
import hashlib
import json
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dataset                                                # noqa: E402

bc = dataset.bc

REL_MIN, REL_MAX = dataset.REL_MIN, dataset.REL_MAX
ALPHABET = dataset.ALPHABET
START_SYMBOL = dataset.START_SYMBOL
BASS_ROLE = 3                      # roles 0..2 are acid, lead, arp (phos::CorpusRoleId)
N_ROLE_BASS = 4                    # the header's `roles` once the bass is in

#: Packs walked for bass material. The first three are ``build_corpus.PACKS``; the fourth is a
#: psytrance pack the melodic round does not use because its role words are unreliable for melody
#: ("main lead" and "bassline" in the same flat folder) -- for the bass the word is unambiguous.
BASS_PACKS = list(bc.PACKS) + [os.path.join("Star Samples", "Psy Trance Midis")]
BASS_TRANCE_PACKS = list(dataset.TRANCE_PACKS)

#: Kick steps of the sixteenth grid: four on the floor. ``Composer::composeBars`` places a kick on
#: every beat unless the form takes it away, so these are the steps a bass note must share with it.
KICK_STEPS = (0, 4, 8, 12)

CACHE_DIR = dataset.CACHE_DIR
TOKEN_VERSION = 1

#: Conditioning table sizes. Everything up to ``N_IDX`` is ``dataset``'s; ``N_KICK`` is the one the
#: bass adds -- see :func:`kick_code`.
N_STYLE, N_BARS, N_STEP, N_BAR, N_GAP, N_IDX = (
    dataset.N_STYLE, dataset.N_BARS, dataset.N_STEP, dataset.N_BAR, dataset.N_GAP, dataset.N_IDX)
N_KICK = 3


def kick_code(step):
    """Where the note sits relative to the kick: 0 on it, 1 just after it, 2 elsewhere in the gap.

    The three cases are the ones the genre distinguishes and the ones the composer can answer before
    it draws a pitch. `0` is the collision case -- a bass note on the kick, which the depth rule of
    PLAN 5.2 makes a masking problem rather than a musical one. `1` is the first sixteenth after the
    kick, the note a rolling bass leans on. `2` is the rest of the gap. A finer code (the distance in
    sixteenths) would be a second copy of ``step``, which the model already has; this one says
    something ``step`` does not, because it stays meaningful when the kick pattern is not four on the
    floor (`Composer` removes kicks in breakdowns and in the pre-drop bar).

    **Measured, and it buys nothing here.** ``train_bass.py --no-kick`` trains the same model without
    this table: 0.4323 against 0.4293 nats on the same split, a paired bootstrap over lines putting
    the difference at -0.0030 nats with a 95 % interval of [-0.0208, +0.0150]. The reason is that the
    corpus contains no kick at all, so the class is taken from the step and is a deterministic
    function of it; only a corpus in which the kick is present *and* sometimes off the beat could
    give it something to say.
    """
    if step % 4 == 0:
        return 0
    if step % 4 == 1:
        return 1
    return 2


def bass_role_of(path):
    """True when the path names a bass loop and not something else that has "bass" in it.

    Only the file name and its own folder are read, the rule ``build_corpus.role_of`` follows,
    because pack folders mix roles. The exclusions matter: "bass drum" and "bassline + kick" are
    drums, "lead bass glide" and "bass stab" are melodic parts written in a bass register, and a
    file called "bass chord" is a chord.
    """
    parts = path.lower().replace("\\", "/").split("/")
    p = "/".join(parts[-2:])
    if any(w in p for w in ("kick", "drum", "perc", "hat", "clap", "snare", "tom", "ride", "crash",
                            "pad", "stab", "chord", "arp", "lead", "melod", "acid", "303")):
        return False
    return "bass" in p or "bsl" in p or " bs " in p or p.endswith("/bs.mid")


def bottom_line(ppq, notes):
    """Bottom voice on a sixteenth grid, otherwise ``build_corpus.top_line`` note for note.

    The only difference is ``min`` instead of ``max`` over the notes that start on one step: a bass
    that is doubled an octave up should contribute its lower note, not its upper one.
    """
    import math
    from collections import defaultdict
    step = ppq / 4.0
    if not notes:
        return []
    total = int(math.ceil(max(e for _, e, _, _ in notes) / step))
    bars = max(1, int(round(total / 16.0)))
    steps = [None] * (bars * 16)
    by_step = defaultdict(list)
    for s, e, p, v in notes:
        k = int(round(s / step))
        if 0 <= k < len(steps):
            by_step[k].append((p, v, s, e))
    onsets = sorted(by_step)
    for idx, k in enumerate(onsets):
        p, v, s, e = min(by_step[k])
        nxt = onsets[idx + 1] if idx + 1 < len(onsets) else None
        length = max(1, int(round((e - s) / step)))
        slide = nxt is not None and e > nxt * step + 1
        steps[k] = (p, v, length, slide)
    return steps


def trim_empty_bars(line):
    """Drops whole empty bars at the head and the tail of a grid.

    ``top_line`` rounds the grid up to whole bars from the last note end, so a loop whose last note
    is held into a trailing bar gets a bar of silence that is an artefact of the export, not a rest
    the player wrote. Empty bars *inside* a loop are kept: those are real.
    """
    bars = len(line) // 16
    first, last = 0, bars - 1
    while first <= last and not any(line[first * 16:(first + 1) * 16]):
        first += 1
    while last >= first and not any(line[last * 16:(last + 1) * 16]):
        last -= 1
    return line[first * 16:(last + 1) * 16] if last >= first else []


def extract(path):
    """One bass file as a record, or None when it is not usable.

    The record carries what both candidate models need: ``syms``/``steps`` for the pitch stream (the
    stage-A alphabet, one symbol per onset) and ``grid`` for the onset stream (one bit per sixteenth,
    whole bars). The tests are ``build_corpus``'s, minus the three-distinct-pitches rule.
    """
    try:
        ppq, notes = bc.read_midi(path)
    except (IndexError, struct.error):
        return None
    if not ppq or len(notes) < 4:
        return None
    tonic, _src = bc.estimate_key(notes, path)
    line = trim_empty_bars(bottom_line(ppq, notes))
    if not line:
        return None
    pitches = [s[0] for s in line if s]
    if len(pitches) < 4:
        return None
    median = sorted(pitches)[len(pitches) // 2]
    ref = median - ((median - tonic) % 12)        # the tonic at or below the median
    syms, steps = [], []
    for k, s in enumerate(line):
        if s:
            syms.append(max(REL_MIN, min(REL_MAX, s[0] - ref)) - REL_MIN)
            steps.append(k)
    grid = [1 if s else 0 for s in line]
    return {"role": BASS_ROLE, "syms": syms, "steps": steps, "grid": grid,
            "bars": max(1, len(line) // 16)}


def collect(root, packs):
    """Walks the packs and returns the usable bass lines with the pack they came from."""
    out = []
    for pi, pack in enumerate(packs):
        base = os.path.join(root, pack)
        if not os.path.isdir(base):
            print(f"  (pack not found, skipped: {pack})", file=sys.stderr)
            continue
        for dirpath, _dirs, files in os.walk(base):
            for f in files:
                if not f.lower().endswith(".mid"):
                    continue
                path = os.path.join(dirpath, f)
                if not bass_role_of(path):
                    continue
                rec = extract(path)
                if rec is None:
                    continue
                rec["pack"] = pi
                rec["file"] = hashlib.sha1(os.path.relpath(path, root).encode("utf-8", "replace")).hexdigest()[:16]
                out.append(rec)
    return out


def cache_path(root, packs):
    tag = hashlib.sha1(("bass|" + "|".join(packs) + "|" + root + f"|v{TOKEN_VERSION}").encode()).hexdigest()[:12]
    return os.path.join(CACHE_DIR, f"bass_{tag}.json")


def load(root="M:/Midi", packs=None, refresh=False):
    """Collects the bass lines, with the same gitignored cache the melodic side uses."""
    packs = packs or BASS_PACKS
    os.makedirs(CACHE_DIR, exist_ok=True)
    cp = cache_path(root, packs)
    if os.path.exists(cp) and not refresh:
        with open(cp, "r", encoding="utf-8") as fh:
            return json.load(fh)
    recs = collect(root, packs)
    with open(cp, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(recs, fh)
    return recs


def rotate(rec, bars_shift):
    """Cyclic rotation by whole bars, the one admissible augmentation (``dataset.rotate``'s reason).

    ``dataset.rotate`` moves ``syms``/``steps`` only; a bass record also carries ``grid``, and a
    rotation that left the grid behind would train the onset model on the unrotated pattern and the
    pitch model on the rotated one.
    """
    out = dataset.rotate(rec, bars_shift)
    total = rec["bars"] * 16
    shift = (bars_shift * 16) % total
    out["grid"] = [rec["grid"][(k + shift) % total] for k in range(total)]
    return out


def augment(records, indices, rotations=1):
    """Originals plus ``rotations`` bar rotations of every multi-bar loop (training side only)."""
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


def build_split(records, mode="group", frac_val=0.15, frac_test=0.15, seed=12345, rotations=1):
    """(train, val, test) record lists, augmented on the training side only.

    The unit of the honest split is the loop group of ``dataset.loop_groups`` -- union-find over
    ``near_duplicate`` across the whole merged corpus -- and never the file, for the reason the
    melodic round measured: the packs sell the same loop several times.
    """
    tr, va, te = dataset.split(records, mode, frac_val, frac_test, seed)
    return (augment(records, tr, rotations), [records[i] for i in va], [records[i] for i in te])


def encode(rec, style=None, ctx=256):
    """One record as the parallel arrays of docs/MODEL_FORMAT.md section 3, plus ``kick``.

    Identical to ``dataset.encode`` except for the ``kick`` array and the role, which is 3.
    """
    enc = dataset.encode(rec, style=style, ctx=ctx)
    enc["kick"] = [kick_code(s % 16) for s in rec["steps"][:ctx]]
    return enc


def report(records):
    print(f"{len(records)} bass lines, {sum(len(r['syms']) for r in records)} notes, "
          f"{sum(r['bars'] for r in records)} bars")
    groups = len({(r["role"],) + tuple(r["syms"]) for r in records})
    near = len(dataset.loop_groups(records))
    print(f"  {groups} distinct contents ({len(records) - groups} exact duplicates), "
          f"{near} loop groups ({len(records) - near} near duplicates)")
    lens = sorted(len(r["syms"]) for r in records)
    print(f"  notes per line: median {lens[len(lens) // 2]}, max {lens[-1]}")
    print("  lines per pack:", dict(Counter(r["pack"] for r in records)))
    tr, va, te = build_split(records)
    print(f"  split group: {len(tr)} train / {len(va)} val / {len(te)} test lines, "
          f"{sum(len(r['syms']) for r in te)} test notes, {sum(r['bars'] for r in te)} test bars, "
          f"{100 * dataset.sibling_leak(tr, te):.1f} % of test lines have a sibling in train")


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--packs", default="psy", choices=["psy", "trance"])
    ap.add_argument("--refresh", action="store_true")
    a = ap.parse_args()
    report(load(a.root, BASS_PACKS if a.packs == "psy" else BASS_TRANCE_PACKS, a.refresh))
    return 0


if __name__ == "__main__":
    sys.exit(main())
