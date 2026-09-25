#!/usr/bin/env python3
"""Measure every wavetable of Noctuary's library and choose the set Phosphene ships.

Phosphene is not Noctuary, but it still has one part that wants what Noctuary was built for: a
psytrance set needs four different things at once, and the four are measurable:

* the **pad** needs tables whose spectrum *moves* across the frames -- a table whose 64 frames are
  nearly the same wave is 2 MB of mip levels that sound like one frame -- and that keep a
  fundamental, because a pad that is all formant disappears under the kick;
* the **lead** needs hard, bright tables -- the sync- and PWM-like shapes whose upper harmonics
  carry the sound -- and it needs them to survive the mipmap at C5 and C6, where the lead plays;
* the **arp** sits between the two: bright but thinner than the lead, so it does not mask it;
* the **drone** (added 20.09.2026, round "wavetable-selection") wants what Noctuary's own tables
  were built for after all: an organ-like, clearly pitched fundamental that stays put rather than
  sweeping, an octave or more under the pad, moving only as far as its own slow cutoff/position
  ramp (Composer.cpp, kSaltDroneRide) needs.

So this tool measures, per candidate table:

``centroid``         power-weighted spectral centroid in harmonic numbers, median over the frames.
``centroid_span``    log2 of the ratio of the largest to the smallest frame centroid: how far the
                     table travels in brightness.
``move``             median total variation between the power spectra of neighbouring frames, in
                     [0, 1]. 0 is a table that does not move at all.
``travel``           total variation between the first and the last frame: the whole journey.
``path``             the sum of all the steps: how far the table walks in all.
``directness``       ``travel / path``, in [0, 1] by the triangle inequality. This is the one
                     number that separates a *sweep* from a *shuffle*, and the pad needs the
                     difference: a bank of 64 unrelated waves has a huge ``move`` and a
                     ``directness`` near zero, and under a slow position LFO it steps rather than
                     glides. A table that walks steadily from one timbre to another has a small
                     ``move``, a large ``travel`` and a ``directness`` an order of magnitude higher.
``f1``               median share of the frame's power in the fundamental.
``odd``              share of the power in odd harmonics (a square is 1, a saw 0.5): the
                     hollow/full axis.
``alias_c5``,
``alias_c6``         the aliasing the table actually produces after mipmapping, measured the way
                     the DSP round of 16.09.2026 measured the supersaw: a held note rendered
                     through the same level choice, the same Catmull-Rom and the same guards the
                     C++ uses, at a pitch that is an exact bin of the analysis, so everything that
                     is not a multiple of the fundamental is alias and nothing is leakage.

The choice is then made by *spanning*, not by taste: inside each lane's pool the tables are ranked
by a lane score, the best one is taken, and every further one is the candidate whose normalised
feature vector is farthest from everything already chosen (Gonzalez, "Clustering to minimize the
maximum intercluster distance", TCS 38, 1985 -- the same greedy farthest-point rule). Twelve tables
that cluster in one corner of the space would be one table twelve times over.

Usage::

    python Tools/wt_select.py --measure           # measure the library into Tools/wt_measure.json
    python Tools/wt_select.py --select            # choose, print the table, write the selection
    python Tools/wt_select.py --measure --select  # both

The library itself is never copied into the repository by this tool; ``wt_pack.py`` writes the one
packed file that ships.
"""

import argparse
import json
import math
import os
import struct
import sys
import time

import numpy as np

# --------------------------------------------------------------------------------------------
# The library and the constants the C++ side uses (WaveTable.h). Kept literal rather than parsed
# out of the header: when one of them changes, this file must be looked at anyway.
# --------------------------------------------------------------------------------------------

LIBRARY = r"G:\Tools\VRAudio\AmbientSynth\Library\Wavetables"
HERE = os.path.dirname(os.path.abspath(__file__))
MEASURE_JSON = os.path.join(HERE, "wt_measure.json")
SELECTION_JSON = os.path.join(HERE, "wt_selection.json")

STORE_LEN = 4096          # WaveTable::kStoreLen
LEVELS = 10               # WaveTable::kLevels
MAX_FRAMES = 64           # WaveTable::kMaxFrames
TOP_HARMONICS = STORE_LEN // 8    # WaveTable::levelHarmonics(0) = 512

