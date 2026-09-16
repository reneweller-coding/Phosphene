"""Even and odd harmonics of the pitched mid-band lines in the reference recordings, and in ours.

**The question this was written for.** A review of 16.09.2026 proposed replacing the point-symmetric
saturation of the acid filter (``Core/include/phos/DiodeLadder.h``: sigma(v) = v / sqrt(1 + v^2)) by
an asymmetric one, ``v + alpha v^2`` with alpha about 0.06, on the argument that the even harmonics
it adds remove a "digital coldness". That is a claim about the *spectrum of the references*, so it
can be measured: do reference acid lines carry more even harmonic content than ours?

**What is measured, and why this measure.** Harmonic energies in a finished mix are dominated by the
filter slope of the instrument and by the mix's own tilt, so "sum of even over sum of odd" mostly
measures brightness. This tool therefore uses a **slope-free evenness**: for a detected fundamental
f0 with harmonic magnitudes a_1..a_6,

    E2 = 20 log10( a_2 / sqrt(a_1 a_3) )        E4 = 20 log10( a_4 / sqrt(a_3 a_5) )

Each compares an even harmonic against the geometric mean of its two odd neighbours, which cancels
any smooth spectral envelope through that region -- a first-order slope correction, exact for an
envelope that is log-linear in harmonic number. The reference values are analytic and are checked in
``--selftest``: an ideal sawtooth (a_n = 1/n) gives E_n = 10 log10(1 - 1/n^2), that is -1.249 dB at
n = 2 and -0.280 dB at n = 4; a square wave (odd only) gives -inf; a signal with equal harmonics
gives 0 dB. The plain odd/even energy ratio the review would ask for is printed beside it, so nothing
is hidden by the choice of measure.

**How a line is isolated in a full mix, and what that can and cannot support.** Nothing here
separates the acid from the lead. What it does is restrict the measurement to frames where the band
above the kick/bass region carries *one* strong, stable, tonal fundamental:

 1. mono, 44.1 kHz, four 45 s windows over the middle 80 % of the track (as ``Tools/ref_width.py``);
 2. an 8192-point Hann STFT, hop 2048; bins below ``--fmin-band`` (250 Hz) are zeroed, so kick and
    bass -- which by the depth rule own everything under 140 Hz -- cannot set the fundamental;
 3. f0 by harmonic summation over harmonics 1..10 on the log spectrum, searched between 130 Hz and
    700 Hz, with the usual sub-octave preference (the lowest candidate within 0.7 dB of the best) and
    an explicit octave guard: a candidate is rejected when f0/2 also carries a peak;
 4. a frame is kept only when the harmonic series stands ``--snr`` dB above the median of the
    surrounding bins (tonal, not a transient) and when its f0 agrees within 3 % with both neighbouring
    frames (a sustained note, not a hat);
 5. E2 and E4 are computed per kept frame; the median over frames of a window, then over windows of a
    track, then over tracks -- the same two-stage median as the width round, because one window is one
    throw of the arrangement.

**What this cannot support.** (a) The kept frames are whatever pitched instrument is loudest in that
band -- acid, lead, arp or a stab -- and in a mix several of them overlap. (b) A finished master has
been through saturation and limiting, both of which add their own even harmonics *after* the
instrument. (c) Most important for the question asked: a sawtooth already carries every integer
harmonic, so an even/odd measurement on a saw-fed filter cannot tell the symmetry of the saturator
from the waveform and the cutoff. A point-symmetric nonlinearity yields only odd harmonics *from a
sine*; from a saw it yields all of them. The measurement can therefore say whether our lines sit in
the same evenness range as the references, and it can rule the proposal out if they already do -- it
cannot, in the other direction, prove that an asymmetric saturator is what produced a reference
number.

Selection is by the album tag, as in ``Tools/ref_style.py`` and ``Tools/ref_width.py``: the
recordings with album tag "Psytrance Collection" are the collection. Only statistics leave this
tool; no audio is copied or stored.

    python Tools/ref_harmonics.py --dir "C:/Users/Rene/Desktop/Kandidaten/Pop - Kopie"
    python Tools/ref_harmonics.py --files render.wav          # the same measurement on our renders
    python Tools/ref_harmonics.py --selftest                  # the analytic checks of the measure

Needs ffmpeg and ffprobe on the PATH and numpy.
"""
import argparse
import json
import math
import os
import subprocess
import sys

