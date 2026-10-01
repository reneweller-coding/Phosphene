#!/usr/bin/env python3
"""The listening bench (23.09.2026): every section of every style as a folder of short WAVs.

`phos_render --excerpts DIR` cuts one file per section of every track out of a render (Tools/render/main.cpp).
This script drives it once per style and seed, in parallel, so that a morning's listening can be "all drops
in Full-On" or "every breakdown of seed 7" instead of whole sets:

    python Tools/listen_bench.py --render bin/msvc/phos_render.exe --out work/bench
    python Tools/listen_bench.py --render ... --out work/bench --styles Full-On Goa --seeds 1-3 --minutes 24
    python Tools/listen_bench.py --render ... --out work/bench --set compose.style_tempo=1   # each style at its own tempo

Each style gets a folder, each render its own `index.tsv` and `plan.log` (the `--tracks` print of the render);
`index.tsv` at the top merges every render's index with the style folder in front. Files are named
`s<seed>_t<track>_<style>_<section index>-<section>_b<first bar>.wav`, so sorting a folder by name lists a seed's
sections in order and a search for `-Drop_` finds every drop.

The style is pinned (`compose.style_mix=0`), because the bench asks how *one* style sounds; a mixed set is what
the plugin plays by default and what the ordinary renders measure. Every child runs with `PHOS_MUTE=1` (house
rule: a batch never opens an audio device) and with a share of the probe threads, like Tools/batch_render.py.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List

STYLES = ["Goa", "Full-On", "Progressive", "Dark Forest", "Hi-Tech"]


def parse_seeds(text: str) -> List[int]:
    seeds: List[int] = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            seeds.extend(range(int(lo), int(hi) + 1))
        else:
            seeds.append(int(part))
    return seeds


def run_one(render: str, style: str, seed: int, minutes: float, bars: int, extra: List[str], out: Path, env: Dict[str, str]) -> Dict:
    folder = out / style.replace(" ", "-")
    folder.mkdir(parents=True, exist_ok=True)
    cmd = [render, "--seed", str(seed), "--set", f"compose.style={style}", "--set", "compose.style_mix=0",
           "--minutes", str(minutes), "--excerpts", str(folder), "--excerpt-bars", str(bars), "--tracks"] + extra
    log = folder / f"s{seed}_plan.log"
    t0 = time.time()
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write(" ".join(cmd) + "\n")
        f.flush()
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, env=env)
    # phos_render writes one index.tsv per call; keep it under the seed's name so several seeds can share a folder.
    idx = folder / "index.tsv"
    kept = folder / f"s{seed}_index.tsv"
    if idx.exists():
        if kept.exists():
            kept.unlink()
        idx.rename(kept)
    return {"style": style, "seed": seed, "returncode": rc, "seconds": round(time.time() - t0, 1), "index": kept, "ok": rc == 0 and kept.exists()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--render", required=True, help="phos_render executable")
    ap.add_argument("--out", required=True, help="the bench folder (one subfolder per style)")
    ap.add_argument("--styles", nargs="*", default=STYLES, help="styles to render (default: all five)")
    ap.add_argument("--seeds", default="1", help="set seeds, e.g. 1-3 or 7,42,303 (default 1)")
    ap.add_argument("--minutes", type=float, default=16.0, help="minutes of set per render (default 16: about two tracks)")
    ap.add_argument("--bars", type=int, default=16, help="bars per excerpt (default 16)")
    ap.add_argument("--set", action="append", default=[], help="one more phos_render --set, repeatable")
    ap.add_argument("--workers", type=int, default=5, help="renders at once (default 5)")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    render = str(Path(args.render).resolve())   # CreateProcess wants a path it can find from any working directory
    if not Path(render).exists():
        print(f"no such file: {render}", file=sys.stderr)
        return 2
    seeds = parse_seeds(args.seeds)
    jobs = [(style, seed) for style in args.styles for seed in seeds]
    workers = max(1, min(args.workers, len(jobs)))
    env = dict(os.environ)
    env["PHOS_MUTE"] = "1"
    env["PHOS_PROBE_THREADS"] = str(max(1, min(8, (os.cpu_count() or 1) // workers)))
    extra: List[str] = []
    for s in args.set:
        extra += ["--set", s]

    results: List[Dict] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(run_one, render, style, seed, args.minutes, args.bars, extra, out, env) for style, seed in jobs]
        for fut in concurrent.futures.as_completed(futures):
            r = fut.result()
            results.append(r)
            print(f"{'ok  ' if r['ok'] else 'FAIL'} {r['seconds']:7.1f} s  {r['style']:<12} seed {r['seed']}", flush=True)

    # One index over the bench: the style folder in front of every line of every render's index.
    lines: List[str] = []
    header = None
    for r in sorted(results, key=lambda x: (x["style"], x["seed"])):
        if not r["ok"]:
            continue
        with open(r["index"], encoding="utf-8") as f:
            rows = f.read().splitlines()
        if not rows:
            continue
        if header is None:
            header = "folder\t" + rows[0]
        folder = r["style"].replace(" ", "-")
        lines += [f"{folder}\t{row}" for row in rows[1:]]
    if header is not None:
        with open(out / "index.tsv", "w", encoding="utf-8") as f:
            f.write(header + "\n" + "\n".join(lines) + ("\n" if lines else ""))
        print(f"{len(lines)} excerpts in {out}")
    failed = sum(1 for r in results if not r["ok"])
    return failed


if __name__ == "__main__":
    sys.exit(main())
