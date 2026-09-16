"""Stereo width of the reference recordings, per third octave, with its distribution.

The listening report of Phase 9 said Phosphene mixes "far narrower than the references in every
band" and left it at that. This tool turns that sentence into a distribution: for every recording of
the collection, and for several windows inside every recording, the side-to-mid power ratio per
third octave, the zero-lag inter-channel correlation that produced it, and the two short-window
numbers that say *how* the width was made (``Tools/metrics.py``: ``short_pan``).

Why several windows per track. The mix round measured that a single eight-minute render swings by
5 dB between windows where a reference recording swings 0.6 dB -- a render is one throw of the
arrangement lottery, a finished track is not. A width target taken from one window of one recording
would therefore be noise. Every number printed here is a median over windows within a track first
and over tracks second, and the spread of both is printed beside it.

Selection is by the album tag, as in ``Tools/ref_style.py``: in the user's library the recordings
with album tag "Psytrance Collection" are the collection. Only statistics leave this tool; no audio
is copied or stored.

    python Tools/ref_width.py --dir DIR                       # the distribution, five bands
    python Tools/ref_width.py --dir DIR --third               # the third-octave curve as well
    python Tools/ref_width.py --dir DIR --out refs_width.json # cache it for the next round
    python Tools/ref_width.py --files a.wav b.wav             # the same measurement on renders

Needs ffmpeg and ffprobe on the PATH and numpy.
"""
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics as M                                                      # noqa: E402

#: Windows per recording and their length in seconds. Four windows of 45 s cover three minutes of a
#: track, which is more than the mix round needed to bring a render's 5 dB swing down to under 1 dB.
WINDOWS, WINDOW_SECONDS = 4, 45.0


def tags(path):
    """(artist, album, duration) of a file, from ffprobe. Tags are decoded as UTF-8 explicitly."""
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
    """Every file in `directory` whose album tag matches, with its duration."""
    rows = []
    names = sorted(os.listdir(directory))
    for n in names:
        if not n.lower().endswith((".mp3", ".flac", ".wav", ".m4a", ".ogg", ".opus")):
            continue
        p = os.path.join(directory, n)
        artist, alb, dur = tags(p)
        if alb.strip().lower() == album.strip().lower():
            rows.append({"path": p, "artist": artist, "duration": dur})
    return rows


def windows_of(path, duration):
    """`WINDOWS` evenly spaced windows over the middle 80 % of a file, as (start, seconds) pairs.

    The first and last tenth are left out on purpose: an intro or an outro is not the mix, and a
    fade-out measures the fade.
    """
    if duration <= 0.0:
        duration = M.probe_duration(path)
    lo, hi = 0.1 * duration, 0.9 * duration
    span = max(hi - lo, WINDOW_SECONDS)
    if span <= WINDOW_SECONDS * WINDOWS:
        starts = [lo + i * max(0.0, (span - WINDOW_SECONDS) / max(WINDOWS - 1, 1)) for i in range(WINDOWS)]
    else:
        starts = [lo + (i + 0.5) * span / WINDOWS - WINDOW_SECONDS / 2 for i in range(WINDOWS)]
    return [(max(0.0, s), WINDOW_SECONDS) for s in starts]


def five_band(x, short=False):
    """The same four measures over the five named bands of `metrics.py`, for one signal."""
    f, pl, pr, cc = M.cross_spectra(x)
    edges = [(lo, hi) for _, lo, hi in M.BANDS]
    ild, sh = M.short_pan_bands(x, edges) if short else (np.zeros(len(edges)), np.zeros(len(edges)))
    out = {}
    for i, (name, lo, hi) in enumerate(M.BANDS):
        w, r = M.band_width_rho(f, pl, pr, cc, lo, hi)
        out[name] = (w, r, float(ild[i]), float(sh[i]))
    return out


def measure_file(path, duration=0.0, short=False, centres=None):
    """Every window of one file, measured once and used for both resolutions.

    Returns (third, five) where `third` holds the (windows, bands) arrays for width, rho, ILD and
    short |rho| and `five` is the list of five-band dictionaries, one per window. Both come out of the
    same decode: the five-band headline numbers must not rest on a different window than the curve
    that explains them.
    """
    c = M.THIRD_CENTRES if centres is None else centres
    ws, rs, ils, srs, fives = [], [], [], [], []
    for start, secs in windows_of(path, duration):
        x = M.decode(path, start, secs)
        if x.shape[1] < 1 << 16 or np.sqrt(np.mean(x ** 2)) < 1e-5:
            continue
        w, r, ild, sh = M.width_profile(x, centres=c, short=short)
        ws.append(w)
        rs.append(r)
        ils.append(ild)
        srs.append(sh)
        fives.append(five_band(x, short))
    if not ws:
        return None
    return (np.array(ws), np.array(rs), np.array(ils), np.array(srs)), fives


