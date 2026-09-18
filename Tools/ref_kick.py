"""What a psytrance kick is, measured where a recording plays it (nearly) alone.

Many tracks open with the kick alone, or with kick and bass and nothing else, for eight or sixteen
bars: the DJ's mixing stretch. This tool finds such stretches in the first 90 s of every recording
and measures the kick there, kick by kick, then prints the median over the kicks:

    f_end     Hz  the kick's end pitch: the mean period of the last two whole cycles of the low-passed
                  kick (under 250 Hz) before its envelope falls 24 dB under the peak or the window ends
    sub-low   dB  power below 60 Hz minus power in 60 .. 120 Hz
    click     dB  power in 2 .. 5 kHz minus power in 40 .. 120 Hz ("the click against the body")
    crest     dB  peak over RMS
    attack    ms  onset to the envelope peak
    body      ms  onset to the point where the low-passed envelope has fallen 20 dB under its peak

All of it over the window from the onset to the first bass slot, a quarter beat later -- the only part
of a kick that a kick-plus-bass stretch shows cleanly, and so the one window in which both kinds of
stretch, and a Phosphene render, are comparable. For kick-alone stretches the same numbers over the
whole beat are printed as well (`beat` columns), because a kick's sub lives longer than 100 ms.

A beat counts as "sparse" when its power in 300 Hz .. 16 kHz is at least --sparse dB (default 18)
under its power in 30 .. 150 Hz, and it is loud in that low band (within 10 dB of the loudest beat of
the window, which rules out an ambient intro). A stretch needs eight sparse beats in a row. It is
"kick alone" when the last three quarters of its beats hold at least 15 dB less low-band power than
their first quarter, and "kick + bass" otherwise.

Usage:
    python Tools/ref_kick.py --dir "C:/.../Pop - Kopie" [--album "Psytrance Collection"]
    python Tools/ref_kick.py render.wav --whole       (a kick solo render: every beat is used)
    python Tools/ref_kick.py a.mp3 b.mp3 ...

Only statistics are printed; nothing of the audio is stored.
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from scipy.signal import hilbert

SR = 44100
AUDIO_EXT = (".mp3", ".flac", ".wav", ".m4a", ".ogg", ".aif", ".aiff")


def tags(path):
    """(album, duration) of a file, from ffprobe; tags decoded as UTF-8 explicitly."""
    out = subprocess.run(["ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", path],
                         capture_output=True).stdout.decode("utf-8", "replace")
    try:
        f = json.loads(out).get("format", {})
    except json.JSONDecodeError:
        return "", 0.0
    t = {k.lower(): v for k, v in f.get("tags", {}).items()}
    try:
        return t.get("album", ""), float(f.get("duration", 0.0))
    except (TypeError, ValueError):
        return t.get("album", ""), 0.0


def decode(path, start, dur):
    cmd = ["ffmpeg", "-v", "quiet", "-ss", str(start), "-t", str(dur), "-i", path,
           "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def band(x, lo, hi):
    """Brick-wall band pass by FFT (zero phase: onsets stay where they are)."""
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, len(x))


def power(x, lo, hi):
    """Power of `x` in [lo, hi) Hz, from a periodogram zero-padded to 2^16.

    The segment starts at a kick's onset, so a Hann window would multiply the click -- the first two
    milliseconds -- by nearly zero. The window is flat instead, with a 3 ms half-cosine fade at the
    end only, where the next event would otherwise leak in as a step.
    """
    n = max(1 << 16, 1 << int(np.ceil(np.log2(max(len(x), 2)))))
    w = np.ones(len(x))
    fade = min(len(x) // 4, int(0.003 * SR))
    if fade > 0:
        w[-fade:] = 0.5 + 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    X = np.abs(np.fft.rfft(x * w, n)) ** 2
    f = np.fft.rfftfreq(n, 1 / SR)
    return X[(f >= lo) & (f < hi)].sum() + 1e-30


def tempo(low, lo=128.0, hi=160.0):
    """Tempo from the autocorrelation of the low band's 5 ms energy envelope (1, 2 and 4 beats)."""
    hop = int(0.005 * SR)
    env = np.array([np.mean(low[i * hop:(i + 1) * hop] ** 2) for i in range(len(low) // hop)])
    env = env - env.mean()
    best, bestv = 145.0, -1e30
    for bpm10 in range(int(lo * 10), int(hi * 10) + 1):
        bpm = bpm10 / 10
        v = 0.0
        for m in (1, 2, 4):
            L = 60.0 / bpm * SR / hop * m
            i0 = int(L)
            fr = L - i0
            if i0 + 2 >= len(env):
                continue
            c = np.dot(env[:-i0 - 1], env[i0:len(env) - 1]) * (1 - fr) + np.dot(env[:-i0 - 1], env[i0 + 1:]) * fr
            v += c / (len(env) - i0 - 1)
        if v > bestv:
            bestv, best = v, bpm
    return best


def onsets(x, low, bpm):
    """Sample index of every kick: the grid from the folded low band, each refined to its own rise."""
    beat = 60.0 / bpm * SR
    e2 = low ** 2
    nb = int(len(e2) // beat) - 1
    best, bestScore = 0.0, -1e30
    for off in np.linspace(0, beat, 256, endpoint=False):
        s = np.zeros(16)
        for b in range(nb):
            t0 = off + b * beat
            for k in (0, 4, 8, 12):
                a, z = int(t0 + k * beat / 16), int(t0 + (k + 1) * beat / 16)
                s[k] += e2[a:z].mean()
        score = s[0] - 0.33 * (s[4] + s[8] + s[12])
        if score > bestScore:
            bestScore, best = score, off
    lp = np.abs(band(x, 20, 300))
    out = []
    w = int(0.02 * SR)
    for b in range(nb):
        g = int(best + b * beat)
        a, z = max(0, g - w), min(len(x), g + 2 * w)
        seg = lp[a:z]
        if len(seg) < w:
            continue
        pk = seg.max()
        above = np.nonzero(seg > 0.2 * pk)[0]
        if len(above) == 0:
            continue
        out.append(max(0, a + int(above[0]) - int(0.001 * SR)))
    return out, beat


def features(k, beat):
    """The kick features of one segment `k` that starts at its onset (see the module doc)."""
    f = {}
    f["sub-low"] = 10 * np.log10(power(k, 20, 60) / power(k, 60, 120))
    f["click"] = 10 * np.log10(power(k, 2000, 5000) / power(k, 40, 120))
    rms = np.sqrt(np.mean(k ** 2)) + 1e-30
    f["crest"] = 20 * np.log10(np.max(np.abs(k)) / rms)
    # Envelopes from the analytic signal, sampled every millisecond. A short RMS window would ripple
    # at the kick's own period (20 ms at 50 Hz) by more than the 20 dB the body length is read at.
    h = int(0.001 * SR)
    env = np.abs(hilbert(k))[::h]
    ipk = int(np.argmax(env))
    f["attack"] = ipk * 1.0
    lpk = band(np.concatenate([k, np.zeros(len(k))]), 20, 250)[:len(k)]
    lenv = np.convolve(np.abs(hilbert(lpk)), np.ones(h) / h, mode="same")[::h]
    lp = int(np.argmax(lenv))
    below = np.nonzero(lenv[lp:] < lenv[lp] * 0.1)[0]
    f["body"] = float(lp + below[0]) if len(below) else float(len(lenv))
    # End pitch: zero crossings (upward) of the low-passed kick up to where it falls 24 dB.
    stop = np.nonzero(lenv[lp:] < lenv[lp] * 0.063)[0]
    end = (lp + int(stop[0])) * h if len(stop) else len(lpk)
    s = lpk[:end]
    up = np.nonzero((s[:-1] < 0) & (s[1:] >= 0))[0]
    if len(up) >= 3:
        # Sub-sample crossing times, then the last two periods.
        t = up + s[up] / (s[up] - s[up + 1])
        f["f_end"] = SR / np.mean(np.diff(t[-3:]))
    else:
        f["f_end"] = float("nan")
    return f


def analyse(x, whole=False, sparse_db=18.0):
    """(kind, n kicks, median features over the quarter beat, median over the whole beat or None)."""
    low = band(x, 30, 150)
    bpm = tempo(low)
    on, beat = onsets(x, low, bpm)
    if len(on) < 8:
        return None
    b = int(beat)
    sparse, alone, ratio = [], [], []
    lows = [np.mean(low[o:o + b] ** 2) for o in on if o + b <= len(x)]
    loudest = max(lows) if lows else 0.0
    for o in on:
        if o + b > len(x):
            sparse.append(False)
            alone.append(False)
            ratio.append(0.0)
            continue
        seg = x[o:o + b]
        lo_p = np.mean(low[o:o + b] ** 2) + 1e-30
        hi_p = power(seg, 300, 16000) / len(seg)
        lo_spec = power(seg, 30, 150) / len(seg)
        ratio.append(10 * np.log10(hi_p / lo_spec))
        ok = whole or (ratio[-1] < -sparse_db and lo_p > 0.1 * loudest)
        sparse.append(ok)
        q = b // 4
        head = np.mean(low[o:o + q] ** 2) + 1e-30
        tail = np.mean(low[o + q:o + b] ** 2) + 1e-30
        alone.append(10 * np.log10(tail / head) < -15.0)
    # The longest run of sparse beats.
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
    q = int(beat / 4)
    fq = [features(x[on[i]:on[i] + q], beat) for i in idx]
    med = {k: float(np.nanmedian([f[k] for f in fq])) for k in fq[0]}
    med["hi/low"] = float(np.median([ratio[i] for i in idx]))
    medb = None
    if kind == "alone":
        fb = [features(x[on[i]:on[i] + b], beat) for i in idx]
        medb = {k: float(np.nanmedian([f[k] for f in fb])) for k in fb[0]}
    return kind, len(idx), bpm, med, medb


KEYS = ["f_end", "sub-low", "click", "crest", "attack", "body", "hi/low"]


def row(name, r):
    kind, n, bpm, med, medb = r
    s = f"{name[:40]:40s} {kind:7s} {n:3d} {bpm:5.1f}  " + " ".join(f"{med[k]:7.1f}" for k in KEYS)
    if medb:
        s += "   beat: " + " ".join(f"{medb[k]:6.1f}" for k in ("f_end", "sub-low", "click", "crest", "body"))
    return s


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--dir", default=None)
    ap.add_argument("--album", default="Psytrance Collection")
    ap.add_argument("--whole", action="store_true", help="every beat counts (a kick solo render)")
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--sparse", type=float, default=18.0)
    a = ap.parse_args(argv[1:])
    files = list(a.files)
    if a.dir:
        for root, _, names in os.walk(a.dir):
            for n in sorted(names):
                p = os.path.join(root, n)
                if n.lower().endswith(AUDIO_EXT) and tags(p)[0].strip().lower() == a.album.strip().lower():
                    files.append(p)
    if not files:
        print(__doc__)
        return 2
    print(f"{'file':40s} {'kind':7s} {'n':>3s} {'bpm':>5s}  " + " ".join(f"{k:>7s}" for k in KEYS))
    rows = []
    for p in files:
        try:
            x = decode(p, a.start, a.seconds)
        except subprocess.CalledProcessError:
            print(f"{os.path.basename(p)[:40]:40s} cannot decode")
            continue
        r = analyse(x, a.whole, a.sparse)
        if r is None:
            print(f"{os.path.basename(p)[:40]:40s} no sparse stretch")
            continue
        rows.append(r)
        print(row(os.path.basename(p), r))
    if len(rows) > 1:
        for kind in ("alone", "k+bass", None):
            sel = [r for r in rows if kind is None or r[0] == kind]
            if not sel:
                continue
            med = {k: float(np.nanmedian([r[3][k] for r in sel])) for k in KEYS}
            q1 = {k: float(np.nanpercentile([r[3][k] for r in sel], 25)) for k in KEYS}
            q3 = {k: float(np.nanpercentile([r[3][k] for r in sel], 75)) for k in KEYS}
            label = f"median {kind or 'all'} ({len(sel)})"
            print(f"{label:40s} {'':7s} {'':3s} {'':5s}  " + " ".join(f"{med[k]:7.1f}" for k in KEYS))
            print(f"{'  quartiles':40s} {'':7s} {'':3s} {'':5s}  " + " ".join(f"{q1[k]:3.0f}..{q3[k]:<3.0f}" for k in KEYS))
            beats = [r[4] for r in sel if r[4]]
            if beats:
                print(f"{'  whole beat, kick alone':40s} " + " ".join(
                    f"{k} {np.nanmedian([b[k] for b in beats]):.1f}" for k in ("f_end", "sub-low", "click", "crest", "body")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
