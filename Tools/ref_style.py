"""Tempo per style and the shape of a break, measured on the reference recordings.

Two questions of Phase 5 (docs/PLAN.md 6.2, 6.3, and the style table of 2.6) are answerable on the
user's own reference material rather than from the literature alone:

1. **What tempo does a style really run at?** The style profiles carry a BPM centre and a range.
   The recordings are tagged with their artist, and the artists divide by style (Goa: Astral
   Projection, Pleiadians, Cosmosis, Hallucinogen, Juno Reactor; Full-On: Astrix, 1200 Mics,
   Infected Mushroom). The tempo comes from the autocorrelation of the onset envelope of the
   kick band, exactly as Tools/ref_slot_profile.py does it, over a window wide enough for both
   styles (128 .. 165 BPM).

2. **How deep is a breakdown, and how does the track come back?** Solberg and Dibben ("Peak
   experiences with EDM", Music Perception 2019) describe a U-shaped amplitude: the breakdown far
   below the core, the buildup rising, the drop at the maximum, and after the drop at least the
   loudness of before the break. The self test of Phase 5 measures exactly that on a render, so the
   numbers to compare against must come from recordings. For every file: short-term loudness
   (RMS over 3 s, 1 s hop, of the whole mix) and the power of the kick-and-bass band 40 .. 140 Hz in
   the same windows. The deepest dip inside the middle 80 % of the track is taken as the breakdown,
   and reported are its depth against the 20 s before it, the depth of the bass band, and the level
   of the 20 s after the following rise ("after the drop") against the 20 s before the break.

Only statistics are printed; nothing of the audio is stored. Selection is by the album tag, as in
Tools/ref_band_balance.py's usage: of the 1336 files in the reference folder only the 40 with the
album tag "Psytrance Collection" belong to the collection.

Usage:
    python Tools/ref_style.py --dir "C:/Users/Rene/Desktop/Kandidaten/Pop - Kopie"
    python Tools/ref_style.py --dir DIR --album "Psytrance Collection" --form
    python Tools/ref_style.py track.mp3 ...

Needs ffmpeg/ffprobe on the PATH and numpy.
"""
import json
import os
import subprocess
import sys

import numpy as np

SR = 8000
AUDIO_EXT = (".mp3", ".flac", ".wav", ".m4a", ".ogg", ".aif", ".aiff")

# Artist to style. The mapping is the user's listening reference, not a claim about the artists:
# the profiles are parameter vectors with descriptive names (PLAN 1, "Nicht-Ziele").
STYLE_OF_ARTIST = {
    "astral projection": "Goa",
    "pleiadians": "Goa",
    "cosmosis": "Goa",
    "hallucinogen": "Goa",
    "juno reactor": "Goa",          # early Juno Reactor is Goa-side: 1993-1997 material
    "man with no name": "Goa",
    "total eclipse": "Goa",
    "the infinity project": "Goa",
    "astrix": "Full-On",
    "1200 mics": "Full-On",
    "1200 micrograms": "Full-On",
    "infected mushroom": "Full-On",
    "moksha": "Full-On",            # the user's "own judgement" call: fast, full-on arrangement
    "gms": "Full-On",
    "skazi": "Full-On",
}


def tags(path):
    """artist, album and duration of a file, from ffprobe."""
    # Tags carry any encoding; ffprobe writes UTF-8, so decode it explicitly rather than in the
    # console code page (cp1252 chokes on the first umlaut).
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


