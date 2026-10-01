#!/usr/bin/env python3
"""Writes the plan snapshots again (23.09.2026, round "Schnappschuss"; Tests/plan_snapshot.cmake).

Run it when a change to what the composer decides is *meant*; then `git diff Tests/golden` is the record of
what the change did musically, and it goes into the same commit:

    python Tools/update_snapshots.py --plandump build/msvc/Tools/plandump/Release/phos_plandump.exe
    python Tools/update_snapshots.py --plandump ... --check     # compare only, like ctest

A snapshot is `Tests/golden/*.txt`; its first line `# args: ...` says how it was made.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plandump", required=True, help="phos_plandump executable")
    ap.add_argument("--check", action="store_true", help="compare instead of writing")
    args = ap.parse_args()
    plandump = str(Path(args.plandump).resolve())
    script = ROOT / "Tests" / "plan_snapshot.cmake"
    failed = 0
    for golden in sorted((ROOT / "Tests" / "golden").glob("*.txt")):
        cmd = ["cmake", f"-DPLANDUMP={plandump}", f"-DGOLDEN={golden}"]
        if not args.check:
            cmd.append("-DUPDATE=1")
        cmd += ["-P", str(script)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        ok = r.returncode == 0
        failed += 0 if ok else 1
        print(f"{'ok  ' if ok else 'DIFF'} {golden.name}")
        if not ok:
            print(r.stdout + r.stderr)
    return failed


if __name__ == "__main__":
    sys.exit(main())