import numpy as np

SR = 44100
NFFT, HOP = 8192, 2048
WINDOWS, WINDOW_SECONDS = 4, 45.0
F0_LO, F0_HI = 130.0, 700.0
NHARM_SEARCH = 10


def tags(path):
    """(artist, album, duration) of a file, from ffprobe; tags decoded as UTF-8 explicitly."""
    out = subprocess.run(["ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", path],
                         capture_output=True).stdout.decode("utf-8", "replace")
    try:
        f = json.loads(out).get("format", {})
    except json.JSONDecodeError:
        return "", "", 0.0
    t = {k.lower(): v for k, v in f.get("tags", {}).items()}
    try:
        dur = float(f.get("duration", 0.0))
    except (TypeError, ValueError):
        dur = 0.0
    return t.get("artist", ""), t.get("album", ""), dur


def collect(directory, album):
    """Every file in `directory` whose album tag matches, with artist and duration."""
    rows = []
    for n in sorted(os.listdir(directory)):
        if not n.lower().endswith((".mp3", ".flac", ".wav", ".m4a", ".ogg", ".opus")):
            continue
        p = os.path.join(directory, n)
        artist, alb, dur = tags(p)
        if alb.strip().lower() == album.strip().lower():
            rows.append({"path": p, "artist": artist, "duration": dur})
    return rows


