"""Mines one-bar rhythm templates and anchor skeletons from the melodic corpus (22.09.2026).

**Why.** The lead round of 22.09.2026 builds a phrase from a one-bar *cell* and an operator program
instead of sampling eight bars note by note (docs/PLAN.md, block "Lead: Zelle, Operatoren,
Kritiker"). The cell's rhythm and its contour skeleton come from two small pools that the genre
rules filter and the corpus fills -- the user's rule since 18.09.2026: rules over corpus, the MIDI
data only decides what the rules leave open.

**What is counted.** On the deduplicated corpus (``Tools/train/dataset.py``: union-find over near
duplicates up to transposition, so a loop resold in three packs counts once):

  * *bar masks*: for every bar of every line, the 16-bit onset mask of its sixteenths (bit s = an
    onset on sixteenth s); empty bars are left out. The table holds the commonest masks per role
    with their counts.
  * *skeletons*: for every bar, the pitch sounding at the start of beats 2, 3 and 4 relative to the
    pitch at the start of beat 1, in semitones clamped to +-12 -- the four anchors that carry a
    bar's contour, which is what the lead's critic compares a cell against. A beat without an
    onset keeps the note that was sounding into it; a bar whose first beat has no note yet is left
    out (nothing to anchor to).

Counts only, as everything in Core/src/CorpusTables.cpp: no loop can be read back from a table of
bar masks and three-interval skeletons.

Usage:
    python Tools/corpus/lead_templates.py --root M:/Midi --report
    python Tools/corpus/build_corpus.py --out Core/src/CorpusTables.cpp --lead-templates-only
"""
import argparse
import importlib.util
import os
import sys
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_TRAIN = os.path.normpath(os.path.join(_HERE, "..", "train"))

MARKER = "// ---------------------------------------------------------------------------- lead templates"
ROLES = ["acid", "lead", "arp"]
REL_MIN = -12
TOP_MASKS = 64
TOP_SKELETONS = 48


def _dataset():
    spec = importlib.util.spec_from_file_location("phos_dataset", os.path.join(_TRAIN, "dataset.py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules["phos_dataset"] = mod
    spec.loader.exec_module(mod)
    return mod


def bar_masks(rec):
    """The 16-bit onset masks of a line's bars, empty bars left out."""
    masks = [0] * rec["bars"]
    for s in rec["steps"]:
        b = s // 16
        if 0 <= b < len(masks):
            masks[b] |= 1 << (s % 16)
    return [m for m in masks if m]


def skeletons(rec):
    """(d1, d2, d3) per bar: the pitch at beats 2..4 relative to beat 1, semitones clamped to +-12."""
    steps, syms = rec["steps"], rec["syms"]
    out = []
    notes = {s: syms[i] + REL_MIN for i, s in enumerate(steps)}
    sounding = None
    for b in range(rec["bars"]):
        anchors = []
        for beat in range(4):
            first = b * 16 + beat * 4
            hit = None
            for s in range(first, first + 4):
                if s in notes:
                    hit = notes[s]
                    break
            if beat == 0:
                # the note sounding at the bar's start: an onset in the first beat, else what holds into it
                held = sounding if first not in notes else notes[first]
                anchors.append(notes[first] if first in notes else (hit if hit is not None else held))
            else:
                anchors.append(hit if hit is not None else anchors[-1])
            for s in range(first, first + 4):
                if s in notes:
                    sounding = notes[s]
        if anchors[0] is None or any(a is None for a in anchors):
            continue
        out.append(tuple(max(-12, min(12, a - anchors[0])) for a in anchors[1:]))
    return out


def mine(root, report=False):
    ds = _dataset()
    recs = ds.load(root, ds.PSY_PACKS)
    kept, removed = ds.dedupe(recs)
    masks = {r: Counter() for r in ROLES}
    skels = {r: Counter() for r in ROLES}
    lines = {r: 0 for r in ROLES}
    for rec in kept:
        role = ROLES[rec["role"]]
        lines[role] += 1
        masks[role].update(bar_masks(rec))
        skels[role].update(skeletons(rec))
    if report:
        print(f"{len(recs)} lines, {removed} near duplicates dropped, {len(kept)} kept")
        for r in ROLES:
            tm, ts = sum(masks[r].values()), sum(skels[r].values())
            topm = sum(n for _, n in masks[r].most_common(TOP_MASKS))
            tops = sum(n for _, n in skels[r].most_common(TOP_SKELETONS))
            print(f"{r}: {lines[r]} lines, {tm} bars, {len(masks[r])} distinct masks "
                  f"(top {TOP_MASKS} cover {100.0 * topm / max(1, tm):.0f} %), "
                  f"{len(skels[r])} distinct skeletons (top {TOP_SKELETONS} cover {100.0 * tops / max(1, ts):.0f} %)")
            for m, n in masks[r].most_common(8):
                print(f"   {''.join('x' if (m >> s) & 1 else '.' for s in range(16))}  {bin(m).count('1'):2d} onsets  {n}")
            for d, n in skels[r].most_common(6):
                print(f"   skeleton {d[0]:+d} {d[1]:+d} {d[2]:+d}  {n}")
    return masks, skels, lines


def emit_lines(root):
    """The C++ block for Core/src/CorpusTables.cpp (Corpus.h: CorpusBarMask, CorpusSkeleton)."""
    masks, skels, lines = mine(root)
    out = [MARKER, "namespace {"]
    for r in ROLES:
        top = masks[r].most_common(TOP_MASKS)
        top.sort(key=lambda kv: (-kv[1], kv[0]))
        body = ", ".join(f"{{{m},{n}}}" for m, n in top) or "{0,0}"
        out.append(f"const CorpusBarMask k_{r}_bars[] = {{ {body} }};")
        tops = skels[r].most_common(TOP_SKELETONS)
        tops.sort(key=lambda kv: (-kv[1], kv[0]))
        body = ", ".join(f"{{{{{d[0]},{d[1]},{d[2]}}},{n}}}" for d, n in tops) or "{{0,0,0},0}"
        out.append(f"const CorpusSkeleton k_{r}_skeletons[] = {{ {body} }};")
    out.append("} // namespace")
    out.append("")
    out.append("const CorpusBarMask* const kCorpusRoleBars[kNumCorpusRoles] = { " + ", ".join(f"k_{r}_bars" for r in ROLES) + " };")
    out.append("const int kNumCorpusRoleBars[kNumCorpusRoles] = { " + ", ".join(str(max(1, min(TOP_MASKS, len(masks[r])))) for r in ROLES) + " };")
    out.append("const CorpusSkeleton* const kCorpusRoleSkeletons[kNumCorpusRoles] = { " + ", ".join(f"k_{r}_skeletons" for r in ROLES) + " };")
    out.append("const int kNumCorpusRoleSkeletons[kNumCorpusRoles] = { " + ", ".join(str(max(1, min(TOP_SKELETONS, len(skels[r])))) for r in ROLES) + " };")
    out.append("const int kCorpusTemplateLines[kNumCorpusRoles] = { " + ", ".join(str(lines[r]) for r in ROLES) + " };")
    out.append("")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    mine(a.root, report=True)
    if not a.report:
        print("\n".join(emit_lines(a.root)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
