"""Texture of the hat carpet in the band 6..16 kHz: is it white noise, or something sparser?

Phosphene's percussion lanes drive their filters with white noise. The literature on velvet noise
(Jaervelaeinen and Karjalainen, "Reverberation modeling using velvet noise", AES 30th Int. Conf.,
2007; Vaelimaeki, Lehtonen and Takanen, "A perceptual study on velvet noise and its variants",
IEEE TASLP 21(7), 2013; Alary, Politis and Vaelimaeki, "Velvet-noise decorrelator", Proc. DAFx-17,
Edinburgh 2017) says a sparse sequence of +-1 impulses -- one per equal interval, at a random
position inside it -- is perceptually equal to white noise above a few thousand impulses per second
and cheaper. Whether it is also *closer to real hats* is a question about recordings, so this tool
measures the recordings.

Per file: decode 60 s from 2:30, band-pass 6..16 kHz, and print

    flatness   spectral flatness (geometric over arithmetic mean of the power spectrum) inside the
               band, averaged over 1024-sample Hann frames. White noise is 1; a few strong partials
               (a metal cymbal) push it down.
    crest      20 log10(peak / RMS) of the band-passed signal over the whole excerpt, and the median
               of the same over 100 ms windows (which does not depend on one loud crash).
    kurtosis   the fourth moment over the squared second, minus 3. Gaussian noise is 0; a carpet of
               sparse transients is far above it.
    duty       share of samples whose envelope (1 ms) is within 20 dB of the excerpt's peak.

Usage:
    python Tools/ref_hat_texture.py file1.mp3 file2.mp3 ...
    python Tools/ref_hat_texture.py --list files.txt --root "C:/Music"
    python Tools/ref_hat_texture.py --synth            (white, velvet and a hat-like burst, as a control)

Needs ffmpeg on the PATH and numpy. Nothing of the audio is stored, only the printed statistics.
"""
import os
import subprocess
import sys

import numpy as np

SR = 48000
LO, HI = 6000.0, 16000.0


def decode(path, start, dur, sr=SR):
    cmd = ["ffmpeg", "-v", "quiet", "-ss", str(start), "-t", str(dur), "-i", path,
           "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def bandpass(x, lo=LO, hi=HI, sr=SR):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / sr)
    X[(f < lo) | (f > hi)] = 0.0
    return np.fft.irfft(X, len(x))


def flatness(x, lo=LO, hi=HI, sr=SR, n=1024):
    win = np.hanning(n)
    f = np.fft.rfftfreq(n, 1.0 / sr)
    sel = (f >= lo) & (f <= hi)
    vals = []
    for i in range(0, len(x) - n, n // 2):
        p = np.abs(np.fft.rfft(x[i:i + n] * win)) ** 2
        p = p[sel]
        if p.mean() <= 0.0:
            continue
        vals.append(np.exp(np.log(p + 1e-30).mean()) / p.mean())
    return float(np.median(vals)) if vals else float("nan")


def envelope(x, ms=1.0, sr=SR):
    n = max(1, int(ms * 1e-3 * sr))
    k = np.ones(n) / n
    return np.sqrt(np.convolve(x * x, k, mode="same"))


def stats(x):
    rms = np.sqrt(np.mean(x * x))
    crest = 20.0 * np.log10(np.max(np.abs(x)) / (rms + 1e-30))
    w = int(0.1 * SR)
    frames = [x[i:i + w] for i in range(0, len(x) - w, w)]
    loc = [20.0 * np.log10(np.max(np.abs(a)) / (np.sqrt(np.mean(a * a)) + 1e-30)) for a in frames]
    kurt = np.mean(x ** 4) / (np.mean(x ** 2) ** 2 + 1e-30) - 3.0
    e = envelope(x)
    duty = float(np.mean(e > e.max() * 10 ** (-20.0 / 20.0)))
    return flatness(x), crest, float(np.median(loc)), kurt, duty


def velvet(n, density, rng, sr=SR):
    """One +-1 impulse per interval sr/density, at a random position, scaled to unit variance."""
    x = np.zeros(n)
    td = sr / density
    k = 0
    while True:
        pos = int(k * td + rng.random() * td)
        if pos >= n:
            break
        x[pos] = 1.0 if rng.random() < 0.5 else -1.0
        k += 1
    return x * np.sqrt(sr / density) / np.sqrt(3.0)   # same RMS as the kit's uniform white noise


def synth():
    rng = np.random.default_rng(7)
    n = 60 * SR
    print(f"{'source':44s} {'flat':>6s} {'crest':>6s} {'crest100':>9s} {'kurt':>8s} {'duty':>6s}")
    cases = [("white noise", rng.uniform(-1.0, 1.0, n))]
    for d in (500, 1000, 2000, 4000, 8000):
        cases.append((f"velvet noise {d}/s", velvet(n, d, rng)))
    for name, x in cases:
        f, c, c100, k, duty = stats(bandpass(x))
        print(f"{name:44s} {f:6.3f} {c:6.1f} {c100:9.1f} {k:8.2f} {duty:6.3f}")


def main(argv):
    args = argv[1:]
    if args and args[0] == "--synth":
        synth()
        return 0
    files = []
    root = ""
    if args and args[0] == "--list":
        root = args[3] if len(args) > 3 and args[2] == "--root" else ""
        with open(args[1], "r", encoding="utf-8") as fh:
            files = [os.path.join(root, line.strip()) for line in fh if line.strip()]
    else:
        files = args
    if not files:
        print(__doc__)
        return 2
    print(f"{'file':44s} {'flat':>6s} {'crest':>6s} {'crest100':>9s} {'kurt':>8s} {'duty':>6s}")
    rows = []
    for path in files:
        try:
            x = decode(path, 150, 60)
        except subprocess.CalledProcessError:
            print(f"{os.path.basename(path)[:44]:44s} cannot decode")
            continue
        if len(x) < SR:
            print(f"{os.path.basename(path)[:44]:44s} too short")
            continue
        f, c, c100, k, duty = stats(bandpass(x))
        rows.append((f, c, c100, k, duty))
        print(f"{os.path.basename(path)[:44]:44s} {f:6.3f} {c:6.1f} {c100:9.1f} {k:8.2f} {duty:6.3f}")
    if rows:
        a = np.array(rows)
        med = np.median(a, axis=0)
        q1 = np.percentile(a, 25, axis=0)
        q3 = np.percentile(a, 75, axis=0)
        print(f"{'MEDIAN of ' + str(len(rows)):44s} {med[0]:6.3f} {med[1]:6.1f} {med[2]:9.1f} {med[3]:8.2f} {med[4]:6.3f}")
        print(f"{'quartiles':44s} {q1[0]:.3f}-{q3[0]:.3f} {q1[1]:.1f}-{q3[1]:.1f} {q1[2]:.1f}-{q3[2]:.1f} "
              f"{q1[3]:.2f}-{q3[3]:.2f} {q1[4]:.3f}-{q3[4]:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
