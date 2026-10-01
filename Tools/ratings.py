#!/usr/bin/env python3
"""The listener's verdicts, read back (23.09.2026, round "Bewertung"; Core/include/phos/Rating.h).

Two sources write them:

* the plugin's Perform tab ("Good here" / "Bad here") appends to `ratings.tsv` in the user's application data
  folder (Windows: `%APPDATA%\\Phosphene\\ratings.tsv`) -- time, seed, track, bar, bar in track, section, style,
  verdict, note, source;
* the listening bench (Tools/listen_bench.py) writes an `index.tsv` whose last two columns, `rating` and `note`,
  are left empty for the listener to fill in ("good" or "bad", and a note) in any spreadsheet or editor.

This script reads any number of either, and prints what a round of work needs: the verdicts per style and
section, and every "bad" with its note and the exact place to hear it again. `--out FILE` writes all of them as
one file in the plugin's format -- with the composer's decisions at each rated bar (the features column) -- which
`phos_render --learn all.tsv preferences.txt` turns into the listener's preferences (Core/include/phos/Preferences.h).

    python Tools/ratings.py                                  # the plugin's file
    python Tools/ratings.py work/bench/index.tsv              # a bench
    python Tools/ratings.py work/bench/index.tsv %APPDATA%/Phosphene/ratings.tsv --out all_ratings.tsv
"""
from __future__ import annotations

import argparse
import collections
import os
import sys
from pathlib import Path
from typing import Dict, List

HEADER = ["time", "seed", "track", "bar", "bar_in_track", "section", "style", "verdict", "note", "source", "features"]


def default_plugin_file() -> Path:
    base = os.environ.get("APPDATA") or str(Path.home() / ".config")
    return Path(base) / "Phosphene" / "ratings.tsv"


def read_file(path: Path) -> List[Dict[str, str]]:
    """Rows of a ratings file or of a bench index, in the ratings format."""
    rows: List[Dict[str, str]] = []
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = [l.rstrip("\r\n") for l in f]
    if not lines:
        return rows
    head = lines[0].split("\t")
    for line in lines[1:]:
        if not line.strip():
            continue
        cells = line.split("\t")
        cells += [""] * (len(head) - len(cells))
        r = dict(zip(head, cells))
        if "verdict" in r:   # the plugin's format
            rows.append({k: r.get(k, "") for k in HEADER})
            continue
        # A bench index: only the rows somebody rated or annotated.
        verdict = (r.get("rating") or "").strip().lower()
        note = (r.get("note") or "").strip()
        if not verdict and not note:
            continue
        verdict = {"+": "good", "gut": "good", "1": "good", "-": "bad", "schlecht": "bad", "-1": "bad"}.get(verdict, verdict)
        if verdict not in ("good", "bad"):
            verdict = "note"
        rows.append({"time": "", "seed": r.get("seed", ""), "track": r.get("track", ""), "bar": r.get("first_bar", ""),
                     "bar_in_track": "", "section": r.get("section", ""), "style": r.get("style", ""), "verdict": verdict,
                     "note": note, "source": f"bench:{r.get('file', '')}", "features": r.get("features", "")})
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", help="ratings.tsv or bench index.tsv files (default: the plugin's ratings.tsv)")
    ap.add_argument("--out", help="write every verdict read as one ratings file")
    args = ap.parse_args()

    files = [Path(f) for f in args.files] or [default_plugin_file()]
    rows: List[Dict[str, str]] = []
    for f in files:
        if not f.exists():
            print(f"no such file: {f}", file=sys.stderr)
            continue
        rows += read_file(f)
    if not rows:
        print("no verdicts found")
        return 0

    table: Dict[tuple, collections.Counter] = collections.defaultdict(collections.Counter)
    for r in rows:
        table[(r["style"] or "?", r["section"] or "?")][r["verdict"]] += 1
    print(f"{len(rows)} verdicts\n")
    print(f"{'style':<14}{'section':<10}{'good':>6}{'bad':>6}{'note':>6}")
    for (style, section), c in sorted(table.items()):
        print(f"{style:<14}{section:<10}{c['good']:>6}{c['bad']:>6}{c['note']:>6}")

    bad = [r for r in rows if r["verdict"] in ("bad", "note")]
    if bad:
        print("\nbad and noted, where to hear them again:")
        for r in bad:
            where = f"seed {r['seed']} track {r['track']} bar {r['bar']}"
            if r["bar_in_track"]:
                where += f" (bar {r['bar_in_track']} of the track)"
            print(f"  {r['verdict']:<5} {where}, {r['style']} {r['section']}: {r['note'] or '-'}   [{r['source']}]")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write("\t".join(HEADER) + "\n")
            for r in rows:
                f.write("\t".join((r[k] or "").replace("\t", " ") for k in HEADER) + "\n")
        print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
