"""Spectral balance of recordings: energy per band relative to the kick-and-bass band.

For every file, 60 s from the middle (or the whole file if shorter): power in
    low        40 .. 140 Hz   kick and bass
    low-mid   140 .. 500 Hz
    mid       500 .. 1500 Hz
    presence 1.5 .. 6 kHz     claps, snares, rims, leads
    air        6 .. 16 kHz    hats, shakers, rides
printed in dB relative to the low band. Used to calibrate the percussion levels against the reference
tracks: a render whose air band sits far below the references' is missing its hats.

Usage:
    python Tools/ref_band_balance.py file1.mp3 file2.wav ...
"""
import os
import subprocess
import sys

import numpy as np

SR = 44100
BANDS = [("low", 40, 140), ("low-mid", 140, 500), ("mid", 500, 1500), ("presence", 1500, 6000), ("air", 6000, 16000)]


def decode(path):
    probe = subprocess.run(["ffprobe", "-v", "quiet", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                           capture_output=True, text=True)
    try:
        dur = float(probe.stdout.strip())
    except ValueError:
        dur = 0.0
    start = max(0.0, dur / 2 - 30) if dur > 70 else 0.0
    cmd = ["ffmpeg", "-v", "quiet", "-ss", str(start), "-t", "60", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def balance(x):
    n = 1 << 15
    win = np.hanning(n)
    f = np.fft.rfftfreq(n, 1 / SR)
    acc = np.zeros(len(f))
    frames = 0
    for i in range(0, len(x) - n, n // 2):
        acc += np.abs(np.fft.rfft(x[i:i + n] * win)) ** 2
        frames += 1
    acc /= max(frames, 1)
    power = {name: acc[(f >= lo) & (f < hi)].sum() for name, lo, hi in BANDS}
    return {name: 10 * np.log10(power[name] / power["low"]) for name, _, _ in BANDS}


def main(argv):
    files = argv[1:]
    if not files:
        print(__doc__)
        return 2
    rows = []
    for path in files:
        b = balance(decode(path))
        rows.append(b)
        print(f"{os.path.basename(path)[:44]:44s} " + "  ".join(f"{k} {v:+6.1f}" for k, v in b.items() if k != "low"))
    if len(rows) > 1:
        keys = [k for k, _, _ in BANDS if k != "low"]
        print(f"{'median':44s} " + "  ".join(f"{k} {np.median([r[k] for r in rows]):+6.1f}" for k in keys))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
