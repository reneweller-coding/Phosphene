"""Tuning audit of the factory presets (10.11.2026): where the energy of a sound lies against the pitch of its note.

    python Tools/tuning_audit.py [--exe build/msvc/Tools/tunebench/Release/phos_tunebench.exe] [--work work/tuning]
                                 [--synths bass,acid,lead,counter,arp,stab,pad,drone] [--keep] [--strict]

Runs phos_tunebench for every synth asked for (every preset: a note of the part's usual length, and one held for 2.9 s),
measures both and writes <work>/<synth>.res.tsv, one row per preset; prints a table per group and the presets over the
limits. With --strict the exit code is 1 when a preset is over a limit (LIMITS).

Long note: the spectrum of the sounding part -> centroid and spread of the energy around the fundamental (cents), the
shares further than 15/25/50/100 cents, and the share of the energy between 0.45 f0 and 6.5 f0 that no harmonic of the
note or of its second oscillator explains (+-70 cents: a supersaw at 0.85 spreads +-61, and how wide a unison is
is the question of the shares above, not of this one). Short note: the instantaneous frequency of the fundamental
band, weighted by energy -> the pitch a short note is really heard at.

Traps the audit ran into: the spread around the fundamental takes a wobbling filter or a gate (amplitude modulation,
sidebands) for mistuning -- only the instantaneous frequency (if_*) tells them apart; and a bass note of 70 ms at 52 Hz
is too short for either measure (the bass is judged on its long note).
"""
import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np

SR = 48000
SHORT_N, LONG_N = 36000, 144000
PITCH = {"bass": 32, "acid": 57, "lead": 69, "counter": 82, "arp": 73, "stab": 63, "pad": 61, "drone": 54}
OSC2_SEMIS = [-24, -12, -7, 0, 7, 12]
OSC2_DEFAULT_INTERVAL = 1


def hz(m):
    return 440.0 * 2 ** ((m - 69) / 12)


def parse(text, synth):
    d = {}
    for kv in text.split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            k = k.split(".", 1)[1] if "." in k else k
            try:
                d[k] = float(v)
            except ValueError:
                d[k] = v
    return d