SR = 48000.0
FFT_N = 65536             # the window of the DSP round


def level_length(level):
    """Samples per stored cycle at a level (WaveTable::levelLength)."""
    n = STORE_LEN >> level
    return 32 if n < 32 else n


def level_harmonics(level):
    """Harmonics a level keeps (WaveTable::levelHarmonics)."""
    h = TOP_HARMONICS >> level
    return 1 if h < 1 else h


def wave_level_for(hz, sample_rate):
    """The level a cycle at *hz* reads, with no previous level (waveLevelFor(hz, sr, -1))."""
    nyquist = 0.5 * sample_rate
    level = 0
    while level < LEVELS - 1 and level_harmonics(level) * hz >= nyquist:
        level += 1
    return level


# --------------------------------------------------------------------------------------------
# Reading the library files
# --------------------------------------------------------------------------------------------

def read_wav_mono(path):
    """A minimal WAV reader for the two layouts the library holds.

    ``Classic/`` is 16-bit PCM with the ``clm `` chunk Serum writes and Vital copies;
    ``Harmonic/`` and ``Ambient/`` are 32-bit float with no ``clm`` at all. Returns
    ``(samples float64, cycle_len)``, the cycle length from the ``clm`` chunk when the file
    states it and otherwise 2048 -- the layout every file of this library is written in
    (Noctuary ``Tools/WavetableLib/build_classic.py`` and ``Tools/WavetableGen``).
    """
    raw = open(path, "rb").read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(path + ": not a RIFF/WAVE file")
    at, bits, fmt, chans, data, clm = 12, 0, 0, 1, None, 0
    while at + 8 <= len(raw):
        tag = raw[at:at + 4]
        size = struct.unpack("<I", raw[at + 4:at + 8])[0]
        at += 8
        body = raw[at:at + size]
        if tag == b"fmt ":
            fmt, chans = struct.unpack("<HH", body[0:4])
            bits = struct.unpack("<H", body[14:16])[0]
        elif tag == b"clm ":
            text = body.decode("latin-1")
            if text.startswith("<!>"):
                clm = int(text[3:].split()[0])
        elif tag == b"data":
            data = body
        at += size + (size & 1)
    if data is None:
        raise ValueError(path + ": no data chunk")
    if fmt == 3 and bits == 32:
        x = np.frombuffer(data, dtype="<f4").astype(np.float64)
    elif fmt == 1 and bits == 16:
        x = np.frombuffer(data, dtype="<i2").astype(np.float64) / 32768.0
    elif fmt == 1 and bits == 24:
        b = np.frombuffer(data, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = (b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16))
        v = np.where(v >= (1 << 23), v - (1 << 24), v)
        x = v.astype(np.float64) / 8388608.0
    else:
        raise ValueError("%s: format %d / %d bit is not read here" % (path, fmt, bits))
    if chans > 1:
        x = x.reshape(-1, chans).mean(axis=1)
    return x, (clm if clm and clm >= 8 else 2048)


