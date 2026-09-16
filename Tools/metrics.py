"""The listening metrics of docs/PLAN.md 11.4 in one command.

One tool that prints, for a render or a recording, everything a calibration round needs to compare
against the reference material: band balance, crest factor, loudness, loudness range, true peak,
transient density, stereo width and the spectral distance to the reference median. The point is that
the next round is cheap: one command, the same numbers every time, and a cached reference profile so
the 40 recordings have to be decoded only once.

    python Tools/metrics.py out/mix.wav
    python Tools/metrics.py --ref-build --refs refs.json      # cache the reference profile
    python Tools/metrics.py out/mix.wav --third               # the 1/3-octave curve as well
    python Tools/metrics.py --selftest                        # every measure on a known signal

**Why the measures are shaped the way they are.** Every one of them has a way of lying that this
project has already been bitten by, so each carries its guard:

* **Power, never magnitude.** The spectral centroid and all band sums weight by power (Noctuary,
  10.09.2026: a magnitude-weighted centroid called fourteen renders "too bright" where the power
  one saw a factor of two). Welch's method (Welch 1967) with a Hann window and half overlap; the
  window's normalisation cancels in every ratio reported here.
* **Never a mono downmix.** `ffmpeg -ac 1` averages the channels, and anti-correlated material
  cancels: a wide hat layer measures quieter than it is, and by exactly as much as it is wide. All
  bands are summed over the two channels' power (L plus R), which is invariant to the panning law
  and to phase between the channels. `--mono` exists only to show the difference.
* **The reference band is part of the answer.** Band balance is relative to the kick-and-bass band
  40 .. 140 Hz, so a mix whose low band is hot reads low everywhere else. The 1/3-octave curve is
  printed alongside for exactly that reason: a tilt and a notch look the same in five numbers.
* **MP3 references are lossy above 16 kHz.** The air band stops at 16 kHz; `--bandwidth` prints
  where each file's spectrum actually ends, so a band is never read across an encoder's cliff.
* **Loudness from ffmpeg's `ebur128`**, not from a reimplementation: it is the same filter chain the
  engine's own meter follows (ITU-R BS.1770-4, EBU Tech 3341/3342) and an independent second opinion
  on the engine's number.

Needs ffmpeg and ffprobe on the PATH, numpy and scipy.
"""
import argparse
import json
import os
import re
import subprocess
import sys

import numpy as np

SR = 48000
BANDS = [("low", 40.0, 140.0), ("low-mid", 140.0, 500.0), ("mid", 500.0, 1500.0),
         ("presence", 1500.0, 6000.0), ("air", 6000.0, 16000.0)]
#: Third-octave band centres from 25 Hz to 20 kHz (ISO 266 preferred numbers, computed not tabulated).
THIRD_CENTRES = np.array([1000.0 * 2.0 ** (k / 3.0) for k in range(-16, 14)])
PROFILE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ref_profile.json")


# --------------------------------------------------------------------------------------------- IO

