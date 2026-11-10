"""Which recordings of the Field track carry a held tone, and at which pitch? (10.11.2026) -> Tools/field_tones.json
and Core/src/FieldToneData.inl

    python Tools/field_tones.py [field folder]          (default: Core/data/field)

The harmony audit of 10.11.2026: Tools/field_select.py prefers steady recordings, and nothing is steadier than a tone.
`fr-rain-roof-0158` is one -- 604.5 Hz, D5 +45 cents, all of its power, 32 dB over the pad in its band in an intro --
and it played under whatever key the track had. Thirteen of the 157 recordings have a fifth or more of their power in
such tones; the odds that a track's place carries one were 6 .. 15 % by style.

Per file: the long-term spectrum on a 10-cent grid (100 Hz .. 8 kHz), a floor (running median over +-300 cents), the
peaks 10 dB or more over it. "tonal" is the share of the power (100 Hz .. 8 kHz) in such peaks, "share" the strongest
peak's own, "held" the share of the frames in which that peak stands 6 dB or more over its surroundings.

What the file says about a recording (only those with a fifth of their power in tones are listed):

  * kind "tone": one tone carries it (share >= 0.3, there all the time, and no second tone of its strength other than
    its octave, fourth or fifth). The composer tunes the recording at playback so that the tone falls on the key's root, fourth or
    fifth, whichever is nearest -- at most 2.5 semitones (ComposerControls.cpp, trackStartControls). "hz" is the tone.
  * kind "music": several tones and none of them the recording's -- the Webb sonifications are pieces in keys of their
    own, an insect chorus is a cluster, a light curve a chord. No single shift puts those into a key; Tools/field_presets.py gives their
    presets no style weight, so the composer does not draw them (they stay in the menu).
"""
import json
import os
import subprocess
import sys

import numpy as np
from scipy.ndimage import median_filter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Tools", "field_tones.json")
INL = os.path.join(ROOT, "Core", "src", "FieldToneData.inl")
SR = 22050
NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
TONAL = 0.2       # listed from this share of the power in tones on
SHARE = 0.3       # one tone carries the recording from this share on
HELD = 0.9        # ... if it is there in this share of the frames


def decode(path):
    raw = subprocess.run(["ffmpeg", "-v", "quiet", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def name_of(hz):
    m = 69 + 12 * np.log2(hz / 440.0)
    near = int(round(m))
    return "%s%d%+d" % (NOTE[near % 12], near // 12 - 1, round(100 * (m - near)))


def analyse(path):
    x = decode(path)
    n = 8192
    if len(x) < 2 * n:
        x = np.pad(x, (0, 2 * n - len(x)))
    hop = n // 2
    frames = min(1 + (len(x) - n) // hop, 4000)
    w = np.hanning(n)
    f = np.fft.rfftfreq(n, 1 / SR)
    grid_c = np.arange(0, 1200 * np.log2(8000 / 100.0), 10.0)       # cents over 100 Hz
    grid_f = 100.0 * 2 ** (grid_c / 1200)
    S = np.empty((frames, len(grid_f)))
    lin = np.zeros(len(f))
    for i in range(frames):
        p = np.abs(np.fft.rfft(x[i * hop: i * hop + n] * w)) ** 2
        lin += p
        S[i] = np.interp(grid_f, f, p)
    ltas = S.mean(axis=0)
    db = 10 * np.log10(ltas + 1e-20)
    floor = median_filter(db, size=61, mode="nearest")
    over = db - floor
    peaks = [k for k in range(1, len(db) - 1) if over[k] >= 10 and db[k] >= db[k - 1] and db[k] >= db[k + 1]]
    total = ltas.sum() + 1e-30
    mask = np.zeros(len(db), bool)
    for k in peaks:
        mask[max(0, k - 2): k + 3] = True
    res = dict(tonal=float(ltas[mask].sum() / total), share=0.0, held=0.0, hz=0.0, rival=False)
    if not peaks:
        return res
    k = max(peaks, key=lambda j: ltas[j])
    # The tone itself, finer than the grid: the strongest bin of the mean spectrum within 30 cents, and a parabola
    # through its neighbours' levels.
    near = np.nonzero(np.abs(1200 * np.log2(np.maximum(f, 1e-9) / grid_f[k])) <= 30)[0]
    b = near[np.argmax(lin[near])]
    la, lb, lc = (10 * np.log10(lin[j] + 1e-30) for j in (b - 1, b, b + 1))
    d = 0.5 * (la - lc) / (la - 2 * lb + lc) if la - 2 * lb + lc != 0 else 0.0
    res["hz"] = float((b + np.clip(d, -0.5, 0.5)) * SR / n)
    res["share"] = float(ltas[max(0, k - 2): k + 3].sum() / total)
    ring = np.r_[S[:, max(0, k - 30): max(0, k - 5)], S[:, k + 6: k + 31]].reshape(frames, -1) if False else \
        np.concatenate([S[:, max(0, k - 30): max(0, k - 5)], S[:, k + 6: k + 31]], axis=1)
    res["held"] = float(np.mean(10 * np.log10(S[:, max(0, k - 2): k + 3].max(axis=1) + 1e-20)
                                >= 10 * np.log10(np.median(ring, axis=1) + 1e-20) + 6))
    # A rival: another peak within 8 dB of the tone that is not its octave, fourth or fifth (40 cents either way) --
    # those three move into the key with it, anything else lands beside it.
    for j in peaks:
        dist = abs(grid_c[j] - grid_c[k]) % 1200
        if j != k and db[j] >= db[k] - 8 and min(abs(dist - t) for t in (0, 500, 700, 1200)) > 40:
            res["rival"] = True
    return res


def write_inl(tones):
    """The table of the core (FieldLibrary.cpp, fieldToneOf): the recording's stem and its tone, -1 for "music"."""
    lines = ["// Generated by Tools/field_tones.py (Tools/field_tones.json) -- do not edit.",
             "// the recording (its file's stem), its held tone in Hz; -1: tones of its own that no shift puts into a key"]
    for stem, t in sorted(tones.items()):
        lines.append('{ "%s", %s },   // %s, %d %% of its power in tones' % (
            stem.replace('"', ''), "%.1ff" % t["hz"] if t["kind"] == "tone" else "-1.0f", t["note"], round(100 * t["tonal"])))
    open(INL, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "Core", "data", "field")
    out = {}
    count = 0
    for dp, _, fs in os.walk(root):
        for fn in sorted(fs):
            if not fn.lower().endswith(".flac"):
                continue
            count += 1
            r = analyse(os.path.join(dp, fn))
            if r["tonal"] < TONAL:
                continue
            tone = r["share"] >= SHARE and r["held"] >= HELD and not r["rival"]
            stem = os.path.splitext(fn)[0]
            out[stem] = dict(kind="tone" if tone else "music", hz=round(r["hz"], 1), note=name_of(r["hz"]),
                             tonal=round(r["tonal"], 3), share=round(r["share"], 3), held=round(r["held"], 2),
                             shot="shots" in os.path.relpath(dp, root).replace(os.sep, "/").split("/"))
            print("%-58s %-5s %8.1f Hz %-7s tonal %.2f share %.2f held %.2f" % (
                stem, out[stem]["kind"], r["hz"], out[stem]["note"], r["tonal"], r["share"], r["held"]))
    json.dump(dict(sorted(out.items())), open(OUT, "w", encoding="utf-8", newline="\n"), indent=1)
    write_inl(out)
    print("%d of %d recordings carry tones -> %s" % (len(out), count, OUT))


if __name__ == "__main__":
    main()