def run(rows, want_third, short, out_path):
    """Measures every file, prints the distribution and optionally writes the profile."""
    per_track_band, per_track_third, swings, five_swings = [], [], [], []
    for i, r in enumerate(rows):
        res = measure_file(r["path"], r.get("duration", 0.0), short)
        if res is None:
            print(f"  [{i + 1:2d}/{len(rows)}] {os.path.basename(r['path'])[:52]:52s} unusable, skipped")
            continue
        (w, rho, ild, sh), fives = res
        per_track_third.append([np.median(w, axis=0), np.median(rho, axis=0),
                                np.median(ild, axis=0), np.median(sh, axis=0)])
        # The window swing of this track: the largest spread any one band shows between its windows.
        sel = (M.THIRD_CENTRES >= 140.0) & (M.THIRD_CENTRES <= 16000.0)
        swings.append(float(np.max(np.ptp(w[:, sel], axis=0))))
        # Five-band figures are taken on the band itself, not as a median of third octaves, and are
        # the median over the same windows; their own window swing is printed beside the curve's.
        med = {}
        for k, _, _ in M.BANDS:
            med[k] = tuple(float(np.median([f[k][j] for f in fives])) for j in range(4))
        five_swings.append(max(float(np.ptp([f[k][0] for f in fives])) for k, _, _ in M.BANDS if k != "low"))
        per_track_band.append(med)
        print(f"  [{i + 1:2d}/{len(rows)}] {os.path.basename(r['path'])[:44]:44s} "
              + "  ".join(f"{k} {med[k][0]:+6.1f}" for k, _, _ in M.BANDS if k != "low")
              + f"   swing {five_swings[-1]:4.1f} / {swings[-1]:4.1f} dB")

    if not per_track_band:
        print("nothing measured")
        return 1
    print(f"\n{len(per_track_band)} recordings, {WINDOWS} windows of {WINDOW_SECONDS:.0f} s each")
    print(f"within-track window swing: five bands median {np.median(five_swings):.2f} dB, worst "
          f"{np.max(five_swings):.2f};  worst third octave 140 Hz .. 16 kHz median {np.median(swings):.2f} dB, "
          f"worst {np.max(swings):.2f} dB")
    print("\nwidth = side/mid power (dB).  rho = zero-lag inter-channel correlation.")
    print(f"{'band':10s} {'median':>8s} {'q1':>7s} {'q3':>7s} {'min':>7s} {'max':>7s} {'rho':>7s}"
          + (f" {'ILD rms':>8s} {'|rho| 85ms':>10s}" if short else ""))
    band_stats = {}
    for name, _, _ in M.BANDS:
        v = np.array([t[name][0] for t in per_track_band])
        rr = np.array([t[name][1] for t in per_track_band])
        line = (f"{name:10s} {np.median(v):+8.2f} {np.percentile(v, 25):+7.2f} {np.percentile(v, 75):+7.2f} "
                f"{np.min(v):+7.2f} {np.max(v):+7.2f} {np.median(rr):+7.3f}")
        band_stats[name] = {"median": float(np.median(v)), "q1": float(np.percentile(v, 25)),
                            "q3": float(np.percentile(v, 75)), "min": float(np.min(v)),
                            "max": float(np.max(v)), "rho": float(np.median(rr))}
        if short:
            il = np.array([t[name][2] for t in per_track_band])
            sh = np.array([t[name][3] for t in per_track_band])
            line += f" {np.median(il):8.2f} {np.median(sh):10.3f}"
            band_stats[name]["ild_rms"] = float(np.median(il))
            band_stats[name]["rho_short"] = float(np.median(sh))
        print(line)

    third = np.array([t[0] for t in per_track_third])
    third_rho = np.array([t[1] for t in per_track_third])
    if want_third:
        print("\n1/3 octave width (dB), median over recordings")
        for i, fc in enumerate(M.THIRD_CENTRES):
            if fc < 40.0 or fc > 20000.0:
                continue
            print(f"    {fc:8.0f} Hz  {np.median(third[:, i]):+7.2f}   q1 {np.percentile(third[:, i], 25):+7.2f}"
                  f"   q3 {np.percentile(third[:, i], 75):+7.2f}   rho {np.median(third_rho[:, i]):+6.3f}")
    if out_path:
        prof = {
            "files": len(per_track_band), "windows": WINDOWS, "window_seconds": WINDOW_SECONDS,
            "band": band_stats,
            "third_centres": M.THIRD_CENTRES.tolist(),
            "third_median": np.median(third, axis=0).tolist(),
            "third_q1": np.percentile(third, 25, axis=0).tolist(),
            "third_q3": np.percentile(third, 75, axis=0).tolist(),
            "third_rho_median": np.median(third_rho, axis=0).tolist(),
            "window_swing_median": float(np.median(swings)), "window_swing_max": float(np.max(swings)),
            "five_swing_median": float(np.median(five_swings)), "five_swing_max": float(np.max(five_swings)),
        }
        if short:
            prof["third_ild_median"] = np.median(np.array([t[2] for t in per_track_third]), axis=0).tolist()
            prof["third_rho_short_median"] = np.median(np.array([t[3] for t in per_track_third]), axis=0).tolist()
        json.dump(prof, open(out_path, "w", encoding="utf-8"), indent=1)
        print(f"\nwritten to {out_path}")
    return 0


def main(argv):
    args = argv[1:]
    def opt(name, default=None):
        return args[args.index(name) + 1] if name in args else default
    directory = opt("--dir")
    files = []
    if "--files" in args:
        files = [a for a in args[args.index("--files") + 1:] if not a.startswith("--")]
    if not directory and not files:
        print(__doc__)
        return 2
    rows = [{"path": p, "duration": 0.0} for p in files]
    if directory:
        rows += collect(directory, opt("--album", "Psytrance Collection"))
        print(f"{len(rows)} recordings tagged with the album")
    return run(rows, "--third" in args, "--short" in args, opt("--out"))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
