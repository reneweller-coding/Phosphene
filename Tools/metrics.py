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


#: The three bands above the air band. The first is still inside the reference material's reach, the
#: second is above every encoder's cliff, the third is above the 44.1 kHz Nyquist frequency -- so a
#: spectrum that reads the same in the last two does not end, it runs flat into Nyquist.
TOP_BANDS = [("16-20k", 16000.0, 20000.0), ("20-22k", 20000.0, 22000.0), ("22-24k", 22000.0, 24000.0)]


def top_bands(f, p, total=None):
    """Power of the three top bands as a share of the whole spectrum, in dB, and per hertz.

    Two numbers per band, because the share alone cannot tell a spectrum that ends from one that only
    got narrower: 20 .. 22 kHz is half as wide as 16 .. 20 kHz, so an honestly falling spectrum reads
    3 dB lower there by width alone. The density per hertz divides that out; when the last two
    densities are level, the spectrum is flat into Nyquist.
    """
    tot = float(p.sum()) if total is None else total
    out = {}
    for name, lo, hi in TOP_BANDS:
        s = float(p[(f >= lo) & (f < hi)].sum())
        out[name] = (10.0 * np.log10(max(s, 1e-300) / max(tot, 1e-300)),
                     10.0 * np.log10(max(s / (hi - lo), 1e-300) / max(tot, 1e-300)))
    return out


# ------------------------------------------------------------------- stereo: width and where it comes from