def decode(path, start, dur):
    """Mono float32 at SR from ffmpeg."""
    cmd = ["ffmpeg", "-v", "quiet", "-ss", str(start), "-t", str(dur), "-i", path,
           "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def windows_of(duration):
    """`WINDOWS` windows over the middle 80 % of a track, as (start, seconds)."""
    lo, hi = 0.1 * duration, 0.9 * duration
    span = max(hi - lo, WINDOW_SECONDS)
    if span <= WINDOW_SECONDS * WINDOWS:
        starts = [lo + i * max(0.0, (span - WINDOW_SECONDS) / max(WINDOWS - 1, 1)) for i in range(WINDOWS)]
    else:
        starts = [lo + (i + 0.5) * span / WINDOWS - WINDOW_SECONDS / 2 for i in range(WINDOWS)]
    return [(max(0.0, s), WINDOW_SECONDS) for s in starts]


def spectrogram(x):
    """(freqs, magnitude frames) of an 8192-point Hann STFT with hop HOP."""
    win = np.hanning(NFFT)
    frames = max(0, (len(x) - NFFT) // HOP)
    mag = np.empty((frames, NFFT // 2 + 1))
    for i in range(frames):
        mag[i] = np.abs(np.fft.rfft(x[i * HOP:i * HOP + NFFT] * win))
    return np.fft.rfftfreq(NFFT, 1.0 / SR), mag


def peak_at(mag, freqs, f, halfwidth=2):
    """Magnitude of the partial at `f`: the root of the summed power of the main lobe.

    A Hann window's main lobe is four bins wide and a partial almost never sits on a bin centre, so
    the single largest bin underestimates it by up to 1.4 dB (the scalloping loss) and by an amount
    that depends on the partial's frequency -- which would put a systematic, frequency-dependent
    error straight into a ratio of two partials. Summing the power over the lobe (+-2 bins) removes
    it: the checks in --selftest reproduce the analytic sawtooth value to 0.01 dB with this estimator
    and fail by 0.5 dB with the single-bin one.
    """
    k = int(round(f / (freqs[1] - freqs[0])))
    lo, hi = max(0, k - halfwidth), min(len(mag), k + halfwidth + 1)
    return float(np.sqrt((mag[lo:hi] ** 2).sum())) if hi > lo else 0.0


def detect_f0(mag, freqs, fmin_band):
    """Harmonic-summation f0 in [F0_LO, F0_HI] with a sub-octave preference and an octave guard.

    Returns (f0, salience_dB) or (None, 0). The salience is the harmonic sum of the chosen f0 above
    the median log magnitude of the band, which is the tonality test of step 4.
    """
    df = freqs[1] - freqs[0]
    m = mag.copy()
    m[freqs < fmin_band] = 0.0
    logm = 20.0 * np.log10(m + 1e-12)
    floor = np.median(logm[(freqs >= fmin_band) & (freqs <= 8000.0)])
    cands = np.arange(F0_LO, F0_HI, df * 0.5)
    score = np.empty(len(cands))
    for i, f0 in enumerate(cands):
        s = 0.0
        for n in range(1, NHARM_SEARCH + 1):
            fn = n * f0
            if fn >= 8000.0:
                break
            k = int(round(fn / df))
            s += max(logm[max(0, k - 2):k + 3].max() - floor, 0.0)
        score[i] = s
    if not len(score):
        return None, 0.0
    best = int(np.argmax(score))
    # Sub-octave preference: the lowest candidate whose score is within 0.7 dB-sum of the best and
    # that is (close to) an integer submultiple of it.
    for i in range(best):
        if score[i] >= score[best] - 0.7:
            r = cands[best] / cands[i]
            if abs(r - round(r)) < 0.03 and round(r) >= 2:
                best = i
                break
    f0 = float(cands[best])
    # Octave guard: if half the fundamental carries a partial of its own, the line is an octave lower
    # than detected and every even harmonic would be counted as an odd one -- drop the frame rather
    # than measure it inverted. The test is relative to the detected fundamental, not to the noise
    # floor: in a clean signal the floor is arbitrarily far down and any leakage would trip it.
    if f0 * 0.5 >= fmin_band:
        sub = peak_at(m, freqs, f0 * 0.5)
        top = peak_at(m, freqs, f0)
        if top > 0.0 and 20.0 * np.log10((sub + 1e-15) / top) > -12.0:
            return None, 0.0
    return f0, float(score[best])


def evenness(mag, freqs, f0, nmax=6):
    """(E2, E4, odd/even energy ratio in dB, harmonic magnitudes) for one frame."""
    a = [peak_at(mag, freqs, (n + 1) * f0) for n in range(nmax)]
    a = [max(v, 1e-12) for v in a]
    e2 = 20.0 * math.log10(a[1] / math.sqrt(a[0] * a[2]))
    e4 = 20.0 * math.log10(a[3] / math.sqrt(a[2] * a[4])) if nmax >= 5 else float("nan")
    odd = a[0] ** 2 + a[2] ** 2 + a[4] ** 2
    even = a[1] ** 2 + a[3] ** 2 + (a[5] ** 2 if nmax >= 6 else 0.0)
    ratio = 10.0 * math.log10(even / odd) if odd > 0 else float("nan")
    return e2, e4, ratio, a


def measure_window(x, snr, fmin_band):
    """Median (E2, E4, even/odd dB) over the kept frames of one window, and how many were kept."""
    freqs, mag = spectrogram(x)
    f0s, sals = [], []
    for i in range(len(mag)):
        f0, s = detect_f0(mag[i], freqs, fmin_band)
        f0s.append(f0)
        sals.append(s)
    rows = []
    for i in range(1, len(mag) - 1):
        f0 = f0s[i]
        if f0 is None or sals[i] < snr:
            continue
        # Sustained: the neighbouring frames must agree within 3 %.
        if f0s[i - 1] is None or f0s[i + 1] is None:
            continue
        if abs(f0s[i - 1] / f0 - 1.0) > 0.03 or abs(f0s[i + 1] / f0 - 1.0) > 0.03:
            continue
        e2, e4, ratio, _ = evenness(mag[i], freqs, f0)
        rows.append((e2, e4, ratio, f0))
    if len(rows) < 8:
        return None
    arr = np.array(rows)
    return {"e2": float(np.median(arr[:, 0])), "e4": float(np.median(arr[:, 1])),
            "even_odd_db": float(np.median(arr[:, 2])), "f0": float(np.median(arr[:, 3])),
            "frames": len(rows)}


def measure_file(path, duration, snr, fmin_band):
    """The two-stage median for one file: over frames within a window, then over windows."""
    if duration <= 0.0:
        duration = 300.0
    per = []
    for start, secs in windows_of(duration):
        x = decode(path, start, secs)
        if len(x) < NFFT * 4:
            continue
        r = measure_window(x, snr, fmin_band)
        if r:
            per.append(r)
    if not per:
        return None
    out = {k: float(np.median([p[k] for p in per])) for k in ("e2", "e4", "even_odd_db", "f0")}
    out["windows"] = len(per)
    out["frames"] = int(sum(p["frames"] for p in per))
    return out


def selftest():
    """The analytic checks of the measure: saw -1.25 dB, square -inf, flat 0 dB."""
    ok = True
    t = np.arange(NFFT * 8) / SR
    f0 = 220.0
    # For a_n = 1/n the measure is E_n = 20 log10( sqrt(n^2 - 1) / n ) = 10 log10(1 - 1/n^2):
    # -1.2494 dB at n = 2 and -0.2803 dB at n = 4. It is *not* the same number for both, which is
    # exactly the point -- the measure divides out the envelope, and 1/n is not a log-linear one.
    saw2, saw4 = 10.0 * math.log10(0.75), 10.0 * math.log10(1.0 - 1.0 / 16.0)
    for name, gen, want in (
        ("saw", lambda: sum(np.sin(2 * np.pi * n * f0 * t) / n for n in range(1, 13)), (saw2, saw4)),
        ("square", lambda: sum(np.sin(2 * np.pi * n * f0 * t) / n for n in range(1, 13, 2)), None),
        ("flat", lambda: sum(np.sin(2 * np.pi * n * f0 * t) for n in range(1, 13)), (0.0, 0.0)),
    ):
        x = gen()
        freqs, mag = spectrogram(x)
        e2, e4, ratio, a = evenness(mag[2], freqs, f0)
        if want is None:
            good = e2 < -60.0
            print(f"  {name:7s} E2 = {e2:8.2f} dB  (want very negative)   {'ok' if good else 'FAIL'}")
        else:
            good = abs(e2 - want[0]) < 0.01 and abs(e4 - want[1]) < 0.01
            print(f"  {name:7s} E2 = {e2:8.4f} dB (want {want[0]:.4f}), "
                  f"E4 = {e4:8.4f} dB (want {want[1]:.4f})   {'ok' if good else 'FAIL'}")
        ok = ok and good
    # The detector must find the fundamental of a saw and not its second harmonic.
    x = sum(np.sin(2 * np.pi * n * f0 * t) / n for n in range(1, 13))
    freqs, mag = spectrogram(x)
    f, s = detect_f0(mag[2], freqs, 100.0)
    good = f is not None and abs(f / f0 - 1.0) < 0.02
    print(f"  f0 detector on a 220 Hz saw: {f}   {'ok' if good else 'FAIL'}")
    return ok and good


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", default="")
    ap.add_argument("--album", default="Psytrance Collection")
    ap.add_argument("--files", nargs="*", default=[])
    ap.add_argument("--snr", type=float, default=60.0, help="least harmonic salience of a kept frame")
    ap.add_argument("--fmin-band", type=float, default=250.0)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--json", default="")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)

    rows = []
    if a.dir:
        rows += collect(a.dir, a.album)
        print(f"{len(rows)} recordings tagged '{a.album}'")
    rows += [{"path": p, "artist": "", "duration": 0.0} for p in a.files]
    if a.limit:
        rows = rows[:a.limit]

    results = []
    for r in rows:
        try:
            m = measure_file(r["path"], r["duration"], a.snr, a.fmin_band)
        except (OSError, ValueError):
            m = None
        name = os.path.basename(r["path"])
        if not m:
            print(f"  {name[:56]:56s}  no usable frames")
            continue
        m["file"] = name
        m["artist"] = r["artist"]
        results.append(m)
        print(f"  {name[:56]:56s}  E2 {m['e2']:6.2f}  E4 {m['e4']:6.2f}  "
              f"even/odd {m['even_odd_db']:6.2f} dB  f0 {m['f0']:6.1f} Hz  "
              f"({m['frames']} frames, {m['windows']} windows)")
    if results:
        for k in ("e2", "e4", "even_odd_db"):
            v = sorted(x[k] for x in results)
            n = len(v)
            print(f"{k:12s} median {v[n // 2]:7.2f} dB   quartiles {v[n // 4]:7.2f} / {v[3 * n // 4]:7.2f}   "
                  f"range {v[0]:7.2f} .. {v[-1]:7.2f}   over {n} files")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)
        print(f"written to {a.json}")


if __name__ == "__main__":
    main()
