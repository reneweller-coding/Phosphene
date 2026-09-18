"""Arrangement dynamics: the three measurements of the round of 16.09.2026.

The user's review asked for three things -- a snare roll that lifts, a macro ride on the acid, and a
tempo-synchronous auto-pan on the percussion -- and each of them has exactly one number that decides
whether it works. This tool produces those numbers, on renders and on the reference recordings, so
that "before" and "after" are the same measurement and not two different opinions.

    --contrast FILE --bpm B --pdb BAR     the buildup-to-drop step: loudness and spectral contrast
                                          between the last bar of the buildup and the first bar of
                                          the drop, plus the four bars of the roll one by one
    --roll FILE --bpm B --from BAR --bars N
                                          the roll itself: per-hit band balance and centroid, so a
                                          pitch ramp and a rising high pass are visible as numbers
    --ride FILE --bpm B --from BAR --to BAR
                                          the acid's macro movement: the power centroid per bar over
                                          a section (the macro ride) against the movement *within* a
                                          bar (the accent sweep and the per-note envelope), so the
                                          two can be told apart
    --pan FILE [--band air]               the width question of the round: the 85 ms inter-channel
                                          level difference, the side/mid ratio and the correlation,
                                          together with the bound that panning alone cannot beat
    --pan-bound                           that bound on its own, derived and printed

Needs ffmpeg on the PATH, numpy and Tools/metrics.py.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics as M                                                      # noqa: E402

#: The bands the round reads. "air" is the one the width round found short of the references.
BANDS = {"low": (40.0, 140.0), "low-mid": (140.0, 500.0), "mid": (500.0, 1500.0),
         "presence": (1500.0, 6000.0), "air": (6000.0, 16000.0)}


def bar_seconds(bpm):
    """Length of a 4/4 bar in seconds."""
    return 4.0 * 60.0 / bpm


def slice_of(path, bpm, bar, bars=1.0):
    """`bars` bars of `path` starting at bar index `bar` (0-based, as phos_render --tracks prints)."""
    b = bar_seconds(bpm)
    return M.decode(path, start=bar * b, dur=bars * b)


def rms_db(x):
    """Full-band rms of a stereo block in dB, channel powers summed (as every number of the plan is)."""
    return 10.0 * np.log10(max(float(np.mean(x ** 2)), 1e-30))


def band_db(x, lo, hi):
    """Power of one band in dB, channel powers summed."""
    f, p = M.welch(x)
    sel = (f >= lo) & (f < hi)
    return 10.0 * np.log10(max(float(p[sel].sum()), 1e-30))


def spectral_contrast(x):
    """(centroid in Hz, air-minus-low balance in dB): the two sides of "how bright is this bar".

    The centroid alone hides a bar that is both bright and heavy; the balance alone hides where
    inside the top the energy sits. Both are read from the same Welch estimate.
    """
    f, p = M.welch(x)
    c = M.centroid(f, p)
    lo = 10.0 * np.log10(max(float(p[(f >= 40.0) & (f < 140.0)].sum()), 1e-30))
    hi = 10.0 * np.log10(max(float(p[(f >= 6000.0) & (f < 16000.0)].sum()), 1e-30))
    return c, hi - lo


def contrast(path, bpm, pdb_bar, roll_bars=4):
    """The step from the end of a buildup into the drop.

    `pdb_bar` is the pre-drop break, i.e. the last bar of the buildup; the drop starts at pdb_bar+1.
    The roll runs over the `roll_bars` bars ending with it.
    """
    rows = []
    for k in range(roll_bars):
        bar = pdb_bar - (roll_bars - 1) + k
        x = slice_of(path, bpm, bar)
        c, bal = spectral_contrast(x)
        rows.append((f"roll bar {k}", bar, rms_db(x), c, bal))
    drop = slice_of(path, bpm, pdb_bar + 1)
    c, bal = spectral_contrast(drop)
    rows.append(("drop bar 0", pdb_bar + 1, rms_db(drop), c, bal))
    print(f"{os.path.basename(path)}  {bpm:.1f} BPM  PDB at bar {pdb_bar}")
    print("  what           bar      rms dB   centroid Hz   air-low dB")
    for name, bar, r, c, bal in rows:
        print(f"  {name:<13} {bar:4d}   {r:8.2f}   {c:9.0f}    {bal:8.2f}")
    pdb = rows[-2]
    drop_row = rows[-1]
    print(f"  contrast PDB -> drop: loudness {drop_row[2] - pdb[2]:+.2f} dB, "
          f"centroid {drop_row[3] / max(pdb[3], 1e-9):.2f}x, air-low {drop_row[4] - pdb[4]:+.2f} dB")
    # The roll's own lift: the last roll bar before the PDB against the first.
    print(f"  lift over the roll:   loudness {rows[-2][2] - rows[0][2]:+.2f} dB, "
          f"centroid {rows[-2][3] / max(rows[0][3], 1e-9):.2f}x, air-low {rows[-2][4] - rows[0][4]:+.2f} dB")
    return rows


def roll_hits(path, bpm, first_bar, bars=4, step=0.125):
    """Per-hit centroid and band balance over a roll, on the 32nd grid.

    Each hit is read over one 32nd note, which at 145 BPM is 52 ms -- shorter than the snare's tail,
    so consecutive readings overlap in energy; the *trend* is what the measurement is for, and a
    trend that survives overlapping windows is a real one.
    """
    b = bar_seconds(bpm)
    beat = b / 4.0
    x = M.decode(path, start=first_bar * b, dur=bars * b)
    n = int(round(bars * 4.0 / step))
    hop = int(round(step * beat * M.SR))
    win = hop
    out = []
    for i in range(n):
        seg = x[:, i * hop:i * hop + win]
        if seg.shape[1] < 64:
            break
        f, p = M.welch(seg, n=1 << 12)
        c = M.centroid(f, p)
        lo = float(p[(f >= 150.0) & (f < 500.0)].sum())
        hi = float(p[(f >= 2000.0) & (f < 8000.0)].sum())
        out.append((i, c, 10.0 * np.log10(max(hi, 1e-30) / max(lo, 1e-30)), rms_db(seg)))
    return out


def report_roll(path, bpm, first_bar, bars=4):
    """Prints the roll's trend: first quarter against last quarter of the hits."""
    rows = roll_hits(path, bpm, first_bar, bars)
    if not rows:
        print("no hits")
        return
    q = max(1, len(rows) // 4)
    head, tail = rows[:q], rows[-q:]
    cf, ct = np.mean([r[1] for r in head]), np.mean([r[1] for r in tail])
    bf, bt = np.mean([r[2] for r in head]), np.mean([r[2] for r in tail])
    print(f"{os.path.basename(path)}  roll from bar {first_bar}, {bars} bars, {len(rows)} windows")
    print(f"  centroid      first quarter {cf:8.0f} Hz   last quarter {ct:8.0f} Hz   "
          f"{12.0 * np.log2(max(ct, 1e-9) / max(cf, 1e-9)):+.2f} semitones")
    print(f"  2-8k / 150-500 first quarter {bf:8.2f} dB   last quarter {bt:8.2f} dB   {bt - bf:+.2f} dB")
    return rows


def ride(path, bpm, first_bar, last_bar):
    """The acid's macro movement against its per-note movement.

    Two readings of the same signal. *Between* bars: the power centroid of each bar, whose spread
    over the section is the macro ride the review asked for. *Within* a bar: the centroid of each
    sixteenth, whose spread inside one bar is what the accent sweep and the per-note envelope already
    do. Both are reported in octaves, because a filter ride is heard in octaves and the two are only
    comparable on that scale.
    """
    bars, bright = [], []
    for bar in range(first_bar, last_bar):
        x = slice_of(path, bpm, bar)
        f, p = M.welch(x, n=1 << 14)
        bars.append(M.centroid(f, p, lo=100.0))
        # Brightness: the energy a filter ride moves, above the fundamental against below it. The
        # centroid alone hides a cutoff ride, because a resonant low-pass line keeps most of its power
        # in the fundamental wherever the corner stands; measured on this generator a cutoff ride of
        # 0.56 octaves moves the centroid by 0.10 and this ratio by 2 dB.
        lo = float(p[(f >= 100.0) & (f < 1000.0)].sum())
        hi = float(p[(f >= 1000.0) & (f < 8000.0)].sum())
        bright.append(10.0 * np.log10(max(hi, 1e-30) / max(lo, 1e-30)))
    steps = []
    b = bar_seconds(bpm)
    x = M.decode(path, start=first_bar * b, dur=(last_bar - first_bar) * b)
    hop = int(round(b / 16.0 * M.SR))
    for i in range(int((last_bar - first_bar) * 16)):
        seg = x[:, i * hop:i * hop + hop]
        if seg.shape[1] < 64:
            break
        f, p = M.welch(seg, n=1 << 11)
        steps.append(M.centroid(f, p, lo=100.0))
    bars = np.array(bars)
    steps = np.array(steps)
    oct_bars = np.log2(np.maximum(bars, 1e-9))
    oct_steps = np.log2(np.maximum(steps, 1e-9))
    # The within-bar part is removed from the between-bar part by folding: a bar's mean is the macro
    # value, the deviation inside a bar is the per-note part.
    per_bar = oct_steps[:len(oct_steps) // 16 * 16].reshape(-1, 16)
    macro = per_bar.mean(axis=1)
    micro = per_bar - macro[:, None]
    print(f"{os.path.basename(path)}  bars {first_bar}..{last_bar}  {bpm:.1f} BPM")
    print(f"  bar centroids        {bars.min():7.0f} .. {bars.max():7.0f} Hz")
    print(f"  macro ride (between bars)  span {oct_bars.max() - oct_bars.min():.3f} oct   "
          f"sd {oct_bars.std():.3f} oct")
    print(f"  accent/envelope (within a bar) sd {micro.std():.3f} oct   "
          f"span {micro.max() - micro.min():.3f} oct")
    print(f"  macro from the folded steps  sd {macro.std():.3f} oct")
    br = np.array(bright)
    print(f"  brightness 1-8k / 0.1-1k     {br.min():7.2f} .. {br.max():7.2f} dB   sd {br.std():.2f} dB")
    return oct_bars, macro, micro


def pan(path, band="air", start=None, seconds=None):
    """The three width numbers of one band, on one window."""
    lo, hi = BANDS[band]
    if start is None:
        x = M.middle(path, seconds if seconds else 45.0)
    else:
        x = M.decode(path, start=start, dur=seconds if seconds else 45.0)
    f, pl, pr, cc = M.cross_spectra(x)
    sm, rho = M.band_width_rho(f, pl, pr, cc, lo, hi)
    ild, rho85 = M.short_pan(x, lo, hi)
    m = M.mono_loss(x)
    print(f"{os.path.basename(path)}  band {band} {lo:.0f}..{hi:.0f} Hz")
    print(f"  side/mid {sm:+.2f} dB   rho {rho:+.3f}   85 ms level difference {ild:.2f} dB rms   "
          f"|rho| 85 ms {rho85:.3f}")
    print(f"  cost of a mono sum (dB, to 40..140 Hz)  " + "  ".join(f"{k} {v:+.2f}" for k, v in m.items()))
    return sm, rho, ild, rho85


def pan_bound(sm_db=-9.35, n=4001):
    """What a constant-power panner can and cannot do -- derived, not assumed.

    For a *mono* source placed by a constant-power panner at position p, both numbers of the width
    measurement are readings of the same angle theta = (p+1)pi/4:

        side/mid over a long window : E[1 - cos(p pi/2)] / E[1 + cos(p pi/2)]
        level difference in a window: 10 log10( <1 - sin(p pi/2)> / <1 + sin(p pi/2)> )

    the first averaged over all time, the second over the 85 ms window only. Two consequences, and
    they decide the third item of this round:

    1. A *slow* pan cannot trade one against the other. Over a window short against the LFO the
       average inside the window is the instantaneous value, so both numbers read the same theta. The
       cheapest distribution of p at a given side/mid -- mass at the centre plus a vanishing
       excursion -- still costs a level difference, and this function computes that floor.
    2. A pan *fast against the window* can. Then <sin(p pi/2)> over the window tends to zero while
       E[cos(p pi/2)] does not, so the level difference falls with the side/mid staying put. That is
       not decorrelation of the material; it is modulation, and it is audible as movement.

    The floor of 1: minimise E[ILD^2] subject to E[cos(p pi/2)] = m. With mass w at p = 0 and 1 - w
    split over p = +-b, the constraint fixes (1 - w)(1 - cos(b pi/2)) = 1 - m, so
    E[ILD^2] = (1 - m) * ILD(b)^2 / (1 - cos(b pi/2)), whose infimum is at b -> 0 and equals
    (1 - m) * (20/ln10 * pi/2)^2 / (pi^2/8) = (1 - m) * 8 * (20/ln10)^2 / 2 ... evaluated here
    numerically over b so the closed form is checked against the sweep.
    """
    sm = 10.0 ** (sm_db / 10.0)
    m = (1.0 - sm) / (1.0 + sm)
    b = np.linspace(1e-4, 0.999, n)
    theta = (b + 1.0) * np.pi / 4.0
    ild = 10.0 * np.log10(np.cos(theta) ** 2 / np.sin(theta) ** 2)
    g = ild ** 2 / (1.0 - np.cos(b * np.pi / 2.0))
    floor = np.sqrt((1.0 - m) * g.min())
    # Closed form of the infimum: ILD(b) -> -(20/ln10) * (pi/2) * b and 1 - cos(b pi/2) -> (pi b/2)^2/2.
    closed = np.sqrt((1.0 - m) * 2.0 * (20.0 / np.log(10.0)) ** 2)
    print(f"  side/mid {sm_db:+.2f} dB  =>  E[cos(p pi/2)] = {m:.4f}")
    print(f"  floor of the 85 ms level difference for a slow pan of mono lanes: {floor:.2f} dB rms")
    print(f"  closed form of the same infimum: {closed:.2f} dB rms  (at b -> 0)")
    return floor, closed


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--contrast")
    ap.add_argument("--roll")
    ap.add_argument("--ride")
    ap.add_argument("--pan")
    ap.add_argument("--pan-bound", action="store_true")
    ap.add_argument("--bpm", type=float, default=145.0)
    ap.add_argument("--pdb", type=int, default=87)
    ap.add_argument("--from", dest="first", type=int, default=0)
    ap.add_argument("--to", dest="last", type=int, default=32)
    ap.add_argument("--bars", type=int, default=4)
    ap.add_argument("--band", default="air")
    ap.add_argument("--start", type=float, default=None)
    ap.add_argument("--seconds", type=float, default=None)
    ap.add_argument("--sm", type=float, default=-9.35)
    a = ap.parse_args(argv[1:])
    if a.contrast:
        contrast(a.contrast, a.bpm, a.pdb)
    if a.roll:
        report_roll(a.roll, a.bpm, a.first, a.bars)
    if a.ride:
        ride(a.ride, a.bpm, a.first, a.last)
    if a.pan:
        pan(a.pan, a.band, a.start, a.seconds)
    if a.pan_bound:
        pan_bound(a.sm)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
