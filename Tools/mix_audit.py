"""The mix guide's measurements, on Phosphene's renders and on the user's reference recordings (25.09.2026).

The user brought a mix guide ("Psytrance-Mix: Tiefe, Weite, Klarheit") with a list of checks a generator
should pass after every render. Several of its numbers are the guide's own estimates, so none of them is
taken as a bound before it is measured on the recordings the user supplied as the reference: this tool
prints, for every check, the guide's value, the references' value and Phosphene's side by side, and
``--check`` then holds a render to bounds that are written down in ``LIMITS`` below, each with the reason
for the number.

    python Tools/mix_audit.py --render bin/msvc/phos_render.exe --out work/audit
    python Tools/mix_audit.py --refs "C:/Users/Rene/Desktop/Kandidaten/Pop - Kopie"   # cache the references
    python Tools/mix_audit.py --render ... --check                                   # ctest: bounds, exit code

The measures, and how each is taken so that it cannot lie the ways earlier rounds were bitten:

* **Correlation** per 400 ms window, from the windows' cross spectra, below 120 Hz and over the whole band;
  the median over the drop's windows (the guide: >= 0.9 low, >= 0.3 overall).
* **Mono loss**: the K-weighted power of (L+R)/2 on both channels against the stereo power, over the drop
  (the guide: at most 1 dB).
* **Spectral tilt** of the drop: the power spectral density's third-octave means from 100 Hz to 10 kHz, a
  straight line through them over log2 f, and the largest band's distance from the line (the guide:
  -2 .. -4 dB per octave, no band 6 dB off).
* **Low energy of the layers** (renders only, from the stems): each stem's power under 120 Hz against its
  own power, in the drops -- only kick and bass belong there (the guide: at most -30 dB).
* **Kick-bass gap**: in the kick and bass stems summed, the sub band under 90 Hz, 1 ms envelope; per kick
  of a drop, how far the level falls between the kick's peak and the next bass onset (the guide: 12 dB).
  On a recording the next onset is not known, and the gap is read up to the next sixteenth.
* **Drop against break**: the median short-term loudness (3 s, BS.1770) of the drops against the breaks'
  (the guide: at least 6 dB). A recording has no section list, so it is read as the 90th against the 10th
  percentile of its short-term loudness -- and the render is read that way as well, for the comparison.
* **Limiter per kick** (renders only, from --mix-log): the limiter's largest reduction in the 30 ms after
  each kick of a drop (the guide: 3 .. 5 dB, spread under 1.5 dB -- a figure for a loud master).
* **Kick against bass tuning**: the kick's tail frequency against the bass's fundamental, the nearest
  octave (the guide: a beat under 1 Hz).
* **Ablation** (renders only, the Dark-Ambient addon's mute test): each stem left out of the sum of all stems,
  the third-octave spectrum of the drops against the full one, 100 Hz .. 10 kHz, as an rms distance in dB. A
  layer under half a dB carries almost nothing ("zwoelf Layer, von denen vier tragen, sind schlechter als vier");
  reported, not bounded -- whether a quiet layer is dead weight or a colour is the listener's call.

Needs numpy and scipy; the references also need ffmpeg and ffprobe on the PATH.
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfiltfilt, welch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SR = 48000
REF_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ref_mix_audit.json")
#: The configurations a render audit plays: one track of each style (compose.style 0..4), its own seed.
CONFIGS = [(0, 303), (1, 42), (2, 7), (3, 2026), (4, 515)]
STYLE_NAMES = ["Goa", "Full-On", "Progressive", "Dark Forest", "Hi-Tech"]
#: The guide's values, as it states them.
GUIDE = {
    "corr_low": ">= 0.90", "corr_all": ">= 0.30", "mono_loss_db": ">= -1.0", "tilt_db_oct": "-4 .. -2",
    "tilt_dev_db": "<= 6", "low_share_db": "<= -30", "kick_bass_gap_db": ">= 12", "drop_break_db": ">= 6",
    "limiter_mean_db": "3 .. 5", "limiter_sd_db": "< 1.5", "tune_beat_hz": "< 1",
}


# ------------------------------------------------------------------------------------------ signal

def read_wav(path):
    sr, x = wavfile.read(path)
    assert sr == SR, (path, sr)
    scale = 1.0 / 32768.0 if x.dtype == np.int16 else (1.0 / 2147483648.0 if x.dtype == np.int32 else 1.0)
    x = x.astype(np.float64) * scale
    return x.T if x.ndim == 2 else np.vstack([x, x])


def kweight(x):
    from ref_bass import kweight as kw
    return np.vstack([kw(ch) for ch in x])


def segments(bars, kinds):
    """(first sample, end sample) of every run of bars whose section is one of `kinds`."""
    out = []
    for i, (start, sec) in enumerate(bars[:-1]):
        if sec in kinds:
            end = bars[i + 1][0]
            if out and out[-1][1] == start:
                out[-1] = (out[-1][0], end)
            else:
                out.append((start, end))
    return out


def gather(x, segs):
    parts = [x[:, a:b] for a, b in segs if b > a]
    return np.concatenate(parts, axis=1) if parts else np.zeros((2, 0))


def correlation(x, segs=None, win=0.4):
    """Median over 400 ms windows of the channel correlation below 120 Hz and over 20 Hz .. 20 kHz."""
    n = int(win * SR)
    f = np.fft.rfftfreq(n, 1 / SR)
    low, full = (f > 20) & (f < 120), (f > 20) & (f < 20000)
    w = np.hanning(n)
    lo, al = [], []
    spans = segs if segs is not None else [(0, x.shape[1])]
    for a, b in spans:
        for s in range(a, b - n, n):
            L, R = np.fft.rfft(x[0, s:s + n] * w), np.fft.rfft(x[1, s:s + n] * w)
            for sel, out in ((low, lo), (full, al)):
                pl, pr = np.sum(np.abs(L[sel]) ** 2), np.sum(np.abs(R[sel]) ** 2)
                if pl > 1e-20 and pr > 1e-20:
                    out.append(np.real(np.sum(L[sel] * np.conj(R[sel]))) / np.sqrt(pl * pr))
    return (float(np.median(lo)) if lo else float("nan")), (float(np.median(al)) if al else float("nan"))


def mono_loss(x):
    k = kweight(x)
    stereo = np.mean(k[0] ** 2) + np.mean(k[1] ** 2)
    mono = 2.0 * np.mean((0.5 * (k[0] + k[1])) ** 2)
    return 10.0 * np.log10(max(mono, 1e-30) / max(stereo, 1e-30))


def tilt(x):
    f, p = welch(x[0], SR, nperseg=1 << 14)
    _, p2 = welch(x[1], SR, nperseg=1 << 14)
    p = p + p2
    centres = [1000.0 * 2.0 ** (k / 3.0) for k in range(-10, 11)]   # 100 Hz .. 10 kHz
    lv, lf = [], []
    for c in centres:
        sel = (f >= c * 2 ** (-1 / 6)) & (f < c * 2 ** (1 / 6))
        if sel.any():
            lv.append(10.0 * np.log10(np.mean(p[sel]) + 1e-30))
            lf.append(np.log2(c))
    lv, lf = np.array(lv), np.array(lf)
    slope, icpt = np.polyfit(lf, lv, 1)
    return float(slope), float(np.max(np.abs(lv - (slope * lf + icpt))))


def low_share(x):
    f, p = welch(x[0], SR, nperseg=1 << 14)
    _, p2 = welch(x[1], SR, nperseg=1 << 14)
    p = p + p2
    tot = p[f > 20].sum()
    return 10.0 * np.log10(max(p[(f > 20) & (f < 120)].sum(), 1e-30) / max(tot, 1e-30)) if tot > 1e-20 else float("nan")


def short_term(x, win=3.0, hop=0.5):
    """BS.1770 short-term loudness (3 s) every half second: (centre sample, LUFS)."""
    k = kweight(x)
    e = k[0] ** 2 + k[1] ** 2
    c = np.concatenate([[0.0], np.cumsum(e)])
    n, h = int(win * SR), int(hop * SR)
    out = []
    for s in range(0, len(e) - n, h):
        m = (c[s + n] - c[s]) / n
        out.append((s + n // 2, -0.691 + 10.0 * np.log10(max(m, 1e-30))))
    return out


def sub_env_db(mono):
    sos = butter(4, 90.0, "low", fs=SR, output="sos")
    y = sosfiltfilt(sos, mono)
    h = int(0.001 * SR)
    e = np.sqrt(np.convolve(y ** 2, np.ones(h) / h, mode="same"))
    return 20.0 * np.log10(e + 1e-9)


def kick_gaps(env, kicks, nexts):
    """Per kick: the level of its peak (first 40 ms) minus the lowest level up to the next bass onset."""
    out = []
    for k, nx in zip(kicks, nexts):
        a, z = int(k), int(k + 0.04 * SR)
        if nx is None or nx <= z or nx > len(env):
            continue
        pk = int(a + np.argmax(env[a:z]))
        out.append(float(env[pk] - np.min(env[pk:int(nx)])))
    return out


def peak_freq(seg, lo, hi):
    n = 1 << 17
    X = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), n))
    f = np.fft.rfftfreq(n, 1 / SR)
    sel = np.nonzero((f >= lo) & (f <= hi))[0]
    i = sel[np.argmax(X[sel])]
    if 0 < i < len(X) - 1:
        a, b, c = np.log(X[i - 1] + 1e-30), np.log(X[i] + 1e-30), np.log(X[i + 1] + 1e-30)
        d = 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c != 0 else 0.0
        return float(f[i] + d * (f[1] - f[0]))
    return float(f[i])


def tuning_beat(kick_mono, bass_mono, kicks, bass_on):
    """Median beat between the kick's tail and the bass, Hz, over the drop's kicks.

    The kick is read in the 40 ms before the next bass onset, where its sweep has settled (earlier it is still
    falling, and a window there measures the sweep, not the tuning). A kick tuned to the key sits on the bass's
    root or on its fifth (Kick::tuneToKey), so the beat is the smallest difference between the harmonics that
    meet: 1:1 at the octave, 2:3 or 3:2 at the fifth.
    """
    beats = []
    for k in kicks[:64]:
        nb = [b for b in bass_on if k + 0.03 * SR < b < k + 0.5 * SR]
        if not nb:
            continue
        seg = kick_mono[int(nb[0] - 0.04 * SR):int(nb[0])]
        bseg = bass_mono[int(nb[0]):int(nb[0] + 0.06 * SR)]
        if len(seg) < 1000 or len(bseg) < 1000 or np.max(np.abs(seg)) < 1e-5 or np.max(np.abs(bseg)) < 1e-4:
            continue
        fk, fb = peak_freq(seg, 30.0, 130.0), peak_freq(bseg, 30.0, 110.0)
        cand = []
        for m in (-2, -1, 0, 1, 2):
            f = fb * 2.0 ** m
            cand += [abs(fk - f), abs(2.0 * fk - 3.0 * f), abs(3.0 * fk - 2.0 * f)]
        beats.append(min(cand))
    return float(np.median(beats)) if beats else float("nan")


def thirds(x):
    f, p = welch(x[0], SR, nperseg=1 << 14)
    _, p2 = welch(x[1], SR, nperseg=1 << 14)
    p = p + p2
    out = []
    for k in range(-10, 11):
        c = 1000.0 * 2.0 ** (k / 3.0)
        sel = (f >= c * 2 ** (-1 / 6)) & (f < c * 2 ** (1 / 6))
        out.append(10.0 * np.log10(np.mean(p[sel]) + 1e-30))
    return np.array(out)


def ablation(stem, segs):
    """Per stem: the rms third-octave distance (dB, 100 Hz .. 10 kHz) between all stems and all but this one, in the drops."""
    parts = {n: gather(s, segs) for n, s in stem.items()}
    full = sum(parts.values())
    ref = thirds(full)
    out = {}
    for n, x in parts.items():
        if np.max(np.abs(x)) < 1e-6:
            continue
        out[n] = float(np.sqrt(np.mean((ref - thirds(full - x)) ** 2)))
    return out


# ----------------------------------------------------------------------------------------- renders

def read_log(path):
    bars, kicks, bass, gr = [], [], [], []
    for line in open(path, encoding="utf-8"):
        t = line.rstrip("\n").split("\t")
        if t[0] == "bar":
            bars.append((int(float(t[2])), t[4]))
        elif t[0] == "kick":
            kicks.append(int(float(t[1])))
        elif t[0] == "bass":
            bass.append(int(float(t[1])))
        elif t[0] == "gr":
            gr.append((int(t[1]), float(t[2]), float(t[3])))
    return bars, sorted(kicks), sorted(bass), gr


def render(exe, style, seed, out_dir, bars=288):
    tag = f"s{style}_{seed}"
    wav, log, stems = [os.path.join(out_dir, tag + s) for s in (".wav", ".tsv", "_stems")]
    if not os.path.exists(wav) or not os.path.exists(log):
        os.makedirs(out_dir, exist_ok=True)
        subprocess.run([os.path.abspath(exe), "--seed", str(seed), "--bars", str(bars), "--block", "128",
                        "--set", f"compose.style={style}", "--out", wav, "--stems", stems, "--mix-log", log],
                       check=True, stdout=subprocess.DEVNULL)
    return wav, log, stems


def audit_render(wav, log, stems):
    x = read_wav(wav)
    bars, kicks, bass, gr = read_log(log)
    bars.append((x.shape[1], "End"))
    drop = segments(bars, {"Drop"})
    brk = segments(bars, {"Break", "Cut"})
    xd = gather(x, drop)
    r = {}
    r["corr_low"], r["corr_all"] = correlation(x, drop)
    r["mono_loss_db"] = mono_loss(xd)
    r["tilt_db_oct"], r["tilt_dev_db"] = tilt(xd)
    st = short_term(x)
    inside = lambda segs, c: any(a + 1.5 * SR <= c <= b - 1.5 * SR for a, b in segs)
    d = [v for c, v in st if inside(drop, c)]
    b = [v for c, v in st if inside(brk, c)]
    r["drop_break_db"] = float(np.median(d) - np.median(b)) if d and b else float("nan")
    loud = np.array([v for _, v in st if v > -60])
    r["p90_p10_db"] = float(np.percentile(loud, 90) - np.percentile(loud, 10))
    # The stems.
    names = sorted(os.listdir(stems))
    stem = {n.split("_", 1)[1][:-4]: read_wav(os.path.join(stems, n)) for n in names if n.endswith(".wav")}
    r["low_share_db"] = {}
    for name, s in stem.items():
        if name in ("Kick", "Bass"):
            continue
        sd = gather(s, drop)
        if sd.shape[1] and np.max(np.abs(sd)) > 1e-5:
            r["low_share_db"][name] = low_share(sd)
    in_drop = lambda t: any(a <= t < b for a, b in drop)
    dk = [k for k in kicks if in_drop(k)]
    kb = stem["Kick"].mean(axis=0) + stem["Bass"].mean(axis=0)
    env = sub_env_db(kb)
    nexts = []
    for k in dk:
        i = np.searchsorted(bass, k + int(0.005 * SR))
        nexts.append(bass[i] if i < len(bass) else None)
    gaps = kick_gaps(env, dk, nexts)
    r["kick_bass_gap_db"] = float(np.median(gaps)) if gaps else float("nan")
    r["kick_bass_gap_p10_db"] = float(np.percentile(gaps, 10)) if gaps else float("nan")
    beat = 60.0 / 145.0 * SR
    grid = kick_gaps(env, dk, [k + beat / 4 + 0.005 * SR for k in dk])
    r["kick_bass_gap_grid_db"] = float(np.median(grid)) if grid else float("nan")
    g = np.array(gr)
    per = []
    for k in dk:
        sel = (g[:, 0] >= k) & (g[:, 0] < k + 0.03 * SR)
        if sel.any():
            per.append(float(np.max(g[sel, 1])))
    r["limiter_mean_db"] = float(np.mean(per)) if per else float("nan")
    r["limiter_sd_db"] = float(np.std(per)) if per else float("nan")
    r["tune_beat_hz"] = tuning_beat(stem["Kick"].mean(axis=0), stem["Bass"].mean(axis=0), dk, bass)
    r["ablation_db"] = ablation(stem, drop)
    return r


# ------------------------------------------------------------------------------------- references

def audit_reference(path, duration):
    from ref_kick import onsets, tempo
    from metrics import decode
    a = 0.1 * duration
    x = decode(path, a, 0.8 * duration)            # stereo, 48 kHz
    if x.ndim == 1:
        x = np.vstack([x, x])
    st = short_term(x)
    loud = np.array([v for _, v in st if v > -60])
    if len(loud) < 20:
        return None
    r = {"p90_p10_db": float(np.percentile(loud, 90) - np.percentile(loud, 10))}
    # "The drop": the windows in the loudest 40 %, the part of a recording with the full floor.
    thr = np.percentile(loud, 60)
    segs = [(max(0, c - int(1.5 * SR)), c + int(1.5 * SR)) for c, v in st if v >= thr]
    merged = []
    for s in segs:
        if merged and s[0] <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], s[1]))
        else:
            merged.append(s)
    xd = gather(x, merged)
    r["corr_low"], r["corr_all"] = correlation(x, merged)
    r["mono_loss_db"] = mono_loss(xd)
    r["tilt_db_oct"], r["tilt_dev_db"] = tilt(xd)
    mono = xd.mean(axis=0)
    sos = butter(4, [20.0, 300.0], "band", fs=SR, output="sos")
    bpm = tempo(sosfiltfilt(sos, mono))
    ks, beat = onsets(mono, sosfiltfilt(sos, mono), bpm)
    env = sub_env_db(mono)
    grid = kick_gaps(env, ks, [k + beat / 4 + 0.005 * SR for k in ks])
    r["kick_bass_gap_grid_db"] = float(np.median(grid)) if grid else float("nan")
    return r


def build_refs(directory, album):
    from ref_width import collect
    rows = collect(directory, album)
    res = []
    for i, row in enumerate(rows):
        path, dur = row["path"], row["duration"]
        r = audit_reference(path, dur)
        print(f"  [{i + 1:2d}/{len(rows)}] {os.path.basename(path)[:50]:50s} " + ("silent" if r is None else
              " ".join(f"{k} {v:+.2f}" for k, v in r.items())), flush=True)
        if r is not None:
            res.append(r)
    keys = res[0].keys()
    prof = {"files": len(res), "median": {k: float(np.median([r[k] for r in res])) for k in keys},
            "q1": {k: float(np.percentile([r[k] for r in res], 25)) for k in keys},
            "q3": {k: float(np.percentile([r[k] for r in res], 75)) for k in keys}}
    json.dump(prof, open(REF_PATH, "w", encoding="utf-8"), indent=1)
    print(f"written to {REF_PATH}")
    return prof


# ------------------------------------------------------------------------------------------- bounds

#: What --check holds a render to (25.09.2026), each bound with its reason. Measured on the five style renders
#: after the mix guide and its addon: the references' values where they exist, the guide's where they do not.
LIMITS = {
    "corr_low": (0.95, 1.0, "the references' lower quartile is 0.98, the guide asks 0.9: the sub is mono"),
    "corr_all": (0.30, 0.97, "the guide's floor; the ceiling keeps a mix from collapsing to mono (references 0.82 .. 0.93)"),
    "mono_loss_db": (-1.0, 0.0, "the guide: a mono fold-down loses at most 1 dB (references -0.77 .. -0.31)"),
    "tilt_db_oct": (-4.7, -3.3, "the references' quartiles -4.18 .. -3.56, half a dB per octave either way"),
    "tilt_dev_db": (0.0, 6.0, "the guide: no third-octave band 6 dB off the line"),
    "kick_bass_gap_db": (12.0, 60.0, "the guide: the sub falls 12 dB between kick and bass (references 26 .. 32)"),
    "drop_break_db": (3.5, 10.0, "the drop's fall height; the references swing 4.4 dB p90 .. p10, the renders 4 .. 7 here"),
    "p90_p10_db": (2.8, 6.5, "the references' lower quartile 2.79 and a margin over their upper 5.56"),
    "tune_beat_hz": (0.0, 1.0, "the guide: kick and bass beat under 1 Hz"),
    "low_share_db": (-200.0, -30.0, "the guide: nothing but kick and bass under 120 Hz (the effects' sub drop excepted)"),
}
#: Stems a dictionary bound does not apply to, with the reason.
EXEMPT = {("low_share_db", "Sfx"): "the sub drop under the impacts is the effects' own, and ducked under the kick"}


def check(results):
    bad = []
    for name, (lo, hi, why) in LIMITS.items():
        for tag, r in results.items():
            v = r.get(name)
            if isinstance(v, dict):
                for sub, w in v.items():
                    if (name, sub) in EXEMPT or w != w:
                        continue
                    if not (lo <= w <= hi):
                        bad.append(f"{tag} {name}.{sub} = {w:+.2f} outside [{lo}, {hi}] ({why})")
            elif v is None or not (lo <= v <= hi):
                bad.append(f"{tag} {name} = {v} outside [{lo}, {hi}] ({why})")
    return bad


def table(results, refs):
    keys = ["corr_low", "corr_all", "mono_loss_db", "tilt_db_oct", "tilt_dev_db", "kick_bass_gap_db",
            "kick_bass_gap_grid_db", "drop_break_db", "p90_p10_db", "limiter_mean_db", "limiter_sd_db", "tune_beat_hz"]
    head = f"{'measure':24s} {'guide':>10s} {'refs median (q1..q3)':>24s} " + " ".join(f"{t:>12s}" for t in results)
    print(head)
    for k in keys:
        ref = "-"
        if refs and k in refs["median"]:
            ref = f"{refs['median'][k]:+.2f} ({refs['q1'][k]:+.2f}..{refs['q3'][k]:+.2f})"
        print(f"{k:24s} {GUIDE.get(k, '-'):>10s} {ref:>24s} " + " ".join(f"{r.get(k, float('nan')):>+12.2f}" for r in results.values()))
    names = sorted({n for r in results.values() for n in r["low_share_db"]})
    for n in names:
        print(f"{'low_share_db.' + n:24s} {GUIDE['low_share_db']:>10s} {'-':>24s} "
              + " ".join(f"{r['low_share_db'].get(n, float('nan')):>+12.2f}" for r in results.values()))
    names = sorted({n for r in results.values() for n in r["ablation_db"]})
    for n in names:
        print(f"{'ablation_db.' + n:24s} {'> 0.5':>10s} {'-':>24s} "
              + " ".join(f"{r['ablation_db'].get(n, float('nan')):>+12.2f}" for r in results.values()))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--render", help="phos_render executable: render and audit the five style configurations")
    ap.add_argument("--out", default="work/audit", help="where the renders go (reused when present)")
    ap.add_argument("--fresh", action="store_true", help="render again even where a render exists")
    ap.add_argument("--refs", help="measure the reference recordings in this folder and cache the statistics")
    ap.add_argument("--album", default="Psytrance Collection")
    ap.add_argument("--check", action="store_true", help="hold the renders to LIMITS; exit 1 on a violation")
    ap.add_argument("--styles", help="only these compose.style values, comma separated (ctest renders two)")
    ap.add_argument("--clean", action="store_true", help="delete each render and its stems once measured (ctest: GBs of stems)")
    a = ap.parse_args(argv)
    refs = build_refs(a.refs, a.album) if a.refs else (json.load(open(REF_PATH, encoding="utf-8")) if os.path.exists(REF_PATH) else None)
    if not a.render:
        return 0
    results = {}
    wanted = {int(x) for x in a.styles.split(",")} if a.styles else None
    for style, seed in CONFIGS:
        if wanted is not None and style not in wanted:
            continue
        if a.fresh:
            for s in (".wav", ".tsv"):
                p = os.path.join(a.out, f"s{style}_{seed}{s}")
                if os.path.exists(p):
                    os.remove(p)
        wav, log, stems = render(a.render, style, seed, a.out)
        results[STYLE_NAMES[style]] = audit_render(wav, log, stems)
        if a.clean:
            import shutil
            for f in (wav, log):
                os.remove(f)
            shutil.rmtree(stems, ignore_errors=True)
    table(results, refs)
    if a.check:
        bad = check(results)
        for b in bad:
            print("FAIL " + b)
        print("mix audit: " + ("all within bounds" if not bad else f"{len(bad)} outside"))
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