def long_stats(x, f0, par):
    """x: (n, 2) float. Returns dict of the spectral measures."""
    p10 = (x ** 2).sum(axis=1)
    hop = 480
    env = p10[: len(p10) // hop * hop].reshape(-1, hop).mean(axis=1)
    peak = env.max() + 1e-30
    a, b = int(1.0 * SR), int(2.9 * SR)
    steady = env[a // hop: b // hop].mean() >= 1e-3 * peak
    if not steady:
        # a pluck: from 30 ms until 30 dB under the peak, at least 200 ms
        above = np.nonzero(env >= 1e-3 * peak)[0]
        a = int(0.03 * SR)
        b = max(a + int(0.2 * SR), min(LONG_N, (above[-1] + 1) * hop))
    seg = x[a:b]
    n = len(seg)
    w = np.hanning(n)[:, None]
    nfft = 1 << 19
    spec = np.abs(np.fft.rfft(seg * w, nfft, axis=0)) ** 2
    P = spec.sum(axis=1)
    f = np.fft.rfftfreq(nfft, 1 / SR)
    out = {"steady": int(steady), "win_s": n / SR}
    # the fundamental band, +-600 cents
    lo, hi = f0 * 2 ** (-0.5), f0 * 2 ** 0.5
    m = (f >= lo) & (f <= hi)
    c = 1200 * np.log2(f[m] / f0)
    pw = P[m]
    tot = pw.sum() + 1e-30
    out["c_mean"] = float((pw * c).sum() / tot)
    out["c_rms"] = float(np.sqrt((pw * c * c).sum() / tot))
    for t in (15, 25, 50, 100):
        out["out%d" % t] = float(pw[np.abs(c) > t].sum() / tot)
    # where the strongest partial of the band sits
    out["c_peak"] = float(c[np.argmax(pw)])
    # the fundamental band's instantaneous frequency over the same stretch (5 ms means): amplitude modulation leaves
    # it where it is, detune and pitch modulation move it
    mid = seg.mean(axis=1) * w[:, 0]
    nf = 1 << 18
    X = np.fft.fft(mid, nf)
    ff = np.fft.fftfreq(nf, 1 / SR)
    lo5, hi5 = f0 * 2 ** (-5 / 12), f0 * 2 ** (5 / 12)
    X[~((ff >= lo5) & (ff <= hi5))] = 0
    z = np.fft.ifft(X)[:n]
    ww = np.abs(z[1:]) ** 2
    inst = np.angle(z[1:] * np.conj(z[:-1])) * SR / (2 * np.pi)
    ker = np.ones(240)
    ws = np.convolve(ww, ker, "same") + 1e-30
    fs = np.clip(np.convolve(ww * inst, ker, "same") / ws, lo5, hi5)
    ci = 1200 * np.log2(fs / f0)
    wt = ww.sum() + 1e-30
    out["if_mean"] = float((ww * ci).sum() / wt)
    out["if_rms"] = float(np.sqrt((ww * ci * ci).sum() / wt))
    out["if_out25"] = float(ww[np.abs(ci) > 25].sum() / wt)
    out["if_out50"] = float(ww[np.abs(ci) > 50].sum() / wt)
    # explained by harmonics of the note / of the second oscillator, 0.45 f0 .. 6.5 f0
    m2 = (f >= 0.45 * f0) & (f <= 6.5 * f0)
    f2, p2 = f[m2], P[m2]
    targets = [h * f0 for h in range(1, 7)]
    own = np.zeros(len(f2), bool)
    for t in targets:
        own |= np.abs(1200 * np.log2(f2 / t)) <= 70
    partner = np.zeros(len(f2), bool)
    osc2 = int(par.get("osc2", 0)) != 0 and par.get("osc2_mix", 0) > 0
    if osc2:
        iv = OSC2_SEMIS[int(par.get("osc2_interval", OSC2_DEFAULT_INTERVAL))] + par.get("osc2_detune", 0) / 100.0
        fp = f0 * 2 ** (iv / 12)
        h = 1
        while h * fp <= 6.6 * f0:
            if h * fp >= 0.4 * f0:
                partner |= np.abs(1200 * np.log2(f2 / (h * fp))) <= 70
            h += 1
    all2 = p2.sum() + 1e-30
    out["own"] = float(p2[own].sum() / all2)
    out["partner"] = float(p2[partner & ~own].sum() / all2)
    out["unexplained"] = float(p2[~own & ~partner].sum() / all2)
    # the fundamental against everything (a missing fundamental moves the heard octave)
    out["fund_share"] = float(tot / (P[(f >= 20) & (f <= 16000)].sum() + 1e-30))
    # where the unexplained energy peaks (in multiples of f0)
    rest = np.where(~own & ~partner, p2, 0.0)
    out["rest_at"] = float(f2[np.argmax(rest)] / f0) if rest.max() > 0 else 0.0
    return out


def short_stats(x, f0):
    """x: (n, 2). The fundamental band's instantaneous frequency, energy-weighted."""
    mid = x.mean(axis=1)
    nfft = 1 << 17
    X = np.fft.fft(mid, nfft)
    f = np.fft.fftfreq(nfft, 1 / SR)
    lo, hi = f0 * 2 ** (-5 / 12), f0 * 2 ** (5 / 12)
    X[~((f >= lo) & (f <= hi))] = 0
    z = np.fft.ifft(X)[: len(mid)]
    w = np.abs(z[1:]) ** 2
    inst = np.angle(z[1:] * np.conj(z[:-1])) * SR / (2 * np.pi)
    k = 240   # 5 ms
    ker = np.ones(k)
    ws = np.convolve(w, ker, "same") + 1e-30
    fs = np.convolve(w * inst, ker, "same") / ws
    fs = np.clip(fs, lo, hi)
    c = 1200 * np.log2(fs / f0)
    tot = w.sum() + 1e-30
    out = {"s_mean": float((w * c).sum() / tot)}
    for t in (25, 50, 100):
        out["s_out%d" % t] = float(w[np.abs(c) > t].sum() / tot)
    # the first 20 ms that carry energy, and the time after which it stays within 20 cents
    lead = w[: int(0.02 * SR)]
    out["s_onset"] = float((lead * c[: len(lead)]).sum() / (lead.sum() + 1e-30))
    loud = w > 1e-3 * w.max()
    bad = np.nonzero(loud & (np.abs(c) > 20))[0]
    out["s_settle_ms"] = float(1000.0 * (bad[-1] + 1) / SR) if len(bad) else 0.0
    out["s_dur_ms"] = float(1000.0 * (np.nonzero(loud)[0][-1] + 1) / SR) if loud.any() else 0.0
    return out


SYNTHS = ["bass", "acid", "lead", "counter", "arp", "stab", "pad", "drone"]
# What a preset may do before it is listed: the short note's heard pitch (cents beside the note), the share of the
# low partials beside the harmonics of the note and of its second oscillator, the share of the fundamental's energy
# further than a semitone from it, and the centre of the long note (cents).
LIMITS = dict(s_mean=35.0, unexplained=0.10, out100=0.10, if_mean=12.0)
KEYS = ["silent", "steady", "win_s", "c_mean", "c_rms", "c_peak", "out15", "out25", "out50", "out100", "own", "partner",
        "unexplained", "rest_at", "fund_share", "if_mean", "if_rms", "if_out25", "if_out50", "s_mean", "s_out25",
        "s_out50", "s_out100", "s_onset", "s_settle_ms", "s_dur_ms"]
PARAMS = ["osc", "detune", "mix", "osc2", "osc2_mix", "osc2_interval", "osc2_detune", "fm_ratio", "fm_index", "drift",
          "lfo_pitch", "resonance", "cutoff", "key_track", "filter_model", "mod", "glide"]


def write_rows(base, rows):
    """Writes <base>.res.tsv, a row per preset."""
    cols = ["synth", "index", "group", "name"] + KEYS + ["p_" + k for k in PARAMS] + ["p_pitchmods"]
    with open(base + ".res.tsv", "w", encoding="utf-8", newline=chr(10)) as fo:
        fo.write(chr(9).join(cols) + chr(10))
        for r in rows:
            fo.write(chr(9).join(("%.5g" % r[k] if isinstance(r.get(k), float) else str(r.get(k, ""))) for k in cols) + chr(10))


def analyse(base):
    """Measures <base>.f32 by <base>.tsv and writes <base>.res.tsv; returns the rows as dicts."""
    rows = [l.rstrip("\n").split("\t") for l in open(base + ".tsv", encoding="latin1")]
    data = np.memmap(base + ".f32", dtype=np.float32, mode="r").reshape(len(rows), SHORT_N + LONG_N, 2)
    out = []
    for i, (synth, idx, group, name, text) in enumerate(rows):
        par = parse(text, synth)
        f0 = hz(PITCH[synth])
        x = np.asarray(data[i], dtype=np.float64)
        r = dict(synth=synth, index=idx, group=group, name=name)
        if (x[SHORT_N:] ** 2).sum() < 1e-12:
            r["silent"] = 1
        else:
            r["silent"] = 0
            r.update(long_stats(x[SHORT_N:], f0, par))
            r.update(short_stats(x[:SHORT_N], f0))
        for k in PARAMS:
            r["p_" + k] = par.get(k, "")
        mods = []
        for s in range(1, 9):
            dst, src, amt = par.get("mx%d_dst" % s, 0), par.get("mx%d_src" % s, 0), par.get("mx%d_amount" % s, 0)
            if dst in (1.0, 2.0) and src and amt:
                mods.append("%d>%d:%.3f" % (int(src), int(dst), amt))
        r["p_pitchmods"] = ",".join(mods)
        out.append(r)
    del data
    write_rows(base, out)
    return out


def over(r, synth):
    """The limits a preset is over, as a list of their names."""
    if r["silent"]:
        return ["silent"]
    bad = []
    if synth != "bass" and abs(r["s_mean"]) > LIMITS["s_mean"]:
        bad.append("s_mean")
    if r["unexplained"] > LIMITS["unexplained"]:
        bad.append("unexplained")
    if r["out100"] > LIMITS["out100"] and r["if_out50"] > 0.05:
        bad.append("out100")
    if abs(r["if_mean"]) > LIMITS["if_mean"]:
        bad.append("if_mean")
    return bad


def report(synth, rows):
    """Prints the table of one synth, a line per group; returns the number of presets over a limit."""
    groups = {}
    for r in rows:
        groups.setdefault(r["group"], []).append(r)
    print("== %s: %d presets" % (synth, len(rows)))
    print("   %-18s %8s %8s %8s %8s %6s" % ("group", "short ct", "long ct", "beside", ">100 ct", "over"))
    total = 0
    for g, rs in groups.items():
        ok = [r for r in rs if not r["silent"]]
        n = sum(1 for r in rs if over(r, synth))
        total += n
        if not ok:
            print("   %-18s silent" % g)
            continue

        def med(k):
            return float(np.median([r[k] for r in ok]))

        print("   %-18s %+8.1f %+8.1f %7.1f%% %7.1f%% %6d" % (g, med("s_mean"), med("if_mean"), 100 * med("unexplained"),
                                                            100 * med("out100"), n))
    return total


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    ap.add_argument("--exe", default=os.path.join(root, "build", "msvc", "Tools", "tunebench", "Release", "phos_tunebench.exe"))
    ap.add_argument("--work", default=os.path.join(root, "work", "tuning"))
    ap.add_argument("--synths", default=",".join(SYNTHS))
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--keep", action="store_true", help="keep the audio (1.4 GB a synth, in chunks)")
    ap.add_argument("--list", type=int, default=12, help="how many presets over a limit to name per synth")
    ap.add_argument("--strict", action="store_true", help="exit code 1 when a preset is over a limit")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    synths = [s for s in a.synths.split(",") if s]

    # The bench renders about seven times faster than real time (the whole engine runs under the one note), and a
    # preset is 3.75 s: a synth in one process is ten minutes. So every synth goes out in chunks, a process each.
    CHUNK = 64

    def one(job):
        synth, first = job
        base = os.path.join(a.work, "%s.%04d" % (synth, first))
        subprocess.run([a.exe, str(SYNTHS.index(synth) + 1), base + ".f32", base + ".tsv", str(first), str(CHUNK)], check=True)
        rows = analyse(base)
        os.remove(base + ".tsv")
        os.remove(base + ".res.tsv")
        if not a.keep:
            os.remove(base + ".f32")
        return rows

    jobs = [(s, f) for s in synths for f in range(0, 1024, CHUNK)]
    with ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        parts = list(pool.map(one, jobs))
    results = []
    for synth in synths:
        rows = [r for job, part in zip(jobs, parts) if job[0] == synth for r in part]
        write_rows(os.path.join(a.work, synth), rows)
        results.append(rows)
    total = 0
    for synth, rows in zip(synths, results):
        total += report(synth, rows)
        bad = [(r, over(r, synth)) for r in rows]
        bad = [(r, b) for r, b in bad if b]
        bad.sort(key=lambda rb: -max(abs(rb[0].get("s_mean", 0)) / LIMITS["s_mean"], rb[0].get("unexplained", 0) / LIMITS["unexplained"],
                                     rb[0].get("out100", 0) / LIMITS["out100"], abs(rb[0].get("if_mean", 0)) / LIMITS["if_mean"]))
        for r, b in bad[: a.list]:
            print("      %-16s %-24s short %+7.1f ct  long %+6.1f ct  beside %5.1f%%  >100 ct %5.1f%%  (%s)" % (
                r["group"], r["name"], r.get("s_mean", 0), r.get("if_mean", 0), 100 * r.get("unexplained", 0),
                100 * r.get("out100", 0), ",".join(b)))
    print("%d presets over a limit" % total)
    return 1 if a.strict and total else 0


if __name__ == "__main__":
    sys.exit(main())
