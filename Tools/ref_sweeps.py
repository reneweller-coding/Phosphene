"""Fast downward pitch sweeps ("squelch") in recordings: how high they start, where they end, how long.

The spectrum is whitened by its median over time per bin, so sustained pads and the hat carpet drop
out and short salient events remain. In every 2 ms frame the strongest whitened peak between 300 Hz
and 10 kHz is taken; a sweep is a run of frames whose peak frequency falls monotonically (a little
tolerance) by at least an octave within 250 ms while staying salient. For each sweep: start and end
frequency, duration, and the time constant of an exponential fitted to the frequency excess over the
end frequency.

Run it on tracks known for squelch and on control tracks: if the counts are alike, the detector is
finding something else.

**Result of 15.09.2026: negative.** Without the tonality test the detector found 570 to 1107 sweeps in
90 s in every track, Hallucinogen and controls alike -- the falling spectral centroid of hat and clap
transients. With it (a narrow peak 6 dB above its neighbourhood, continuous in frequency, at least
30 ms), it found none in the three Hallucinogen tracks and four in a control. A squelch sweeps several
kilohertz within a single analysis window and smears into a broadband event itself, so short-time
spectra cannot separate it from transients in a finished mix. Phosphene's squelch parameters are
therefore design values with adjustable ranges, not calibrated ones.

Usage:
    python Tools/ref_sweeps.py file1.mp3 file2.mp3 ...
"""
import os
import subprocess
import sys

import numpy as np

SR = 32000
N, HOP = 256, 64


def decode(path, start, dur):
    cmd = ["ffmpeg", "-v", "quiet", "-ss", str(start), "-t", str(dur), "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"]
    return np.frombuffer(subprocess.run(cmd, capture_output=True, check=True).stdout, dtype=np.float32).astype(np.float64)


def sweeps(x):
    win = np.hanning(N)
    frames = (len(x) - N) // HOP
    f = np.fft.rfftfreq(N, 1.0 / SR)
    band = (f >= 300) & (f <= 10000)
    spec = np.empty((frames, band.sum()))
    for i in range(frames):
        spec[i] = 20 * np.log10(np.abs(np.fft.rfft(x[i * HOP:i * HOP + N] * win))[band] + 1e-9)
    white = spec - np.median(spec, axis=0)
    fb = f[band]
    peak = np.argmax(white, axis=1)
    sal = white[np.arange(frames), peak]
    freq = fb[peak]
    # Tonality: the peak against the mean of the bins 3..8 away on both sides. A resonant sweep is a
    # narrow line; the decay of a hat or a clap is broadband and fails this.
    m = white.shape[1]
    sharp = np.zeros(frames)
    for i in range(frames):
        k = peak[i]
        idx = [q for q in list(range(k - 16, k - 5)) + list(range(k + 6, k + 17)) if 0 <= q < m]
        sharp[i] = white[i, k] - white[i, idx].mean() if idx else 0.0
    found = []
    i = 0
    dt = HOP / SR
    while i < frames - 3:
        if sal[i] < 12.0 or sharp[i] < 6.0:
            i += 1
            continue
        j = i
        while (j + 1 < frames and sal[j + 1] >= 9.0 and sharp[j + 1] >= 4.0 and freq[j + 1] <= freq[j] * 1.02
               and freq[j + 1] >= freq[j] * 0.85 and (j + 1 - i) * dt <= 0.25):
            j += 1
        if (j - i) * dt >= 0.03 and freq[i] >= 2.0 * freq[j]:
            seg = freq[i:j + 1]
            end = seg[-1]
            excess = np.maximum(seg - end, 1.0)
            t = np.arange(len(seg)) * dt
            slope = np.polyfit(t[:-1], np.log(excess[:-1]), 1)[0] if len(seg) > 3 else -1.0
            tau = -1.0 / slope if slope < 0 else float("nan")
            found.append((freq[i], end, (j - i) * dt, tau))
            i = j + 1
        else:
            i += 1
    return found


def main(argv):
    files = argv[1:]
    if not files:
        print(__doc__)
        return 2
    allrows = []
    for path in files:
        rows = []
        for start in (90, 180, 270):
            try:
                rows += sweeps(decode(path, start, 30))
            except subprocess.CalledProcessError:
                pass
        allrows.append((os.path.basename(path), rows))
        if rows:
            a = np.array(rows)
            print(f"{os.path.basename(path)[:44]:44s} {len(rows):4d} sweeps in 90 s   start {np.median(a[:,0]):6.0f} Hz"
                  f"  end {np.median(a[:,1]):5.0f} Hz  duration {1000*np.median(a[:,2]):4.0f} ms  tau {1000*np.nanmedian(a[:,3]):4.0f} ms"
                  f"  (start 25..75 %: {np.percentile(a[:,0],25):.0f}..{np.percentile(a[:,0],75):.0f} Hz)")
        else:
            print(f"{os.path.basename(path)[:44]:44s}    0 sweeps")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
