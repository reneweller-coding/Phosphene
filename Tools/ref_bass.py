"""What a psytrance bass is, measured between the kicks where a recording plays kick and bass alone.

The companion of `ref_kick.py` and built on it: the same search for a stretch of eight and more
"sparse" beats in the first 90 s of a recording (kick and bass nearly alone, the DJ's mixing
stretch), the same tempo and onset finder. Where `ref_kick.py` measures the kick from its onset to
the first bass slot, this tool measures the three quarter-beat slots after it, where the rolling
bass plays, and prints the median over the beats of the stretch:

    sub       dB  power under 60 Hz          \
    60-120    dB  power in 60 .. 120 Hz       |  each relative to the bass window's power in
    120-300   dB  power in 120 .. 300 Hz      |  20 .. 120 Hz (its fundamental region), so a
    bite      dB  power in 300 Hz .. 2 kHz    |  recording's overall level does not enter
    2-6k      dB  power in 2 .. 6 kHz        /
    b-sub     dB  bite minus sub: the number the brief of 19.09.2026 quotes for our bass (-21 dB)
    b/k       dB  mean K-weighted power of the bass window minus that of the kick window (onset to
                  the first slot): how loud the bass is against the kick, on the same time base
    att       ms  slot start to where the bass envelope (20 Hz .. 2 kHz, 5 ms RMS) first comes within
                  3 dB of its maximum in the slot; median over slots 2 and 3
    len       ms  slot start to the last point within 12 dB of that maximum: how long a note sounds
    batt/blen ms  the same for the bite band (300 Hz .. 2 kHz) alone: how plucky the bite is
    sub' 60-120' bite'  the first columns again over slots 2 and 3 only, out of the kick's tail

Slots 2 and 3 only for the envelopes: slot 1 starts a quarter beat after the kick and still carries
its tail. A kick-alone stretch has no bass to measure and is skipped.

**Selection bias, stated rather than hidden.** A beat is "sparse" when its power above 300 Hz is at
least --sparse dB (default 8: the threshold of the ref_kick.py run of 18.09.2026, which found 24 of
the 40 recordings) under its power in 30 .. 150 Hz. That criterion is what keeps hats,
pads and leads out, but it also caps how much bite a selected bass may have. The tool therefore
takes --sparse and the report runs it at 8 and at 12 dB: if the bite numbers move with the
threshold, the threshold is measuring itself.

Usage:
    python Tools/ref_bass.py --list refs.txt [--sparse 8]
    python Tools/ref_bass.py a.mp3 b.mp3 ...
    python Tools/ref_bass.py render.wav --whole     (a kick + bass render: every beat is used)

Only statistics are printed; nothing of the audio is stored.
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from scipy.signal import butter, lfilter, sosfiltfilt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ref_kick as rk  # noqa: E402

SR = rk.SR
SOS_FULL = butter(4, [20, 2000], btype="bandpass", fs=SR, output="sos")
SOS_BITE = butter(4, [300, 2000], btype="bandpass", fs=SR, output="sos")
KEYS = ["sub", "60-120", "120-300", "bite", "2-6k", "b-sub", "b/k", "att", "len", "batt", "blen", "sub'", "60-120'", "bite'"]


def kweight(x):
    """BS.1770 K-weighting at SR (shelf and RLB high pass, from the analogue prototypes by bilinear)."""
    G, Q, fc = 3.999843853973347, 0.7071752369554196, 1681.974450955533
    A = 10 ** (G / 40)
    w0 = 2 * np.pi * fc / SR
    al = np.sin(w0) / (2 * Q)
    b = [A * ((A + 1) + (A - 1) * np.cos(w0) + 2 * np.sqrt(A) * al), -2 * A * ((A - 1) + (A + 1) * np.cos(w0)),
         A * ((A + 1) + (A - 1) * np.cos(w0) - 2 * np.sqrt(A) * al)]
    a = [(A + 1) - (A - 1) * np.cos(w0) + 2 * np.sqrt(A) * al, 2 * ((A - 1) - (A + 1) * np.cos(w0)),
         (A + 1) - (A - 1) * np.cos(w0) - 2 * np.sqrt(A) * al]
    y = lfilter(b, a, x)
    fc, Q = 38.13547087602444, 0.5003270373238773
    w0 = 2 * np.pi * fc / SR
    al = np.sin(w0) / (2 * Q)
    b = [(1 + np.cos(w0)) / 2, -(1 + np.cos(w0)), (1 + np.cos(w0)) / 2]
    a = [1 + al, -2 * np.cos(w0), 1 - al]
    return lfilter(b, a, y)


def envelope_times(seg):
    """(attack ms, length ms) of one slot, from a 5 ms RMS envelope read every millisecond.

    attack: slot start to the first point within 3 dB of the slot's maximum. length: slot start to
    the last point within 12 dB of it -- how long the note sounds. Thresholds rather than the peak
    itself, because the envelope of a saw-like wave ripples at its own period (an analytic envelope
    fell 12 dB within 2.5 ms of its peak on our own bass), and a first/last crossing is robust to that.
    """
    h = int(0.001 * SR)
    w = int(0.005 * SR)
    env = np.sqrt(np.convolve(seg ** 2, np.ones(w) / w, mode="same"))[::h]
    if env.max() <= 0:
        return float("nan"), float("nan")
    top = env.max()
    att = float(np.nonzero(env >= top * 0.7079)[0][0])
    length = float(np.nonzero(env >= top * 0.2512)[0][-1])
    return att, length


def beat_features(x, xk, o, beat):
    """The bass features of the beat starting at onset `o` (see the module doc)."""
    b = int(beat)
    q = int(beat / 4)
    bass = x[o + q:o + b]
    f = {}
    ref = rk.power(bass, 20, 120)
    for name, lo, hi in (("sub", 20, 60), ("60-120", 60, 120), ("120-300", 120, 300), ("bite", 300, 2000), ("2-6k", 2000, 6000)):
        f[name] = 10 * np.log10(rk.power(bass, lo, hi) / ref)
    f["b-sub"] = f["bite"] - f["sub"]
    # The same over slots 2 and 3 only: the reference kicks stay within 20 dB of their peak until
    # the first slot (ref_kick.py, "body" 104 ms), so slot 1 carries a kick tail at 55 .. 70 Hz that
    # the 60-120 column would otherwise count as bass.
    late = x[o + 2 * q:o + b]
    ref2 = rk.power(late, 20, 120)
    for name, lo, hi in (("sub'", 20, 60), ("60-120'", 60, 120), ("bite'", 300, 2000)):
        f[name] = 10 * np.log10(rk.power(late, lo, hi) / ref2)
    kk = xk[o:o + q]
    kb = xk[o + q:o + b]
    f["b/k"] = 10 * np.log10((np.mean(kb ** 2) + 1e-30) / (np.mean(kk ** 2) + 1e-30))
    # Butterworth rather than the brick wall of rk.band: a brick wall rings for tens of milliseconds
    # on both sides, and the next kick's click leaked backwards into slot 3 of a quiet bite band.
    # The segment ends where the next kick begins.
    full = sosfiltfilt(SOS_FULL, x[o:o + b])
    bite = sosfiltfilt(SOS_BITE, x[o:o + b])
    att, ln, batt, bln = [], [], [], []
    for s in (2, 3):
        a0, a1 = s * q, (s + 1) * q
        t = envelope_times(full[a0:a1])
        att.append(t[0])
        ln.append(t[1])
        t = envelope_times(bite[a0:a1])
        batt.append(t[0])
        bln.append(t[1])
    f["att"], f["len"] = float(np.nanmedian(att)), float(np.nanmedian(ln))
    f["batt"], f["blen"] = float(np.nanmedian(batt)), float(np.nanmedian(bln))
    return f


def analyse(x, whole=False, sparse_db=8.0):
    """(kind, n beats, bpm, median features) of the longest sparse stretch, or None."""
    low = rk.band(x, 30, 150)
    bpm = rk.tempo(low)
    on, beat = rk.onsets(x, low, bpm)
    if len(on) < 8:
        return None
    b = int(beat)
    lows = [np.mean(low[o:o + b] ** 2) for o in on if o + b <= len(x)]
    loudest = max(lows) if lows else 0.0
    sparse, alone = [], []
    for o in on:
        if o + b + b // 4 > len(x):
            sparse.append(False)
            alone.append(False)
            continue
        seg = x[o:o + b]
        lo_p = np.mean(low[o:o + b] ** 2) + 1e-30
        ratio = 10 * np.log10((rk.power(seg, 300, 16000) + 1e-30) / rk.power(seg, 30, 150))
        sparse.append(whole or (ratio < -sparse_db and lo_p > 0.1 * loudest))
        q = b // 4
        head = np.mean(low[o:o + q] ** 2) + 1e-30
        tail = np.mean(low[o + q:o + b] ** 2) + 1e-30
        alone.append(10 * np.log10(tail / head) < -15.0)
    runs, start = [], None
    for i, s in enumerate(sparse + [False]):
        if s and start is None:
            start = i
        elif not s and start is not None:
            runs.append((start, i))
            start = None
    runs = [r for r in runs if r[1] - r[0] >= (1 if whole else 8)]
    if not runs:
        return None
    a, z = max(runs, key=lambda r: r[1] - r[0])
    idx = list(range(a, z))
    kind = "alone" if np.mean([alone[i] for i in idx]) > 0.5 else "k+bass"
    if kind == "alone" and not whole:
        return kind, len(idx), bpm, None
    xk = kweight(x)
    fb = [beat_features(x, xk, on[i], beat) for i in idx]
    med = {k: float(np.nanmedian([f[k] for f in fb])) for k in KEYS}
    return kind, len(idx), bpm, med


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--list", default=None, help="a text file with one recording per line")
    ap.add_argument("--whole", action="store_true", help="every beat counts (a kick + bass render)")
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--sparse", type=float, default=8.0)
    a = ap.parse_args(argv[1:])
    files = list(a.files)
    if a.list:
        files += [ln.strip() for ln in open(a.list, encoding="utf-8") if ln.strip()]
    if not files:
        print(__doc__)
        return 2
    print(f"{'file':34s} {'kind':6s} {'n':>3s} {'bpm':>5s} " + " ".join(f"{k:>7s}" for k in KEYS))
    rows = []
    for p in files:
        try:
            x = rk.decode(p, a.start, a.seconds)
        except subprocess.CalledProcessError:
            print(f"{os.path.basename(p)[:34]:34s} cannot decode")
            continue
        r = analyse(x, a.whole, a.sparse)
        if r is None or r[3] is None:
            print(f"{os.path.basename(p)[:34]:34s} {'no kick+bass stretch' if r is None else 'kick alone'}")
            continue
        rows.append(r)
        print(f"{os.path.basename(p)[:34]:34s} {r[0]:6s} {r[1]:3d} {r[2]:5.1f} " + " ".join(f"{r[3][k]:7.1f}" for k in KEYS), flush=True)
    if len(rows) > 1:
        med = {k: float(np.nanmedian([r[3][k] for r in rows])) for k in KEYS}
        q1 = {k: float(np.nanpercentile([r[3][k] for r in rows], 25)) for k in KEYS}
        q3 = {k: float(np.nanpercentile([r[3][k] for r in rows], 75)) for k in KEYS}
        print(f"{'median (' + str(len(rows)) + ')':34s} {'':6s} {'':3s} {'':5s} " + " ".join(f"{med[k]:7.1f}" for k in KEYS))
        print(f"{'  quartile 1':34s} {'':6s} {'':3s} {'':5s} " + " ".join(f"{q1[k]:7.1f}" for k in KEYS))
        print(f"{'  quartile 3':34s} {'':6s} {'':3s} {'':5s} " + " ".join(f"{q3[k]:7.1f}" for k in KEYS))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
