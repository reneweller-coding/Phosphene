#!/usr/bin/env python3
"""Render many Phosphene jobs at once (round "speed", 20.09.2026).

One `phos_render` uses one core at about eight times real time, and the measurement scripts of earlier
rounds rendered their thirty tracks one after the other on a machine with 24 threads. This script runs a
list of jobs through a pool of processes (8 by default), keeps every job's console output beside its
WAV, and reports what each cost.

Jobs come from a JSON file or from a seed range on the command line:

    python Tools/batch_render.py --render build-plugin/Tools/render/Release/phos_render.exe \
        --seeds 1-30 --bars 256 --set compose.style="Full-On" --out-dir out/batch

    python Tools/batch_render.py --render ...phos_render.exe --jobs jobs.json

A jobs file is a list of objects; every key but `out` is optional:

    [{"seed": 864566672, "bars": 96, "out": "out/a.wav",
      "set": ["compose.level_match=Off"], "args": ["--tracks", "--report"]}]

`set` entries become `--set` options, `args` is passed through as it is (`--tracks`, `--solo kick`,
`--minutes 10`, ...). A job without `bars` and without a length in `args` renders phos_render's default.

From another script:

    from batch_render import run_jobs
    results = run_jobs(jobs, render="...phos_render.exe", workers=8)

What the script sets for every child, and why:

* `PHOS_MUTE=1` -- a batch must never open an audio device (house rules).
* `PHOS_PROBE_THREADS` -- the composer's probe renders run in parallel inside one phos_render
  (Core/include/phos/Probe.h, up to 8 threads while a track is planned). With several renders at once that
  would oversubscribe the machine, so each child gets `cpu_count // workers` threads, at least 1 and at
  most 8. `--probe-threads` overrides it. The rendered audio does not depend on it (bit-identical plans).
* `PHOS_PROBE_CACHE` -- only with `--probe-cache DIR`: probe renders are then shared between the jobs of
  this batch and of later ones (jobs that plan the same seed with the same settings, e.g. a `--solo`
  series, plan it once). Safe across processes; entries are keyed by the core's build id.

The exit code is the number of failed jobs (0 = all rendered).
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional


def parse_seeds(text: str) -> List[int]:
    """'1-5,9,864566672' -> [1, 2, 3, 4, 5, 9, 864566672]."""
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


def command_of(job: Dict[str, Any], render: str) -> List[str]:
    """The phos_render command line of one job."""
    cmd = [render]
    if "seed" in job:
        cmd += ["--seed", str(job["seed"])]
    if "bars" in job:
        cmd += ["--bars", str(job["bars"])]
    for s in job.get("set", []):
        cmd += ["--set", s]
    cmd += [str(a) for a in job.get("args", [])]
    cmd += ["--out", str(job["out"])]
    return cmd


def run_one(job: Dict[str, Any], render: str, env: Dict[str, str]) -> Dict[str, Any]:
    """Runs one job; its console output goes to `<out>.log` (or job["log"])."""
    out = Path(job["out"])
    out.parent.mkdir(parents=True, exist_ok=True)
    log = Path(job.get("log", str(out) + ".log"))
    cmd = command_of(job, render)
    t0 = time.time()
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write(" ".join(cmd) + "\n")
        f.flush()
        # The working directory stays the caller's, so relative `out` paths mean what they say; phos_render
        # finds its data beside itself or in the source tree (Tools/render/main.cpp, installDataSearchPath).
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, env=env)
    return {"out": str(out), "log": str(log), "returncode": rc, "seconds": round(time.time() - t0, 2),
            "seed": job.get("seed"), "ok": rc == 0 and out.exists()}


def run_jobs(jobs: Iterable[Dict[str, Any]], render: str, workers: int = 8, probe_threads: Optional[int] = None,
             probe_cache: Optional[str] = None, quiet: bool = False) -> List[Dict[str, Any]]:
    """Renders `jobs` with `workers` processes at once; returns one result per job, in the jobs' order."""
    jobs = list(jobs)
    outs = [str(Path(j["out"]).resolve()) for j in jobs]
    if len(set(outs)) != len(outs):
        raise ValueError("two jobs write the same file")
    workers = max(1, min(workers, len(jobs) or 1))
    env = dict(os.environ)
    env["PHOS_MUTE"] = "1"
    if probe_threads is None:
        probe_threads = max(1, min(8, (os.cpu_count() or 1) // workers))
    env["PHOS_PROBE_THREADS"] = str(probe_threads)
    if probe_cache:
        Path(probe_cache).mkdir(parents=True, exist_ok=True)
        env["PHOS_PROBE_CACHE"] = str(Path(probe_cache).resolve())
    else:
        env.pop("PHOS_PROBE_CACHE", None)
    results: List[Optional[Dict[str, Any]]] = [None] * len(jobs)
    done = 0
    # Threads, not processes: each one only waits for its child.
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(run_one, job, render, env): i for i, job in enumerate(jobs)}
        for fut in concurrent.futures.as_completed(futures):
            i = futures[fut]
            try:
                results[i] = fut.result()
            except Exception as e:   # the job, not the batch, failed
                results[i] = {"out": str(jobs[i].get("out")), "returncode": -1, "seconds": 0.0, "ok": False, "error": str(e)}
            done += 1
            if not quiet:
                r = results[i]
                print(f"[{done}/{len(jobs)}] {'ok  ' if r['ok'] else 'FAIL'} {r['seconds']:7.1f} s  {r['out']}", flush=True)
    return [r for r in results if r is not None]


def main() -> int:
    ap = argparse.ArgumentParser(description="Render a list of Phosphene jobs with a pool of phos_render processes.")
    ap.add_argument("--render", required=True, help="phos_render executable")
    ap.add_argument("--jobs", help="JSON file: a list of {seed, bars, set, args, out}")
    ap.add_argument("--seeds", help="instead of --jobs: seeds, e.g. 1-30 or 3,5,864566672")
    ap.add_argument("--bars", type=int, help="with --seeds: bars per render")
    ap.add_argument("--set", action="append", default=[], help="with --seeds: a phos_render --set, repeatable")
    ap.add_argument("--arg", action="append", default=[], help="with --seeds: one more phos_render argument, repeatable (--arg=--tracks)")
    ap.add_argument("--out-dir", help="with --seeds: directory of the WAVs")
    ap.add_argument("--name", default="seed{seed}.wav", help="with --seeds: file name pattern (default seed{seed}.wav)")
    ap.add_argument("--workers", type=int, default=8, help="renders at once (default 8)")
    ap.add_argument("--probe-threads", type=int, help="PHOS_PROBE_THREADS of every child (default cpu_count // workers, 1..8)")
    ap.add_argument("--probe-cache", help="a directory for PHOS_PROBE_CACHE, shared by all jobs")
    ap.add_argument("--summary", help="write the results as JSON here")
    a = ap.parse_args()

    if a.jobs:
        with open(a.jobs, "r", encoding="utf-8") as f:
            jobs = json.load(f)
    elif a.seeds and a.out_dir:
        jobs = []
        for seed in parse_seeds(a.seeds):
            job: Dict[str, Any] = {"seed": seed, "set": list(a.set), "args": list(a.arg),
                                   "out": str(Path(a.out_dir) / a.name.format(seed=seed))}
            if a.bars:
                job["bars"] = a.bars
            jobs.append(job)
    else:
        ap.error("give --jobs FILE, or --seeds with --out-dir")
    if not Path(a.render).exists():
        ap.error(f"no such executable: {a.render}")

    t0 = time.time()
    results = run_jobs(jobs, a.render, a.workers, a.probe_threads, a.probe_cache)
    wall = time.time() - t0
    failed = [r for r in results if not r["ok"]]
    total = sum(r["seconds"] for r in results)
    print(f"{len(results)} jobs, {len(failed)} failed, {wall:.1f} s wall, {total:.1f} s summed "
          f"({total / wall if wall > 0 else 0:.1f}x)")
    for r in failed:
        print(f"  FAILED ({r['returncode']}): {r['out']}  -- see {r.get('log', '')}")
    if a.summary:
        with open(a.summary, "w", encoding="utf-8") as f:
            json.dump({"wall_seconds": round(wall, 2), "results": results}, f, indent=1)
    return min(len(failed), 255)


if __name__ == "__main__":
    sys.exit(main())