def cross_spectra(x, sr=SR, n=1 << 15):
    """Welch auto- and cross-spectra of a stereo signal: (f, P_L, P_R, Re C_LR).

    Everything the side-to-mid ratio is made of, in one pass. For two channels
    P_M = (P_L + P_R + 2 Re C) / 4 and P_S = (P_L + P_R - 2 Re C) / 4, so a width figure and the
    inter-channel correlation that produced it come out of the same three sums and cannot contradict
    each other (Blauert, "Spatial Hearing", MIT Press 1997, ch. 3, on interaural coherence).
    """
    win = np.hanning(n)
    f = np.fft.rfftfreq(n, 1.0 / sr)
    pl = np.zeros(len(f))
    pr = np.zeros(len(f))
    cc = np.zeros(len(f))
    frames = 0
    for i in range(0, x.shape[1] - n + 1, n // 2):
        a = np.fft.rfft(x[0, i:i + n] * win)
        b = np.fft.rfft(x[1, i:i + n] * win)
        pl += np.abs(a) ** 2
        pr += np.abs(b) ** 2
        cc += (a * np.conj(b)).real
        frames += 1
    if frames == 0:
        pad = np.zeros((2, n))
        pad[:, :x.shape[1]] = x[:, :n]
        a = np.fft.rfft(pad[0] * win)
        b = np.fft.rfft(pad[1] * win)
        pl, pr, cc, frames = np.abs(a) ** 2, np.abs(b) ** 2, (a * np.conj(b)).real, 1
    return f, pl / frames, pr / frames, cc / frames


def band_width_rho(f, pl, pr, cc, lo, hi):
    """(side/mid in dB, zero-lag inter-channel correlation) inside one band.

    The correlation is the quantity the width *is*: for two channels of equal power,
    side/mid = (1 - rho) / (1 + rho) exactly. Reporting both says whether a wide band is wide because
    the two channels carry different material (rho low) or only because one of them is louder.
    """
    sel = (f >= lo) & (f < hi)
    a, b, c = float(pl[sel].sum()), float(pr[sel].sum()), float(cc[sel].sum())
    pm, ps = (a + b + 2.0 * c) / 4.0, (a + b - 2.0 * c) / 4.0
    rho = c / max(np.sqrt(a * b), 1e-300)
    return 10.0 * np.log10(max(ps, 1e-300) / max(pm, 1e-300)), float(rho)


def short_pan_bands(x, bands, sr=SR, n=4096):
    """Short-window panning per band: arrays of (rms level difference in dB, median |rho|).

    The discriminator between the two ways of being wide. A width made of *decorrelated noise* keeps
    the two channels at the same level at every instant and merely removes their correlation: the
    level difference stays near zero and the short-window correlation stays near zero too. A width
    made of *different material in the two channels* -- a hat left, a shaker right, a delay that
    repeats on one side -- swings the level difference from window to window, and inside any one
    window a single dominant source drives the correlation towards +-1. Window 4096 samples = 85 ms,
    long enough for a third-octave band at 100 Hz and short enough that one hit dominates it.

    @param bands list of (lo, hi) in Hz. The short-time transform is taken once for all of them:
                 doing it per band costs the same transform thirty times over.
    """
    win = np.hanning(n)
    f = np.fft.rfftfreq(n, 1.0 / sr)
    sel = [(f >= lo) & (f < hi) for lo, hi in bands]
    frames = range(0, x.shape[1] - n + 1, n // 2)
    pa = np.zeros((len(bands), len(frames)))
    pb = np.zeros_like(pa)
    cr = np.zeros_like(pa)
    for j, i in enumerate(frames):
        a = np.fft.rfft(x[0, i:i + n] * win)
        b = np.fft.rfft(x[1, i:i + n] * win)
        aa, bb, cc = np.abs(a) ** 2, np.abs(b) ** 2, (a * np.conj(b)).real
        for k, s in enumerate(sel):
            pa[k, j] = aa[s].sum()
            pb[k, j] = bb[s].sum()
            cr[k, j] = cc[s].sum()
    ild = np.zeros(len(bands))
    rho = np.zeros(len(bands))
    for k in range(len(bands)):
        live = (pa[k] + pb[k]) > 1e-16            # silence carries no panning information
        if not live.any():
            continue
        d = 10.0 * np.log10(np.maximum(pa[k][live], 1e-30) / np.maximum(pb[k][live], 1e-30))
        ild[k] = float(np.sqrt(np.mean(d ** 2)))
        rho[k] = float(np.median(np.abs(cr[k][live] / np.maximum(np.sqrt(pa[k][live] * pb[k][live]), 1e-300))))
    return ild, rho


def short_pan(x, lo, hi, sr=SR, n=4096):
    """`short_pan_bands` for a single band: (rms level difference in dB, median |rho|)."""
    ild, rho = short_pan_bands(x, [(lo, hi)], sr, n)
    return float(ild[0]), float(rho[0])


def width_profile(x, sr=SR, centres=None, short=False):
    """Width per third octave: side/mid in dB, correlation, and (optionally) the short-window pair.

    One number for the whole spectrum averages a deliberately mono low end with a deliberately wide
    top into nothing, and five bands still hide a tilt. The third-octave curve is the resolution the
    band balance already uses, so width and balance can be read off the same axis.
    """
    c = THIRD_CENTRES if centres is None else centres
    f, pl, pr, cc = cross_spectra(x, sr)
    edges = [(fc / 2.0 ** (1.0 / 6.0), fc * 2.0 ** (1.0 / 6.0)) for fc in c]
    w = np.empty(len(c))
    r = np.empty(len(c))
    for i, (lo, hi) in enumerate(edges):
        w[i], r[i] = band_width_rho(f, pl, pr, cc, lo, hi)
    ild, sr_rho = short_pan_bands(x, edges, sr) if short else (np.zeros(len(c)), np.zeros(len(c)))
    return w, r, ild, sr_rho


def mono_loss(x, sr=SR):
    """What a mono downmix costs each band *relative to the kick-and-bass band*, in dB.

    Mono compatibility is a measurement, not an assumption: a width built out of anti-phase content
    disappears on a club's mono subwoofer feed and on a phone speaker, and this is the number that
    says so. The figure is taken relative to 40 .. 140 Hz because that band is mono by construction
    here and so cannot cancel -- which also makes the measure blind to the panning law itself: a
    hard-panned but otherwise centred-in-phase mix reads 0.00 dB in every band, and only material
    that actually cancels moves it. That is the same convention the mix round's trap table used
    (the references lose 1.2 dB of presence to a mono sum, a Phosphene render 0.3 dB).
    """
    f, p = welch(x, sr)
    _, pm = welch(x.mean(axis=0)[None, :], sr)
    def ratio(lo, hi):
        sel = (f >= lo) & (f < hi)
        return 10.0 * np.log10(max(p[sel].sum(), 1e-300) / max(pm[sel].sum(), 1e-300))
    ref = ratio(BANDS[0][1], BANDS[0][2])
    return {name: ratio(lo, hi) - ref for name, lo, hi in BANDS}


# ------------------------------------------------------------------------------------ true peak

def true_peak_exact(x, block=1 << 13, over=16, screen_db=6.0):
    """Exact band-limited peak of the signal between the samples, in dBTP.

    Not an interpolator with taps but the reconstruction itself: each block's spectrum is padded with
    zeros to `over` times the length and transformed back, which is sinc interpolation with every tap
    (Smith, "Mathematics of the DFT", ch. 8). Only the middle half of each block is trusted, so the
    circular wrap at the block edges never carries into the answer. This is the same computation the
    self test calls `exactTruePeak`; it exists here so that the engine's own meter can be held against
    a measurement that shares no code with it.

    `screen_db` skips the blocks whose own largest sample is more than that far below the file's --
    an eight-minute render is 2800 blocks and all but a handful are nowhere near the peak. It is a
    screen, not an approximation of the arithmetic: what it assumes is that the waveform between the
    samples does not rise `screen_db` above the samples around it, which for a limited programme it
    never does (the measured margin on these renders is under 1 dB). Set it to None to transform
    every block.
    """
    peak = 0.0
    thr = 0.0 if screen_db is None else float(np.max(np.abs(x))) * 10.0 ** (-screen_db / 20.0)
    for ch in range(x.shape[0]):
        y = x[ch]
        for start in range(0, max(len(y) - block + 1, 0), block // 2):
            seg = y[start:start + block]
            if np.max(np.abs(seg[block // 4:block * 3 // 4])) < thr:
                continue
            a = np.fft.rfft(seg)
            b = np.zeros(block * over // 2 + 1, dtype=complex)
            b[:len(a)] = a
            c = np.fft.irfft(b, block * over) * over
            peak = max(peak, float(np.max(np.abs(c[block * over // 4:block * over * 3 // 4]))))
    return 20.0 * np.log10(max(peak, 1e-300))


def band_limited(x, hz, sr=SR):
    """The signal with every bin above `hz` removed, exactly (one FFT over the whole signal).

    The instrument for attributing a true-peak error to a band: if the exact peak of the whole signal
    is 0.9 dB above the exact peak of the same signal cut at the estimator's band edge, then that
    0.9 dB lives above the edge and no estimator of that bandwidth can see it.
    """
    n = x.shape[1]
    f = np.fft.rfftfreq(n, 1.0 / sr)
    out = np.empty_like(x)
    for ch in range(x.shape[0]):
        a = np.fft.rfft(x[ch])
        a[f > hz] = 0.0
        out[ch] = np.fft.irfft(a, n)
    return out


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
    fx, pl, pr, cc = cross_spectra(x)
    wr = [band_width_rho(fx, pl, pr, cc, lo, hi) for _, lo, hi in BANDS]
    print("  stereo width (side/mid dB)       " + "  ".join(f"{k} {w:+6.1f}" for (k, _, _), (w, _) in zip(BANDS, wr)))
    print("  inter-channel correlation        " + "  ".join(f"{k} {r:+6.3f}" for (k, _, _), (_, r) in zip(BANDS, wr)))
    if prof:
        print("  reference median                 "
              + "  ".join(f"{k} {v:+6.1f}" for (k, _, _), v in zip(BANDS, prof["width_median"])))
    ml = mono_loss(x)
    print("  cost of a mono sum (dB, to 40..140 Hz)  "
          + "  ".join(f"{k} {ml[k]:+5.2f}" for k, _, _ in BANDS if k != "low"))
    tb = top_bands(f, p)
    print("  above the air band (share of total / per hertz, dB)  "
          + "  ".join(f"{k} {tb[k][0]:+6.2f}/{tb[k][1]:+7.2f}" for k, _, _ in TOP_BANDS))
    i, lra, tp = loudness(path)
    print(f"  loudness {i:6.2f} LUFS   LRA {lra:4.1f} LU   true peak {tp:6.2f} dBTP (ffmpeg)"
          f"   exact {true_peak_exact(x):6.3f} dBTP"
          f"   cut at 0.45 fs {true_peak_exact(band_limited(x, 0.45 * SR)):6.3f}")
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
    return {"band": bal, "third": third, "width": {k: w for (k, _, _), (w, _) in zip(BANDS, wr)},
            "rho": {k: r for (k, _, _), (_, r) in zip(BANDS, wr)}, "top": tb, "mono": ml,
            "lufs": i, "lra": lra, "exact_tp": true_peak_exact(x),
            "distance": spectral_distance(third, prof) if prof else float("nan")}


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

    # 8. The top bands of white noise: the share of a band is its share of the width, so
    #    16 .. 20 kHz reads 10 log10 (4000 / 24000) and the per-hertz densities are all equal.
    f, p = welch(w)
    tb = top_bands(f, p)
    for name, lo, hi in TOP_BANDS:
        check(f"white noise top band {name}", tb[name][0], 10.0 * np.log10((hi - lo) / 24000.0), 0.35)
    flat = max(abs(tb["20-22k"][1] - tb["22-24k"][1]), abs(tb["16-20k"][1] - tb["20-22k"][1]))
    check("white noise: the three densities per hertz are level", flat, 0.0, 0.35)
    # The same noise cut at 20 kHz: the two bands above the cut must be gone, and the measure has to
    # say so by a margin no real programme reaches. This is the fault the round was called for.
    cut = band_limited(w, 20000.0)
    f, p = welch(cut)
    check_beyond("a spectrum that ends: 22 .. 24 kHz after a cut at 20 kHz", top_bands(f, p)["22-24k"][0], -100.0, False)

    # 9. Width and correlation: build a pair with a correlation that is known in advance. With equal
    #    channel powers the side-to-mid ratio is (1 - rho) / (1 + rho) exactly -- no measurement
    #    involved, it follows from M = (L+R)/2 and S = (L-R)/2.
    for rho in (0.0, 0.5, 0.9, -0.5):
        a, b = w[0], w[1]
        pair = np.vstack([a, rho * a + np.sqrt(1.0 - rho * rho) * b])
        f, pl, pr, cc = cross_spectra(pair)
        got_w, got_r = band_width_rho(f, pl, pr, cc, 200.0, 16000.0)
        check(f"width of a pair with rho = {rho:+.1f}", got_w,
              10.0 * np.log10((1.0 - rho) / (1.0 + rho)), 0.15)
        check(f"correlation of that pair", got_r, rho, 0.02)

    # 10. The two ways of being wide, told apart. Both signals below measure 0.00 dB wide; only the
    #     short-window numbers say which is which.
    ild_d, rho_d = short_pan(np.vstack([w[0], w[1]]), 2000.0, 8000.0)
    blocks = np.zeros((2, n))
    half = SR // 4                                   # a quarter second hard left, then hard right
    for k in range(n // half):
        blocks[k % 2, k * half:(k + 1) * half] = w[0, k * half:(k + 1) * half]
    ild_p, rho_p = short_pan(blocks, 2000.0, 8000.0)
    print(f"  ---- decorrelated noise: ILD rms {ild_d:.2f} dB, |rho| over 85 ms {rho_d:.3f}")
    print(f"  ---- alternately panned material: ILD rms {ild_p:.2f} dB, |rho| over 85 ms {rho_p:.3f}")
    check_beyond("decorrelated noise keeps the channels level", ild_d, 3.0, False)
    check_beyond("panned material does not", ild_p, 20.0, True)

    # 11. Mono compatibility: identical channels lose nothing, anti-phase channels lose everything,
    #     and a hard-panned but in-phase signal loses nothing either -- the measure sees cancellation,
    #     not the panning law.
    same = mono_loss(np.vstack([w[0], w[0]]))
    # A mix that is mono in the low band (as the depth rule demands) and anti-phase above it: the
    # reference band survives the sum and the presence band vanishes, which is what the measure is for.
    lo_part, hi_part = band_limited(w[:1], 140.0)[0], w[1] - band_limited(w[1:], 1500.0)[0]
    anti2 = mono_loss(np.vstack([lo_part + hi_part, lo_part - hi_part]))
    panned = mono_loss(np.vstack([w[0], np.zeros(n)]))
    check("mono downmix of a centred signal costs nothing", same["presence"], 0.0, 0.01)
    check_beyond("mono downmix of an anti-phase signal costs everything", anti2["presence"], 60.0, True)
    check("mono downmix of a hard-panned signal costs nothing", panned["presence"], 0.0, 0.01)

    # 12. The exact true peak against signals whose peak between the samples is known in closed form.
    #     A sine at 0.25 fs sampled at 45 degrees has its largest sample 3.01 dB below its own peak;
    #     a unit impulse is its own peak; a sine just under Nyquist likewise.
    k = np.arange(1 << 14)
    q = np.sin(2 * np.pi * 0.25 * k + np.pi / 4.0)
    check("exact true peak, sine at 0.25 fs sampled at 45 deg", true_peak_exact(np.vstack([q, q])), 0.0, 0.01)
    imp = np.zeros(1 << 14)
    imp[1 << 13] = 1.0
    check("exact true peak of a unit impulse", true_peak_exact(np.vstack([imp, imp])), 0.0, 0.01)
    q2 = np.sin(2 * np.pi * 0.49 * k + 0.3)
    check("exact true peak, sine at 0.49 fs", true_peak_exact(np.vstack([q2, q2])), 0.0, 0.05)

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
    ap.add_argument("--median", action="store_true", help="print the median over the files given (one render is not a calibration quantity)")
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
    rows = []
    for path in args.files:
        rows.append(report(path, args.seconds if args.middle or args.seconds else None,
                           args.third, args.mono, prof, None if args.middle else args.start))
        print()
    if args.median and len(rows) > 1:
        # A single render is one throw of the arrangement lottery: over eight seeds the presence band
        # of this engine swings from -7.5 to -14.3 dB (docs/rounds/2026-09.md, Phase 9). Only the median over
        # several seeds is a calibration quantity, so the tool prints it rather than leaving it to be
        # eyeballed from the rows above.
        def med(f):
            return float(np.median([f(r) for r in rows]))
        print(f"median over {len(rows)} files")
        print("  band balance   " + "  ".join(f"{k} {med(lambda r, k=k: r['band'][k]):+6.2f}" for k, _, _ in BANDS if k != "low"))
        print("  stereo width   " + "  ".join(f"{k} {med(lambda r, k=k: r['width'][k]):+6.2f}" for k, _, _ in BANDS))
        print("  correlation    " + "  ".join(f"{k} {med(lambda r, k=k: r['rho'][k]):+6.3f}" for k, _, _ in BANDS))
        print("  mono sum cost  " + "  ".join(f"{k} {med(lambda r, k=k: r['mono'][k]):+6.2f}" for k, _, _ in BANDS if k != "low"))
        print("  top bands      " + "  ".join(f"{k} {med(lambda r, k=k: r['top'][k][0]):+7.2f}/{med(lambda r, k=k: r['top'][k][1]):+8.2f}"
                                              for k, _, _ in TOP_BANDS))
        print(f"  third-octave distance {med(lambda r: r['distance']):.2f} dB rms"
              f"   loudness {med(lambda r: r['lufs']):.2f} LUFS   LRA {med(lambda r: r['lra']):.1f} LU"
              f"   exact true peak {med(lambda r: r['exact_tp']):+.3f} dBTP"
              f"   worst {max(r['exact_tp'] for r in rows):+.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