def frame_spectra(samples, cycle_len, max_frames=MAX_FRAMES):
    """The harmonics of every frame, as the complex amplitudes of cosines.

    One row per frame, ``TOP_HARMONICS`` columns; element ``h-1`` is harmonic ``h``. Frames beyond
    *max_frames* are thinned evenly, exactly as Noctuary's ``CycleTable::build`` does at b60a2fe --
    ``src = k * (total - 1) / (keep - 1)``.
    """
    total = len(samples) // cycle_len
    if total < 1:
        raise ValueError("not one whole frame")
    keep = min(total, max_frames)
    if keep == total:
        idx = np.arange(total)
    else:
        idx = (np.arange(keep) * (total - 1)) // max(keep - 1, 1)
    block = np.stack([samples[i * cycle_len:(i + 1) * cycle_len] for i in idx])
    spec = np.fft.rfft(block, axis=1) * (2.0 / cycle_len)
    top = min(TOP_HARMONICS, (cycle_len - 1) // 2)
    out = np.zeros((keep, TOP_HARMONICS), dtype=np.complex128)
    out[:, :top] = spec[:, 1:top + 1]
    return out


# --------------------------------------------------------------------------------------------
# The measurements
# --------------------------------------------------------------------------------------------

def build_level(coeffs, level):
    """One stored cycle at *level* from a frame's harmonics -- what ``synthesise()`` writes.

    ``x[n] = sum_h |c_h| cos(2 pi h n / len + arg c_h)``, the harmonics above the level dropped.
    """
    length = level_length(level)
    top = min(level_harmonics(level), len(coeffs))
    spec = np.zeros(length // 2 + 1, dtype=np.complex128)
    spec[1:top + 1] = coeffs[:top] * (0.5 * length)
    return np.fft.irfft(spec, n=length)


def read_catmull(cycle, phase):
    """The Catmull-Rom read of ``WaveTable::sample`` at a vector of phases in [0, 1).

    The stored cycle carries one guard sample before it and two after it, which are the wrap of the
    cycle itself; taking the four taps modulo the length is the same thing and vectorises.
    """
    length = len(cycle)
    x = phase * length
    i = np.floor(x).astype(np.int64)
    np.clip(i, 0, length - 1, out=i)
    t = (x - i).astype(np.float64)
    y0 = cycle[(i - 1) % length]
    y1 = cycle[i % length]
    y2 = cycle[(i + 1) % length]
    y3 = cycle[(i + 2) % length]
    a = 0.5 * (y2 - y0)
    b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3
    d = 0.5 * (y3 - y0) + 1.5 * (y1 - y2)
    return ((d * t + b) * t + a) * t + y1


def alias_db(coeffs, note_hz, sample_rate=SR):
    """Aliasing of one frame played at *note_hz*, in dB relative to its harmonic power.

    The pitch is moved to the nearest exact bin of the 65536-point analysis, so the harmonics land
    on bins ``K, 2K, ...`` and every other bin between 100 Hz and 18 kHz is alias -- no window, no
    leakage, no band around a line to argue about. This is the same quantity the DSP round called
    ``supersawAliasDb`` for the single-oscillator case.
    """
    k = max(1, int(round(note_hz * FFT_N / sample_rate)))
    f0 = k * sample_rate / FFT_N
    level = wave_level_for(f0, sample_rate)
    cycle = build_level(coeffs, level)
    step = f0 / sample_rate
    phase = (np.arange(FFT_N) * step) % 1.0
    y = read_catmull(cycle, phase)
    p = np.abs(np.fft.rfft(y)) ** 2
    lo = int(math.ceil(100.0 * FFT_N / sample_rate))
    hi = int(math.floor(18000.0 * FFT_N / sample_rate))
    band = np.zeros(len(p), dtype=bool)
    band[lo:hi + 1] = True
    harmonic = np.zeros(len(p), dtype=bool)
    harmonic[k::k] = True
    sig = float(p[band & harmonic].sum())
    noise = float(p[band & ~harmonic].sum())
    if sig <= 0.0:
        return 0.0
    return 10.0 * math.log10(max(noise, 1e-300) / sig)


def measure_table(path):
    """Every number of one candidate table."""
    samples, cycle_len = read_wav_mono(path)
    coeffs = frame_spectra(samples, cycle_len)
    frames = coeffs.shape[0]
    power = np.abs(coeffs) ** 2
    total = power.sum(axis=1)
    live = total > 0.0
    if not live.any():
        raise ValueError("silent")
    norm = power[live] / total[live][:, None]
    h = np.arange(1, TOP_HARMONICS + 1, dtype=np.float64)

    centroid = norm @ h
    f1 = norm[:, 0]
    odd = norm[:, 0::2].sum(axis=1)
    # Total variation between neighbouring frames: 0.5 * L1 of two distributions, in [0, 1].
    if norm.shape[0] > 1:
        step = 0.5 * np.abs(np.diff(norm, axis=0)).sum(axis=1)
        travel = float(0.5 * np.abs(norm[-1] - norm[0]).sum())
    else:
        step = np.zeros(1)
        travel = 0.0
    walked = float(step.sum())
    # Aliasing: the first, the middle and the last frame, the worst of the three.
    probe = sorted(set([0, frames // 2, frames - 1]))
    a5 = max(alias_db(coeffs[i], 523.2511) for i in probe)
    a6 = max(alias_db(coeffs[i], 1046.502) for i in probe)

    cmin, cmax = float(centroid.min()), float(centroid.max())
    return {
        "frames": int(frames),
        "cycle_len": int(cycle_len),
        "centroid": float(np.median(centroid)),
        "centroid_min": cmin,
        "centroid_max": cmax,
        "centroid_span": float(math.log2(max(cmax, 1e-9) / max(cmin, 1e-9))),
        "move": float(np.median(step)),
        "move_max": float(step.max()),
        "travel": travel,
        "path": walked,
        "directness": float(travel / walked) if walked > 1e-12 else 0.0,
        "f1": float(np.median(f1)),
        "f1_min": float(f1.min()),
        "odd": float(np.median(odd)),
        "alias_c5": a5,
        "alias_c6": a6,
        "bytes": int(os.path.getsize(path)),
    }


# --------------------------------------------------------------------------------------------
# Provenance
# --------------------------------------------------------------------------------------------

def provenance(json_path):
    """What a sidecar says about where its table comes from.

    Returns ``(kind, source)``. ``kind`` is ``"cc0"`` for the AKWF and WaveEdit Online tables of
    ``Classic/`` (both CC0 1.0 Universal, see ``CREDITS-classic.md``), ``"procedural"`` for a
    sidecar that names no source at all -- Noctuary's own ``HarmonicGen``/``AmbientGen`` output --
    and ``"measured"`` for one whose ``recipe.source`` names an audio file, which is a spectral
    analysis of the user's own Stable Audio 3 material.

    The test is structural, on the shape of the sidecar, never on the spelling of a file name.
    """
    try:
        meta = json.load(open(json_path, encoding="utf-8"))
    except Exception:
        return "unknown", ""
    top = meta.get("source")
    recipe = meta.get("recipe") if isinstance(meta.get("recipe"), dict) else {}
    inner = recipe.get("source")
    if isinstance(inner, str) and os.path.splitext(inner)[1].lower() in (".flac", ".wav", ".aif", ".aiff", ".mp3", ".ogg"):
        return "measured", inner
    if isinstance(top, str):
        lic = str(meta.get("license", ""))
        return ("cc0" if "CC0" in lic else "unknown"), top
    if top is None and inner is None:
        return "procedural", str(meta.get("generator", ""))
    return "unknown", str(top or inner)


def candidates():
    """Every ``.wav``/``.json`` pair of the library, with its provenance."""
    out = []
    for folder in ("Classic", "Harmonic", "Ambient"):
        d = os.path.join(LIBRARY, folder)
        for name in sorted(os.listdir(d)):
            if not name.endswith(".wav"):
                continue
            wav = os.path.join(d, name)
            js = wav[:-4] + ".json"
            kind, src = provenance(js)
            out.append({"id": folder + "/" + name[:-4], "path": wav, "folder": folder,
                        "kind": kind, "source": src})
    return out


def measure_all(only=None):
    """Measures the whole library into ``wt_measure.json`` (a cache: existing entries are kept)."""
    have = {}
    if os.path.exists(MEASURE_JSON):
        have = json.load(open(MEASURE_JSON, encoding="utf-8"))
    cands = candidates()
    t0 = time.time()
    done = 0
    for c in cands:
        if only and only not in c["id"]:
            continue
        if c["id"] in have:
            continue
        try:
            m = measure_table(c["path"])
        except Exception as exc:          # a broken file must not stop the round
            m = {"error": str(exc)}
        m["kind"] = c["kind"]
        m["source"] = c["source"]
        m["folder"] = c["folder"]
        have[c["id"]] = m
        done += 1
        if done % 50 == 0:
            el = time.time() - t0
            print("  %4d measured, %.1f s (%.0f ms each)" % (done, el, 1000.0 * el / done), flush=True)
    json.dump(have, open(MEASURE_JSON, "w", encoding="utf-8"), indent=1, sort_keys=True)
    print("measured %d tables in %.1f s -> %s" % (done, time.time() - t0, MEASURE_JSON))
    return have


# --------------------------------------------------------------------------------------------
# The choice
# --------------------------------------------------------------------------------------------

# The axes the farthest-point rule spreads over, with the range each is normalised by. They are the
# axes the three lanes differ along; the raw numbers have wildly different units, and an unscaled
# distance would be the centroid's alone.
AXES = (("centroid", 1.0, 60.0),        # harmonic number, log-scaled below
        ("move", 0.0, 0.25),
        ("travel", 0.0, 1.0),
        ("directness", 0.0, 0.5),
        ("f1", 0.0, 0.6),
        ("odd", 0.35, 0.95),
        ("centroid_span", 0.0, 3.0))


def feature_vector(m):
    """The normalised point of a table in the space the selection spreads over."""
    v = []
    for name, lo, hi in AXES:
        x = m[name]
        if name == "centroid":
            x = math.log2(max(x, 1.0))
            lo, hi = math.log2(max(lo, 1.0)), math.log2(hi)
        v.append((x - lo) / (hi - lo))
    return np.array(v, dtype=np.float64)


# The four lanes, each with the gate a candidate must pass and the score that ranks the pool.
#
# The gates are the measurement's own thresholds, not preferences:
#   * ``alias_c6`` under -60 dB: the DSP round put the supersaw's table saw at -61.6 dB at A6 and
#     called that the state of the art of this engine. A table that aliases louder than the lead
#     already does would undo that round.
#   * ``move`` over 0.02: below it the frames of the table differ by two per cent of their power
#     distribution, and 64 frames of mip levels carry one frame's worth of sound.
#   * ``frames`` at least 16: fewer frames make the position knob a switch.
#
# 20.09.2026 (round "wavetable-selection"): every ``take`` widened from the "voices" round's 5/4/3
# (12 tables total, chosen to prove the four new voices had a sound of their own at all, against a
# library that was not yet measured for the purpose) towards how many candidates actually clear
# each lane's gate out of the full 2191 -- 496 pad, 124 lead, 175 arp -- while keeping the pack a
# fraction of what the Quest APK can absorb (docs/rounds/2026-09.md, round block of 20.09.2026, has the bytes).
# A fourth lane, ``drone``, is new: the tonic drone (Melody.cpp, ``makeDrone``) used to draw pad
# lane tables (indices 6/7/9/10, "the organ and the measured, slow tables" of kVoicePalette's own
# comment) because there was no lane measured for its own character -- a held low fundamental under
# a slow cutoff/position ramp (docs/rounds/2026-09.md, "Stimmen" round). That is not what a pad wants: a pad
# glides *through* timbres (high ``travel``/``directness``), a drone wants to *stay* one, clearly
# pitched, organ-like timbre while it slowly moves -- high ``f1`` (a real fundamental, not a cloud
# of partials), a dark-to-mid centroid (it sits at 70..280 Hz, an octave or more under the pad), and
# a **bounded** ``move`` -- gliding is still wanted for the slow ramp, but not a sweep, which is what
# unbounded ``move`` together with the pad's own high ``directness`` gate would let through.
LANE_ORDER = ("lead", "arp", "pad", "counter", "drone")

LANES = {
    "pad": dict(
        take=128,
        # A pad plays held chords under a slow position LFO, so the table must *glide*: a long
        # journey (travel) walked in small steps (directness). It must keep a fundamental, or it
        # disappears under the kick, and it must not alias, because a pad is the one part that
        # sounds all the way through a breakdown with nothing to mask it.
        gate=lambda m: (m["frames"] >= 32 and m["travel"] >= 0.30 and m["directness"] >= 0.08
                        and m["f1"] >= 0.03 and m["alias_c6"] <= -60.0 and m["centroid"] >= 2.5),
        score=lambda m: 2.0 * m["travel"] + 4.0 * m["directness"] + 0.4 * math.log2(max(m["centroid"], 1.0)),
    ),
    "lead": dict(
        take=80,
        # A lead plays short notes: it wants hard and bright, and somewhere to go under the
        # position envelope. Gliding does not matter -- the note is over before a sweep arrives --
        # so directness is not gated here; brightness at C5/C6 without aliasing is.
        gate=lambda m: (m["frames"] >= 16 and m["centroid"] >= 8.0 and m["alias_c6"] <= -60.0
                        and m["f1"] <= 0.5 and m["centroid_span"] >= 0.2),
        score=lambda m: math.log2(max(m["centroid"], 1.0)) + 1.5 * m["centroid_span"] + 2.0 * m["move"],
    ),
    "arp": dict(
        take=96,
        # An arp is a lead that must not mask the lead: mid brightness, the hollow end of the
        # odd/even axis (where a pulse and a hard sync sit), and enough directedness that a
        # sixteenth line does not jump timbre from note to note.
        gate=lambda m: (m["frames"] >= 16 and 4.0 <= m["centroid"] <= 24.0
                        and m["alias_c6"] <= -62.0 and m["travel"] >= 0.15 and m["directness"] >= 0.05),
        score=lambda m: -2.0 * abs(m["odd"] - 0.80) + 1.5 * m["directness"] + 0.3 * m["centroid_span"],
    ),
    "counter": dict(
        take=96,
        # The counter-lead answers the lead, so it must not *be* the lead (Composer.cpp keeps a hard
        # guard against sharing its oscillator or its table). Until 22.09.2026 it had no lane at all:
        # it drew from three built-ins, the vocal/formant family, which was a deliberate choice while
        # the library held 35 tables and became the reason the counter was the one voice that sounded
        # the same in every track. Its character is the vowel: a mid centroid -- under the lead's
        # floor of 8, over the drone's ceiling -- that *moves* across the frames (centroid_span), with
        # a fundamental still present, which is what a formant sweep is and what makes an answering
        # line read as a second voice rather than a thinner copy of the first.
        gate=lambda m: (m["frames"] >= 16 and 3.0 <= m["centroid"] <= 12.0 and m["f1"] >= 0.08
                        and m["centroid_span"] >= 0.5 and m["alias_c6"] <= -60.0),
        score=lambda m: 1.5 * m["centroid_span"] + 1.0 * m["travel"] + 0.5 * m["f1"],
    ),
    "drone": dict(
        take=64,
        # Organ-like and dark: a strong, clear fundamental (f1 well above the library's own median
        # of 0.47), a centroid under the pad lane's own floor, and a move that is bounded rather than
        # merely gated from below -- steady enough that the slow cutoff/position ramp of
        # Composer.cpp's drone ride (kSaltDroneRide) does not turn into an audible sweep, which is
        # exactly the pad's job, not the drone's.
        gate=lambda m: (m["frames"] >= 16 and m["f1"] >= 0.55 and m["centroid"] <= 5.0
                        and m["move"] <= 0.12 and m["alias_c6"] <= -58.0),
        score=lambda m: 2.0 * m["f1"] - 0.4 * math.log2(max(m["centroid"], 1.0)) + 1.0 * m["directness"],
    ),
}


def farthest_point(pool, take, score):
    """Greedy farthest-point choice: the best-scoring candidate, then the most different each time.

    Gonzalez's 2-approximation of the k-centre problem (TCS 38, 1985). The point is not to find an
    optimum but to refuse a cluster: every table after the first is the one whose distance to the
    nearest already-chosen table is largest.
    """
    if not pool:
        return []
    order = sorted(pool, key=lambda c: -score(c["m"]))
    vecs = np.asarray([c["v"] for c in order], dtype=np.float64)
    # 22.09.2026: the same rule, but the distance to the chosen set is carried along instead of
    # recomputed. The old loop was O(take^2 * pool) single distances in Python, which cost seconds
    # at take=8 and would have cost an hour at the takes the lanes need (a lane now takes up to
    # 128 of a pool of ~500). `near[i]` is always the distance from candidate i to its nearest
    # chosen table, so each further pick is one argmax and one vectorised update; the tables it
    # returns are the same ones, in the same order.
    near = np.linalg.norm(vecs - vecs[0], axis=1)
    near[0] = -1.0                      # chosen: never picked again
    chosen = [order[0]]
    while len(chosen) < take and len(chosen) < len(order):
        j = int(np.argmax(near))
        if near[j] < 0.0:
            break
        chosen.append(order[j])
        near = np.minimum(near, np.linalg.norm(vecs - vecs[j], axis=1))
        near[j] = -1.0
    return chosen


def display_name(entry):
    """A short display name for the parameter's choice list, from the file's own name.

    The name says where the table comes from and nothing else: a user who reads "AKWF 0006-02" in
    the Table menu can find that file in the library, and the name never has to be re-invented when
    the selection is run again. At most 31 characters, the field the pack reserves for it.
    """
    stem = entry["id"].split("/")[1]
    if stem.startswith("akwf_"):
        return "AKWF " + stem[5:].replace("_", "-")[:26]
    if stem.startswith("wavedit_"):
        rest = stem[8:]
        return "WaveEdit " + rest[:1].upper() + rest[1:22]
    for prefix, label in (("ambient_", ""), ("harmonic_", "")):
        if stem.startswith(prefix):
            stem = label + stem[len(prefix):]
    parts = [p for p in stem.replace("-", "_").split("_") if p]
    words = [p.capitalize() for p in parts if not p.isdigit()]
    tag = " ".join(p for p in parts if p.isdigit())
    return ((" ".join(words) + " " + tag).strip() or "Table")[:31]


def select(measures, allow_measured=True):
    """Chooses the set, prints the table of numbers, and writes ``wt_selection.json``."""
    pool_all = []
    excluded_measured = 0
    for key, m in sorted(measures.items()):
        if "error" in m:
            continue
        if m.get("kind") == "measured":
            excluded_measured += 1
            if not allow_measured:
                continue
        pool_all.append({"id": key, "m": m, "v": feature_vector(m)})

    print("eligible candidates: %d of %d measured   (tables whose sidecar names a sample source: %d, %s --"
          " the material is the user's own)"
          % (len(pool_all), len(measures), excluded_measured,
             "left out by --no-measured" if not allow_measured else "included"))

    chosen, used = {}, set()
    for lane in LANE_ORDER:
        cfg = LANES[lane]
        pool = [c for c in pool_all if c["id"] not in used and cfg["gate"](c["m"])]
        print("  lane %-5s: %d of %d pass the gate" % (lane, len(pool), len(pool_all)))
        picked = farthest_point(pool, cfg["take"], cfg["score"])
        for c in picked:
            used.add(c["id"])
        chosen[lane] = picked

    rows = []
    print()
    print("%-5s %-22s %-20s %6s %5s %6s %6s %6s %5s %6s %6s %4s" %
          ("lane", "table", "source", "centr", "span", "move", "trav", "direct", "f1", "C5 dB", "C6 dB", "frm"))
    for lane in LANE_ORDER:
        for c in chosen[lane]:
            m = c["m"]
            rows.append({"lane": lane, "id": c["id"], "name": display_name(c),
                         "kind": m.get("kind"), "metrics": m})
            print("%-5s %-22s %-20s %6.2f %5.2f %6.3f %6.3f %6.3f %5.3f %6.1f %6.1f %4d" %
                  (lane, display_name(c), c["id"].split("/")[0] + "/" + m.get("kind", "?"),
                   m["centroid"], m["centroid_span"], m["move"], m["travel"], m["directness"], m["f1"],
                   m["alias_c5"], m["alias_c6"], m["frames"]))
    json.dump({"tables": rows}, open(SELECTION_JSON, "w", encoding="utf-8"), indent=1)
    print("\n%d tables -> %s" % (len(rows), SELECTION_JSON))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--measure", action="store_true", help="measure the library into the cache")
    ap.add_argument("--select", action="store_true", help="choose from the cache")
    ap.add_argument("--only", help="measure only ids containing this text")
    ap.add_argument("--no-measured", action="store_true",
                    help="leave out the tables whose sidecar names a sample source")
    args = ap.parse_args()
    if not args.measure and not args.select:
        ap.error("nothing to do: pass --measure, --select, or both")
    data = {}
    if args.measure:
        data = measure_all(args.only)
    if args.select:
        if not data:
            data = json.load(open(MEASURE_JSON, encoding="utf-8"))
        select(data, allow_measured=not args.no_measured)
    return 0


if __name__ == "__main__":
    sys.exit(main())