def probe_duration(path):
    """Length in seconds from ffprobe, 0.0 if unknown."""
    out = subprocess.run(["ffprobe", "-v", "quiet", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                         capture_output=True, text=True).stdout.strip()
    try:
        return float(out)
    except ValueError:
        return 0.0


def decode(path, start=0.0, dur=None, sr=SR, mono=False):
    """Decodes to float32 at `sr`. Returns (2, N) -- stereo kept unless `mono` is asked for.

    Keeping the channels apart is not a detail: summing them first destroys exactly the information
    the width measure is about, and understates every wide band.
    """
    cmd = ["ffmpeg", "-v", "quiet"]
    if start > 0.0:
        cmd += ["-ss", str(start)]
    if dur is not None:
        cmd += ["-t", str(dur)]
    cmd += ["-i", path, "-ac", "1" if mono else "2", "-ar", str(sr), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    x = np.frombuffer(raw, dtype=np.float32).astype(np.float64)
    if mono:
        return np.vstack([x, x])
    n = len(x) // 2
    return x[:2 * n].reshape(-1, 2).T.copy()


def middle(path, seconds=60.0, sr=SR, mono=False):
    """`seconds` from the middle of the file (the core, not the intro), or all of it if shorter."""
    dur = probe_duration(path)
    start = max(0.0, dur / 2 - seconds / 2) if dur > seconds + 10.0 else 0.0
    return decode(path, start, seconds, sr, mono)


# ---------------------------------------------------------------------------------------- spectra

def welch(x, sr=SR, n=1 << 15):
    """Power spectrum of a (C, N) signal, summed over the channels. Returns (freqs, power)."""
    x = np.atleast_2d(x)
    win = np.hanning(n)
    f = np.fft.rfftfreq(n, 1.0 / sr)
    acc = np.zeros(len(f))
    frames = 0
    for c in range(x.shape[0]):
        for i in range(0, x.shape[1] - n + 1, n // 2):
            acc += np.abs(np.fft.rfft(x[c, i:i + n] * win)) ** 2
            frames += 1
    if frames == 0:                                    # signal shorter than one window: pad once
        pad = np.zeros((x.shape[0], n))
        pad[:, :x.shape[1]] = x[:, :n]
        for c in range(x.shape[0]):
            acc += np.abs(np.fft.rfft(pad[c] * win)) ** 2
            frames += 1
    return f, acc / frames


def band_powers(f, p):
    """Absolute power per named band (no reference yet)."""
    return {name: float(p[(f >= lo) & (f < hi)].sum()) for name, lo, hi in BANDS}


def band_balance(f, p):
    """dB per band relative to the kick-and-bass band, the quantity the plan calibrates against."""
    bp = band_powers(f, p)
    ref = max(bp["low"], 1e-300)
    return {k: 10.0 * np.log10(max(v, 1e-300) / ref) for k, v in bp.items()}


def third_octave(f, p, ref_lo=40.0, ref_hi=140.0):
    """1/3-octave power curve in dB, normalised so the 40 .. 140 Hz band reads 0 dB.

    Five bands cannot tell a tilt from a notch. This curve can, and it is what the reference profile
    stores: the median over the recordings, band by band.
    """
    ref = max(p[(f >= ref_lo) & (f < ref_hi)].sum(), 1e-300)
    out = np.empty(len(THIRD_CENTRES))
    for i, fc in enumerate(THIRD_CENTRES):
        lo, hi = fc / 2.0 ** (1.0 / 6.0), fc * 2.0 ** (1.0 / 6.0)
        out[i] = 10.0 * np.log10(max(p[(f >= lo) & (f < hi)].sum(), 1e-300) / ref)
    return out


def centroid(f, p, lo=40.0, hi=16000.0):
    """Spectral centroid weighted by power (Messfalle 10.09.2026: magnitude overstates brightness)."""
    m = (f >= lo) & (f < hi)
    return float((f[m] * p[m]).sum() / max(p[m].sum(), 1e-300))


def bandwidth(f, p, drop_db=40.0):
    """Highest frequency still within `drop_db` of the peak of the 1 .. 4 kHz plateau.

    Tells an MP3's cliff from a genuinely dark mix: a 128 kBit/s encode stops dead near 16 kHz.
    """
    plateau = p[(f >= 1000.0) & (f < 4000.0)].mean()
    thr = plateau * 10.0 ** (-drop_db / 10.0)
    idx = np.nonzero(p > thr)[0]
    return float(f[idx[-1]]) if len(idx) else 0.0


# ------------------------------------------------------------------------------------- dynamics

def crest_db(x):
    """Peak over RMS of the mid signal, in dB. A sine reads 3.01 dB, white noise about 11 .. 12 dB."""
    m = x.mean(axis=0)
    rms = np.sqrt(np.mean(m ** 2))
    return float(20.0 * np.log10(np.max(np.abs(m)) / max(rms, 1e-300)))


def crest_windows_db(x, sr=SR, win=0.1):
    """Median crest over windows of `win` seconds: transient texture rather than one stray peak."""
    m = x.mean(axis=0)
    n = int(win * sr)
    k = len(m) // n
    if k == 0:
        return crest_db(x)
    w = m[:k * n].reshape(k, n)
    rms = np.sqrt(np.mean(w ** 2, axis=1))
    pk = np.max(np.abs(w), axis=1)
    good = rms > 1e-9
    if not np.any(good):
        return 0.0
    return float(np.median(20.0 * np.log10(pk[good] / rms[good])))


def width(f_unused=None, x=None, sr=SR, band=None):
    """Side-over-mid power in dB, optionally inside one band. Mono reads -inf, a pure side signal 0.

    Reported per band because psytrance is deliberately mono below the kick and wide above it; one
    number over the whole spectrum would average those two decisions into nothing.
    """
    m = 0.5 * (x[0] + x[1])
    s = 0.5 * (x[0] - x[1])
    if band is not None:
        f, pm = welch(m[None, :], sr)
        _, ps = welch(s[None, :], sr)
        lo, hi = band
        sel = (f >= lo) & (f < hi)
        return 10.0 * np.log10(max(ps[sel].sum(), 1e-300) / max(pm[sel].sum(), 1e-300))
    return 10.0 * np.log10(max(np.mean(s ** 2), 1e-300) / max(np.mean(m ** 2), 1e-300))


def transient_density(x, sr=SR, band=(1500.0, 16000.0), n=1024):
    """Onsets per second from the positive spectral flux in one band (Bello et al. 2005).

    Peak picking: a flux peak counts when it is above the median plus 1.5 times the median absolute
    deviation of a 1 s neighbourhood and is the largest within 40 ms -- the same guard the percussion
    profile of Phase 2 uses, so hat layers of a render and of a recording are counted the same way.
    """
    m = x.mean(axis=0)
    hop = n // 4
    win = np.hanning(n)
    f = np.fft.rfftfreq(n, 1.0 / sr)
    sel = (f >= band[0]) & (f < band[1])
    frames = max((len(m) - n) // hop, 0)
    if frames < 4:
        return 0.0
    mags = np.empty((frames, int(sel.sum())))
    for i in range(frames):
        mags[i] = np.abs(np.fft.rfft(m[i * hop:i * hop + n] * win))[sel]
    flux = np.maximum(np.diff(mags, axis=0), 0.0).sum(axis=1)
    w = max(int(1.0 * sr / hop), 5)
    thr = np.empty(len(flux))
    for i in range(len(flux)):
        seg = flux[max(0, i - w // 2):i + w // 2 + 1]
        med = np.median(seg)
        thr[i] = med + 1.5 * np.median(np.abs(seg - med))
    guard = max(int(0.04 * sr / hop), 1)
    onsets = 0
    last = -guard - 1
    for i in range(1, len(flux) - 1):
        if flux[i] > thr[i] and flux[i] >= flux[i - 1] and flux[i] > flux[i + 1] and i - last > guard:
            onsets += 1
            last = i
    return onsets * sr / hop / len(flux)


def loudness(path):
    """Integrated LUFS, LRA and true peak from ffmpeg's ebur128 (BS.1770-4, EBU Tech 3342)."""
    out = subprocess.run(["ffmpeg", "-v", "info", "-i", path, "-af", "ebur128=peak=true", "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    tail = out[-4000:]
    def grab(label):
        m = re.findall(label + r":\s*(-?\d+\.\d+)", tail)
        return float(m[-1]) if m else float("nan")
    return grab("I"), grab("LRA"), grab("Peak")


# ------------------------------------------------------------------------------ reference profile

def build_profile(paths, seconds=60.0, out_path=PROFILE_PATH):
    """Median band balance and 1/3-octave curve over the reference recordings; only statistics are kept."""
    balances, thirds, crests, widths, dens, bws, cents = [], [], [], [], [], [], []
    skipped = []
    for i, path in enumerate(paths):
        x = middle(path, seconds)
        f, p = welch(x)
        # One of the user's forty recordings is silent in the middle. A silent window makes every band
        # ratio 0/0, which reads as a flat +0.0 dB and would pull four medians towards zero -- the kind
        # of quiet bias that is worth one line of guard.
        if np.sqrt(np.mean(x ** 2)) < 1e-5:
            skipped.append(os.path.basename(path))
            print(f"  [{i + 1:2d}/{len(paths)}] {os.path.basename(path)[:52]:52s} silent, skipped")
            continue
        balances.append(band_balance(f, p))
        thirds.append(third_octave(f, p))
        crests.append(crest_windows_db(x))
        widths.append([width(x=x, band=(lo, hi)) for _, lo, hi in BANDS])
        dens.append(transient_density(x))
        bws.append(bandwidth(f, p))
        cents.append(centroid(f, p))
        print(f"  [{i + 1:2d}/{len(paths)}] {os.path.basename(path)[:52]:52s} "
              + " ".join(f"{k} {balances[-1][k]:+6.1f}" for k, _, _ in BANDS if k != "low"))
    prof = {
        "files": len(balances),
        "skipped": skipped,
        "seconds": seconds,
        "band_median": {k: float(np.median([b[k] for b in balances])) for k, _, _ in BANDS},
        "band_q1": {k: float(np.percentile([b[k] for b in balances], 25)) for k, _, _ in BANDS},
        "band_q3": {k: float(np.percentile([b[k] for b in balances], 75)) for k, _, _ in BANDS},
        "third_centres": THIRD_CENTRES.tolist(),
        "third_median": np.median(np.array(thirds), axis=0).tolist(),
        "third_q1": np.percentile(np.array(thirds), 25, axis=0).tolist(),
        "third_q3": np.percentile(np.array(thirds), 75, axis=0).tolist(),
        "crest100_median": float(np.median(crests)),
        "width_median": np.median(np.array(widths), axis=0).tolist(),
        "density_median": float(np.median(dens)),
        "bandwidth_median": float(np.median(bws)),
        "centroid_median": float(np.median(cents)),
    }
    json.dump(prof, open(out_path, "w", encoding="utf-8"), indent=1)
    print(f"\nwritten to {out_path}")
    return prof


def load_profile(path=PROFILE_PATH):
    return json.load(open(path, encoding="utf-8")) if os.path.exists(path) else None


def spectral_distance(third, prof):
    """RMS distance in dB between a 1/3-octave curve and the reference median, 100 Hz .. 16 kHz.

    Both curves are normalised at the kick-and-bass band already, so this is a shape distance: it
    does not move when a mix is simply louder.
    """
    c = np.array(prof["third_centres"])
    med = np.array(prof["third_median"])
    sel = (c >= 100.0) & (c <= 16000.0)
    return float(np.sqrt(np.mean((third[sel] - med[sel]) ** 2)))


# ------------------------------------------------------------------------------------ the report

def report(path, seconds=None, want_third=False, mono=False, prof=None, start=None):
    x = decode(path, start or 0.0, seconds, SR, mono) if (start is not None or seconds is None) \
        else middle(path, seconds, SR, mono)
    f, p = welch(x)
    bal = band_balance(f, p)
    third = third_octave(f, p)
    print(f"{os.path.basename(path)}  {x.shape[1] / SR:.1f} s")
    print("  band balance (dB to 40..140 Hz)  " + "  ".join(f"{k} {bal[k]:+6.2f}" for k, _, _ in BANDS if k != "low"))
    if prof:
        m = prof["band_median"]
        print("  reference median                 " + "  ".join(f"{k} {m[k]:+6.2f}" for k, _, _ in BANDS if k != "low"))
        print("  difference                       " + "  ".join(f"{k} {bal[k] - m[k]:+6.2f}" for k, _, _ in BANDS if k != "low"))
        print(f"  spectral distance to reference median  {spectral_distance(third, prof):.2f} dB rms")
    print(f"  crest {crest_db(x):5.1f} dB   crest/100 ms {crest_windows_db(x):5.1f} dB"
          + (f"   (reference {prof['crest100_median']:.1f})" if prof else ""))
    print(f"  centroid (power) {centroid(f, p):7.0f} Hz   bandwidth {bandwidth(f, p):6.0f} Hz"
          f"   transient density {transient_density(x):5.2f} /s"
          + (f"   (reference {prof['density_median']:.2f})" if prof else ""))
    print("  stereo width (side/mid dB)       "
          + "  ".join(f"{k} {width(x=x, band=(lo, hi)):+6.1f}" for k, lo, hi in BANDS))
    if prof:
        print("  reference median                 "
              + "  ".join(f"{k} {v:+6.1f}" for (k, _, _), v in zip(BANDS, prof["width_median"])))
    i, lra, tp = loudness(path)
    print(f"  loudness {i:6.2f} LUFS   LRA {lra:4.1f} LU   true peak {tp:6.2f} dBTP")
    if want_third:
        print("  1/3 octave (dB to 40..140 Hz)" + ("   vs reference median" if prof else ""))
        med = np.array(prof["third_median"]) if prof else None
        for i2, fc in enumerate(THIRD_CENTRES):
            if fc < 30.0 or fc > 18000.0:
                continue
            line = f"    {fc:8.0f} Hz  {third[i2]:+7.2f}"
            if med is not None:
                line += f"   ref {med[i2]:+7.2f}   diff {third[i2] - med[i2]:+6.2f}"
            print(line)
    return {"band": bal, "third": third}


# ------------------------------------------------------------------------------------- self test

def selftest():
    """Every measure against a signal whose answer is known in advance. Returns the number of failures."""
    rng = np.random.default_rng(7)
    fails = 0

    def check(name, got, want, tol):
        nonlocal fails
        ok = abs(got - want) <= tol
        fails += 0 if ok else 1
        print(f"  {'ok  ' if ok else 'FAIL'} {name:52s} got {got:9.3f}  want {want:9.3f} +- {tol}")

    def check_beyond(name, got, bound, above):
        nonlocal fails
        ok = got > bound if above else got < bound
        fails += 0 if ok else 1
        print(f"  {'ok  ' if ok else 'FAIL'} {name:52s} got {got:9.3f}  want {'>' if above else '<'} {bound}")

    n = SR * 8
    t = np.arange(n) / SR

    # 1. White noise: the power in a band is proportional to its width, so the balance is a ratio of
    #    bandwidths. This is the check that the Welch machinery and the band edges agree with theory.
    w = rng.standard_normal((2, n))
    f, p = welch(w)
    bal = band_balance(f, p)
    for name, lo, hi in BANDS[1:]:
        check(f"white noise band {name}", bal[name], 10.0 * np.log10((hi - lo) / 100.0), 0.35)

    # 2. A sine at 3 kHz: all the power in the presence band, crest 3.01 dB, centroid at 3 kHz.
    s = np.vstack([np.sin(2 * np.pi * 3000 * t)] * 2)
    f, p = welch(s)
    check("sine 3 kHz centroid", centroid(f, p), 3000.0, 2.0)
    check("sine crest", crest_db(s), 20 * np.log10(np.sqrt(2.0)), 0.02)
    # The crest of Gaussian noise is the largest of N draws over their sigma: for N = 2 * 8 * 48000
    # that is about 4.7 sigma, i.e. 13.4 dB -- not the 12 dB of a short burst.
    check("white noise crest", crest_db(w), 13.4, 1.2)

    # 3. Stereo width: identical channels are mono (side vanishes), opposite channels are pure side.
    check_beyond("width of a mono signal", width(x=np.vstack([w[0], w[0]])), -200.0, False)
    check_beyond("width of an anti-phase signal", width(x=np.vstack([w[0], -w[0]])), 200.0, True)
    # A hard-panned source: L only. Side and mid carry the same power, so 0 dB.
    check("width of a hard-panned source", width(x=np.vstack([w[0], np.zeros(n)])), 0.0, 0.01)

    # 4. The mono-downmix trap, stated as a number: an anti-correlated pair vanishes when summed.
    anti = np.vstack([w[0], -w[0]])
    _, p_stereo = welch(anti)
    _, p_mono = welch(anti.mean(axis=0)[None, :])
    drop = 10.0 * np.log10(p_stereo[100:200].sum() / max(p_mono[100:200].sum(), 1e-300))
    print(f"  ---- a mono downmix loses {drop:.0f} dB of an anti-correlated pair (that is the trap)")
    fails += 0 if drop > 60.0 else 1

    # 5. Transient density: a click train at a known rate.
    for rate in (4.0, 12.0):
        clicks = np.zeros(n)
        idx = (np.arange(int(rate * 8)) * SR / rate).astype(int)
        clicks[idx[idx < n]] = 1.0
        clicks = np.convolve(clicks, rng.standard_normal(200) * np.exp(-np.arange(200) / 40.0), "same")
        check(f"transient density of {rate:.0f} clicks/s", transient_density(np.vstack([clicks] * 2)), rate, 0.6)

    # 6. Third-octave curve of white noise: +10 log10 of the band width ratio, a rising line of
    #    1 dB per band (each band is 2^(1/3) wider than the one below it).
    f, p = welch(w)
    third = third_octave(f, p)
    c = THIRD_CENTRES
    sel = (c >= 200.0) & (c <= 8000.0)
    slope = np.polyfit(np.log2(c[sel]), third[sel], 1)[0]
    check("white noise 1/3-octave slope (dB per octave)", slope, 3.01, 0.2)

    # 7. Bandwidth: a signal low-passed at a known frequency.
    X = np.fft.rfft(w[0])
    fr = np.fft.rfftfreq(n, 1.0 / SR)
    X[fr > 9000.0] = 0.0
    lp = np.vstack([np.fft.irfft(X, n)] * 2)
    f, p = welch(lp)
    check("bandwidth of a signal cut at 9 kHz", bandwidth(f, p), 9000.0, 300.0)

    print(f"\n{'all measures agree with theory' if fails == 0 else str(fails) + ' FAILURES'}")
    return fails


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*")
    ap.add_argument("--seconds", type=float, default=None, help="length of the window (default: the whole file)")
    ap.add_argument("--start", type=float, default=None, help="start of the window in seconds")
    ap.add_argument("--middle", action="store_true", help="take the window from the middle of the file")
    ap.add_argument("--third", action="store_true", help="print the 1/3-octave curve")
    ap.add_argument("--mono", action="store_true", help="decode to mono first (to show what that costs)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--ref-build", action="store_true", help="build the cached reference profile")
    ap.add_argument("--refs", default=None, help="JSON list of reference files (path per entry)")
    ap.add_argument("--no-ref", action="store_true", help="do not compare against the cached profile")
    args = ap.parse_args(argv[1:])

    if args.selftest:
        return 1 if selftest() else 0
    if args.ref_build:
        rows = json.load(open(args.refs, encoding="utf-8"))
        paths = [r["path"] if isinstance(r, dict) else r for r in rows]
        build_profile(paths, args.seconds or 60.0)
        return 0
    if not args.files:
        print(__doc__)
        return 2
    prof = None if args.no_ref else load_profile()
    for path in args.files:
        report(path, args.seconds if args.middle or args.seconds else None,
               args.third, args.mono, prof, None if args.middle else args.start)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
