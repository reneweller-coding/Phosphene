"""Per-sixteenth low-band energy profile of reference tracks.

Answers questions like "is the first bass note after the kick quieter than the two that follow?"
on real recordings. For every file: decode 60 s from the middle (ffmpeg), band-pass 40..200 Hz,
find the tempo by autocorrelation of the band's energy envelope (130..155 BPM), fold the energy
into 16 slices per beat aligned so that slice 0 is the kick, and print the slices in dB relative to
the loudest, plus the energy of bass notes 2, 3 and 4 (slices 4..7, 8..11, 12..15) against note 3.

Usage:
    python Tools/ref_slot_profile.py track1.mp3 track2.flac ...
    python Tools/ref_slot_profile.py --dir "C:/Music/Psytrance"        (every audio file in the folder)

Needs ffmpeg on the PATH and numpy. Nothing of the audio is stored; only the printed statistics.
Measurement of 15.09.2026 on nine tracks: note 2 against note 3 within +-1.2 dB everywhere; the
"louder" note 4 is the next kick's onset in the last two slices, not the bass.
"""
import math
import os
import subprocess
import sys

import numpy as np

SR = 8000
AUDIO_EXT = (".mp3", ".flac", ".wav", ".m4a", ".ogg", ".aif", ".aiff")


def decode(path, start, dur):
    cmd = ["ffmpeg", "-v", "quiet", "-ss", str(start), "-t", str(dur), "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def bandpass(x, lo, hi):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, len(x))


def envelope(x, hop):
    n = len(x) // hop
    return np.array([np.mean(x[i * hop:(i + 1) * hop] ** 2) for i in range(n)])


def tempo(env, hop, lo=130.0, hi=155.0):
    """Tempo whose 1-, 2- and 4-beat lags give the largest summed autocorrelation."""
    env = env - env.mean()
    best, bestv = None, -1.0
    for bpm10 in range(int(lo * 10), int(hi * 10) + 1):
        bpm = bpm10 / 10
        lag = 60.0 / bpm * SR / hop
        v = 0.0
        for m in (1, 2, 4):
            L = lag * m
            i0 = int(L)
            fr = L - i0
            c = np.dot(env[:-i0 - 1], env[i0:len(env) - 1]) * (1 - fr) + np.dot(env[:-i0 - 1], env[i0 + 1:]) * fr
            v += c / (len(env) - i0 - 1)
        if v > bestv:
            bestv, best = v, bpm
    return best


def profile(band, bpm):
    beat = 60.0 / bpm * SR
    e2 = band ** 2
    nb = int(len(e2) // beat) - 1

    def fold(offset):
        s = np.zeros(16)
        for b in range(nb):
            t0 = offset + b * beat
            for k in range(16):
                a, z = int(t0 + k * beat / 16), int(t0 + (k + 1) * beat / 16)
                s[k] += e2[a:z].mean()
        return s / nb

    candidates = [fold(o) for o in np.linspace(0, beat, 64, endpoint=False)]
    # The kick is where slice 0 stands out most against the three bass slots.
    return max(candidates, key=lambda s: s[0] - 0.25 * (s[4] + s[8] + s[12]))


def main(argv):
    files = []
    args = argv[1:]
    if args and args[0] == "--dir":
        for root, _, names in os.walk(args[1]):
            for n in sorted(names):
                if n.lower().endswith(AUDIO_EXT):
                    files.append(os.path.join(root, n))
    else:
        files = args
    if not files:
        print(__doc__)
        return 2
    for path in files:
        try:
            x = decode(path, 150, 60)
        except subprocess.CalledProcessError:
            print(f"{os.path.basename(path)[:40]:40s} cannot decode")
            continue
        band = bandpass(x, 40, 200)
        hop = 40  # 5 ms
        bpm = tempo(envelope(band, hop), hop)
        prof = profile(band, bpm)
        db = 10 * np.log10(prof / prof.max() + 1e-12)
        n2, n3, n4 = prof[4:8].sum(), prof[8:12].sum(), prof[12:16].sum()
        print(f"{os.path.basename(path)[:40]:40s} {bpm:5.1f} BPM  slices dB:", " ".join(f"{v:5.1f}" for v in db))
        print(f"{'':40s} bass note 2 / 3 / 4 (dB re note 3): {10*math.log10(n2/n3):+5.1f}  0.0  {10*math.log10(n4/n3):+5.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