def decode(path, start, dur, sr=SR):
    cmd = ["ffmpeg", "-v", "quiet", "-ss", str(start), "-t", str(dur), "-i", path,
           "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def bandpass(x, lo, hi):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1 / SR)
    X[(f < lo) | (f > hi)] = 0
    return np.fft.irfft(X, len(x))


def tempo(env, hop, lo=128.0, hi=165.0):
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
            c = (np.dot(env[:-i0 - 1], env[i0:len(env) - 1]) * (1 - fr)
                 + np.dot(env[:-i0 - 1], env[i0 + 1:]) * fr)
            v += c / (len(env) - i0 - 1)
        if v > bestv:
            bestv, best = v, bpm
    return best


def track_bpm(path, dur):
    """Tempo from 60 s in the middle of the track (the core, not the intro)."""
    start = max(0.0, dur / 2 - 30) if dur > 90 else 0.0
    x = decode(path, start, 60)
    hop = 40  # 5 ms
    band = bandpass(x, 40, 200)
    env = np.array([np.mean(band[i * hop:(i + 1) * hop] ** 2) for i in range(len(band) // hop)])
    return tempo(env, hop)


def form_shape(path, dur):
    """The U of a break: (breakdown depth, bass-band depth, after-the-drop level), all in dB.

    All three relative to the 20 s of core before the break.
    """
    x = decode(path, 0, dur)
    lowband = bandpass(x, 40, 140)
    win, hop = 3 * SR, 1 * SR
    n = max(1, (len(x) - win) // hop)
    full = np.array([np.mean(x[i * hop:i * hop + win] ** 2) for i in range(n)])
    low = np.array([np.mean(lowband[i * hop:i * hop + win] ** 2) for i in range(n)])
    if n < 90:
        return None
    # The breakdown: the quietest 8 s inside the middle 80 % of the track.
    a, z = int(0.1 * n), int(0.9 * n) - 8
    smooth = np.convolve(full, np.ones(8) / 8, mode="valid")
    dip = a + int(np.argmin(smooth[a:z]))
    before = slice(max(0, dip - 25), max(1, dip - 5))       # 20 s of core before the break
    inside = slice(dip, dip + 8)
    after = slice(min(n - 1, dip + 35), min(n, dip + 55))   # 20 s after the build that follows
    def db(u, v):
        return 10 * np.log10((np.mean(u) + 1e-20) / (np.mean(v) + 1e-20))
    return (db(full[inside], full[before]), db(low[inside], low[before]),
            db(full[after], full[before]), dip)


def main(argv):
    args = argv[1:]
    want_form = "--form" in args
    args = [a for a in args if a != "--form"]
    album = "Psytrance Collection"
    if "--album" in args:
        i = args.index("--album")
        album = args[i + 1]
        del args[i:i + 2]
    files = []
    if args and args[0] == "--dir":
        for root, _, names in os.walk(args[1]):
            files += [os.path.join(root, n) for n in sorted(names) if n.lower().endswith(AUDIO_EXT)]
        picked = []
        for p in files:
            artist, alb, dur = tags(p)
            if alb.strip().lower() == album.strip().lower():
                picked.append((p, artist, dur))
        files = picked
    else:
        files = [(p,) + tags(p)[0::2] for p in args]
    if not files:
        print(__doc__)
        return 2

    rows = []
    for path, artist, dur in files:
        style = STYLE_OF_ARTIST.get(artist.strip().lower(), "?")
        try:
            bpm = track_bpm(path, dur)
            shape = form_shape(path, dur) if want_form else None
        except subprocess.CalledProcessError:
            print(f"{os.path.basename(path)[:40]:40s} cannot decode")
            continue
        rows.append((style, artist, bpm, dur, shape))
        line = (f"{os.path.basename(path)[:38]:38s} {artist[:20]:20s} {style:8s} "
                f"{bpm:5.1f} BPM  {dur/60:4.1f} min")
        if shape is not None:
            line += f"  break {shape[0]:+5.1f} dB, bass {shape[1]:+6.1f} dB, after {shape[2]:+5.1f} dB"
        print(line)

    print()
    for style in ("Goa", "Full-On", "?"):
        b = [r[2] for r in rows if r[0] == style]
        if not b:
            continue
        d = [r[3] / 60 for r in rows if r[0] == style]
        print(f"{style:8s} {len(b):2d} tracks   BPM median {np.median(b):5.1f}  "
              f"({np.min(b):5.1f} .. {np.max(b):5.1f}, quartiles {np.percentile(b, 25):5.1f} / {np.percentile(b, 75):5.1f})"
              f"   length median {np.median(d):4.1f} min")
    if want_form:
        s = [r[4] for r in rows if r[4] is not None]
        if s:
            print(f"break shape, {len(s)} tracks: depth median {np.median([v[0] for v in s]):+5.1f} dB, "
                  f"bass band {np.median([v[1] for v in s]):+6.1f} dB, "
                  f"after the drop {np.median([v[2] for v in s]):+5.1f} dB "
                  f"({sum(1 for v in s if v[2] > -1.0)} of {len(s)} within 1 dB of the core before the break or louder)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
