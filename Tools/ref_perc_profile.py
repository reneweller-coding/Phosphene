"""Where percussion sits in the sixteenth grid of reference recordings.

For every file: decode 60 s from the middle, find tempo and kick phase from the bass band (as
ref_slot_profile.py does), then compute onset strength -- positive spectral flux of the log
magnitude -- in three bands and fold it into 16 slices per beat:

    hats     6 .. 14 kHz   closed/open hats, shakers, rides
    snare    1.5 .. 4 kHz  claps, snares, rims (leads leak in here, so read it with care)
    low-mid  150 .. 500 Hz toms, congas (and bass harmonics)

Printed per band: the 16-slice profile normalised to its maximum (in %), the share of onset
strength on the eighth offbeats (slices 8), on the other sixteenths, and on the beats. Nothing of
the audio is stored.

Usage:
    python Tools/ref_perc_profile.py file1.mp3 file2.mp3 ...
    python Tools/ref_perc_profile.py --dir "C:/Music/Psytrance"
"""
import math
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ref_slot_profile as rsp  # noqa: E402

SR = 32000
BANDS = [("hats", 6000.0, 14000.0), ("snare", 1500.0, 4000.0), ("low-mid", 150.0, 500.0)]


def decode(path, start, dur, sr):
    cmd = ["ffmpeg", "-v", "quiet", "-ss", str(start), "-t", str(dur), "-i", path, "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def kick_grid(path):
    """Tempo and the sample offset (at SR) of a kick, from the bass band at 8 kHz."""
    x = decode(path, 150, 60, rsp.SR)
    band = rsp.bandpass(x, 40, 200)
    hop = 40
    bpm = rsp.tempo(rsp.envelope(band, hop), hop)
    beat = 60.0 / bpm * rsp.SR
    e2 = band ** 2
    nb = int(len(e2) // beat) - 1
    best, bestScore = 0.0, -1e30
    for off in np.linspace(0, beat, 128, endpoint=False):
        s = np.zeros(16)
        for b in range(nb):
            t0 = off + b * beat
            for k in (0, 4, 8, 12):
                a, z = int(t0 + k * beat / 16), int(t0 + (k + 1) * beat / 16)
                s[k] += e2[a:z].mean()
        score = s[0] - 0.33 * (s[4] + s[8] + s[12])
        if score > bestScore:
            bestScore, best = score, off
    return bpm, best * SR / rsp.SR


def flux_profile(x, bpm, offset, lo, hi):
    n, hop = 1024, 160  # 5 ms hop
    win = np.hanning(n)
    frames = (len(x) - n) // hop
    f = np.fft.rfftfreq(n, 1.0 / SR)
    sel = (f >= lo) & (f <= hi)
    prev = None
    flux = np.zeros(frames)
    for i in range(frames):
        m = np.log1p(1000.0 * np.abs(np.fft.rfft(x[i * hop:i * hop + n] * win))[sel])
        if prev is not None:
            flux[i] = np.maximum(m - prev, 0.0).sum()
        prev = m
    t = (np.arange(frames) * hop + n / 2)          # frame centre in samples
    beat = 60.0 / bpm * SR
    phase = ((t - offset) / beat) % 1.0
    slot = np.minimum((phase * 16).astype(int), 15)
    prof = np.array([flux[slot == k].mean() for k in range(16)])
    return prof


def main(argv):
    args = argv[1:]
    files = []
    if args and args[0] == "--dir":
        for root, _, names in os.walk(args[1]):
            files += [os.path.join(root, n) for n in sorted(names) if n.lower().endswith(rsp.AUDIO_EXT)]
    else:
        files = args
    if not files:
        print(__doc__)
        return 2
    for path in files:
        try:
            bpm, offset = kick_grid(path)
            x = decode(path, 150, 60, SR)
        except subprocess.CalledProcessError:
            print(f"{os.path.basename(path)[:44]:44s} cannot decode")
            continue
        # The bass-band energy places the grid a slice or two late (the kick's energy lasts); its
        # onset flux does not. Rotate every profile so the kick band's onset peak is slice 0.
        kickProf = flux_profile(x, bpm, offset, 40.0, 120.0)
        # A rolling bass puts a low onset on every sixteenth too, so a single slice maximum is
        # ambiguous. The kick is the sixteenth group (four slices around a grid position) that stands
        # out most against the other three.
        def groups(p, r):
            return np.array([p[[(r + 4 * n + d) % 16 for d in (-1, 0, 1, 2)]].sum() for n in range(4)])
        k0 = max(range(16), key=lambda r: groups(kickProf, r)[0] - groups(kickProf, r)[1:].mean())
        print(f"{os.path.basename(path)[:44]:44s} {bpm:5.1f} BPM   (grid corrected by {k0} of 16 slices)")
        for name, lo, hi in BANDS:
            p = np.roll(flux_profile(x, bpm, offset, lo, hi), -k0)
            p = p - p.min()                       # remove the steady floor
            # Onset share per sixteenth: slices 4n-1 .. 4n+2 around each grid position.
            six = np.array([p[[(4 * n - 1) % 16, 4 * n, 4 * n + 1, 4 * n + 2]].sum() for n in range(4)])
            six = 100.0 * six / (six.sum() + 1e-12)
            pct = 100.0 * p / (p.max() + 1e-12)
            print(f"   {name:8s} " + " ".join(f"{v:3.0f}" for v in pct)
                  + f"   16ths: beat {six[0]:3.0f}%  e {six[1]:3.0f}%  and {six[2]:3.0f}%  a {six[3]:3.0f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
