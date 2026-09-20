#!/usr/bin/env python3
"""Pack the chosen wavetables into the one file Phosphene ships, and generate its table of names.

Reads ``Tools/wt_selection.json`` (written by ``wt_select.py``) and writes three things:

``Core/data/library.phoswt``              the pack, whose byte layout is normative in
                                          ``Core/include/phos/WaveTableFile.h``;
``Core/include/phos/WaveTableList.inl``   the generated list of names and ids, so the parameter's
                                          choice table and the loader agree at compile time;
``Core/data/CREDITS-wavetables.md``       where every shipped table comes from.

**What goes into the file.** Not the samples: the Fourier coefficients of every frame, which is
exactly what ``WaveTable::buildFromHarmonics()`` consumes. A table built from 32 partials then costs
32 coefficients a frame instead of 2048 samples, and the C++ side needs no analysis at load. Each
frame is truncated at the first harmonic below ``--floor`` dB of that frame's loudest one and
quantised to int16 against one scale per frame; ``--check`` measures what that costs before the
file is written.

Usage::

    python Tools/wt_pack.py                 # write the pack, the .inl and the credits
    python Tools/wt_pack.py --check         # also measure the round trip and the alternatives
"""

import argparse
import json
import math
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wt_select import (LIBRARY, SELECTION_JSON, TOP_HARMONICS, MAX_FRAMES, LEVELS,
                       read_wav_mono, frame_spectra, level_length, provenance)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PACK = os.path.join(ROOT, "Core", "data", "library.phoswt")
INL = os.path.join(ROOT, "Core", "include", "phos", "WaveTableList.inl")
CREDITS = os.path.join(ROOT, "Core", "data", "CREDITS-wavetables.md")

MAGIC = b"PHOSWT1\0"
END = b"PHOSWTE1"
NAME_BYTES = 32
ID_BYTES = 64
ALIGN = 32

# Which built-in table stands in for a library table when the pack is missing. A lane's tables get
# the built-ins of that lane's character in turn, so a set saved with a library table still plays
# something of the right kind on a machine that has no pack: Vocal/Glass/Formant Saw are the three
# pad tables written in code, Sync and PWM the two hard ones. The drone lane (20.09.2026) falls back
# to Vocal and Glass, the same steady, formant-bearing pair the pad itself falls back to first --
# there is no organ built-in, and a drone silently sounding like a hushed pad on a machine with no
# library is closer to its own character than Sync or PWM would be.
FALLBACKS = {"pad": (1, 2, 5), "lead": (4, 3), "arp": (3, 4), "drone": (1, 2)}


def band_of(h):
    """The octave band a 1-based harmonic number belongs to: 1 | 2,3 | 4..7 | 8..15 | ..."""
    return int(h).bit_length() - 1


def bands_for(top):
    """How many octave bands a frame of *top* harmonics needs."""
    return 0 if top <= 0 else band_of(top) + 1


def quantise(coeffs, floor_db, dtype):
    """One frame's coefficients as they go into the file.

    Returns ``(harmonics, scales, payload_bytes, reconstructed)``. Harmonics past the last one above
    ``floor_db`` relative to the frame's loudest are dropped; what is left is written as float32
    (``dtype`` 0) or as int16 pairs against **one scale per octave band** (``dtype`` 1).

    The bands are what make int16 worth having. With a single scale a frame the quantisation error
    of every harmonic is the same absolute size, so 512 of them sum to a noise floor 30 dB above the
    error of any one -- measured, that is 82.7 dB of round trip for this selection. A spectrum
    decays, so giving each octave of harmonics its own scale costs at most ten floats a frame and
    puts each band's error 96 dB under *that band's* own peak; the measured round trip rises to
    about 104 dB, which is better than the 16-bit PCM the `Classic/` files are stored in to begin
    with. Octave bands rather than uniform ones because that is how a harmonic spectrum decays
    (and how every perceptual band scheme from Zwicker onwards splits the top of the range).
    """
    mag = np.abs(coeffs)
    peak = float(mag.max()) if len(mag) else 0.0
    if peak <= 0.0:
        return 0, [], b"", np.zeros_like(coeffs)
    above = np.nonzero(mag >= peak * (10.0 ** (floor_db / 20.0)))[0]
    top = int(above[-1]) + 1 if len(above) else 0
    c = coeffs[:top]
    if dtype == 0:
        pay = np.empty(2 * top, dtype="<f4")
        pay[0::2] = c.real
        pay[1::2] = c.imag
        back = np.zeros_like(coeffs)
        back[:top] = pay[0::2].astype(np.float64) + 1j * pay[1::2].astype(np.float64)
        return top, [], pay.tobytes(), back
    nb = bands_for(top)
    idx = np.array([band_of(h) for h in range(1, top + 1)], dtype=np.int64)
    scales = []
    for b in range(nb):
        sel = idx == b
        big = max(float(np.abs(c.real[sel]).max(initial=0.0)), float(np.abs(c.imag[sel]).max(initial=0.0)))
        scales.append(float(np.float32(big / 32767.0)) if big > 0.0 else 0.0)
    s = np.array([scales[b] for b in idx], dtype=np.float64)
    safe = np.where(s > 0.0, s, 1.0)
    q = np.empty(2 * top, dtype="<i2")
    q[0::2] = np.clip(np.round(c.real / safe), -32767, 32767)
    q[1::2] = np.clip(np.round(c.imag / safe), -32767, 32767)
    back = np.zeros_like(coeffs)
    back[:top] = (q[0::2].astype(np.float64) + 1j * q[1::2].astype(np.float64)) * s
    payload = np.array(scales, dtype="<f4").tobytes() + q.tobytes()
    return top, scales, payload, back


def load_selection():
    """The chosen tables with their coefficients, in the order the parameter indices follow."""
    sel = json.load(open(SELECTION_JSON, encoding="utf-8"))["tables"]
    out = []
    for lane in ("pad", "lead", "arp", "drone"):
        n = 0
        for row in sel:
            if row["lane"] != lane:
                continue
            path = os.path.join(LIBRARY, row["id"].replace("/", os.sep) + ".wav")
            samples, cycle_len = read_wav_mono(path)
            coeffs = frame_spectra(samples, cycle_len)
            row = dict(row)
            row["coeffs"] = coeffs
            row["wav_bytes"] = os.path.getsize(path)
            row["fallback"] = FALLBACKS[lane][n % len(FALLBACKS[lane])]
            n += 1
            out.append(row)
    return out


def pack(rows, floor_db, dtype, note):
    """The bytes of the `.phoswt` file."""
    header = ("generator=Tools/wt_pack.py\n"
              "library=Noctuary Library/Wavetables\n"
              "floor_db=%g\ndtype=%d\ntables=%d\nnote=%s\n" % (floor_db, dtype, len(rows), note))
    head = bytearray()
    head += MAGIC
    head += struct.pack("<III", 1, len(rows), len(header))
    head += header.encode("ascii")
    while len(head) % ALIGN:
        head += b"\0"
    body = bytearray(head)
    err_num, err_den = 0.0, 0.0
    for row in rows:
        coeffs = row["coeffs"]
        frames = coeffs.shape[0]
        frame_blocks, hmax = [], 0
        for k in range(frames):
            top, scales, payload, back = quantise(coeffs[k], floor_db, dtype)
            hmax = max(hmax, top)
            frame_blocks.append(struct.pack("<HH", top, len(scales)) + payload)
            err_num += float((np.abs(back - coeffs[k]) ** 2).sum())
            err_den += float((np.abs(coeffs[k]) ** 2).sum())
        name = row["name"].encode("ascii", "replace")[:NAME_BYTES - 1]
        ident = row["id"].encode("ascii", "replace")[:ID_BYTES - 1]
        body += name + b"\0" * (NAME_BYTES - len(name))
        body += ident + b"\0" * (ID_BYTES - len(ident))
        body += struct.pack("<HHB3s", frames, hmax, dtype, b"\0\0\0")
        for blk in frame_blocks:
            body += blk
    body += END
    snr = 10.0 * math.log10(err_den / err_num) if err_num > 0.0 else float("inf")
    return bytes(body), snr


def write_inl(rows):
    """The generated list the parameter table and the loader both read.

    Included twice: once without ``PHOS_WT`` defined, for the count alone, and once with it defined
    to build a list. ``#pragma once`` would break the second use, so there is none.
    """
    lines = [
        "/**",
        " * @file WaveTableList.inl",
        " * @brief The library tables this build ships -- generated by `Tools/wt_pack.py`, do not edit.",
        " *",
        " * Included twice: bare, for `PHOS_WT_LIBRARY_COUNT`, and with `PHOS_WT(index, name, id,",
        " * lane, fallback)` defined, to build a list. The order is the order the `table` parameter's",
        " * indices follow after the six built-in tables, and it must not be reshuffled: a `.phosset`",
        " * stores the index.",
        " */",
        "#ifndef PHOS_WT_LIBRARY_COUNT",
        "#define PHOS_WT_LIBRARY_COUNT %d" % len(rows),
        "#endif",
        "",
        "#ifdef PHOS_WT",
    ]
    for i, row in enumerate(rows):
        lines.append('PHOS_WT(%d, "%s", "%s", WaveTableLane::%s, %d)'
                     % (i, row["name"], row["id"], row["lane"].capitalize(), row["fallback"]))
    lines += ["#endif", ""]
    open(INL, "w", encoding="ascii", newline="\r\n").write("\n".join(lines))


def write_credits(rows):
    """Where every shipped table comes from, per the sidecar of the file it was made from."""
    seen = {}
    for row in rows:
        js = os.path.join(LIBRARY, row["id"].replace("/", os.sep) + ".json")
        meta = json.load(open(js, encoding="utf-8"))
        kind, src = provenance(js)
        seen.setdefault(kind, []).append((row, meta, src))
    text = [
        "# Wavetables shipped with Phosphene",
        "",
        "`Core/data/library.phoswt` holds %d tables, chosen by measurement from the %s wavetable" % (len(rows), "2191-table"),
        "library of the sibling project Noctuary (`G:\\Tools\\VRAudio\\AmbientSynth\\Library\\Wavetables`)",
        "by `Tools/wt_select.py`. Frames of 2048 samples; the pack carries each frame's Fourier",
        "coefficients rather than its samples (`Core/include/phos/WaveTableFile.h`).",
        "",
        "## AKWF -- Adventure Kid Waveforms, Kristoffer Ekstrand",
        "",
        "<https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE> (8de90bf, 2025-12-04)",
        "CC0 1.0 Universal. Single cycles of 600 samples, ordered into morph tables and aligned by",
        "Noctuary's `Tools/WavetableLib/build_classic.py`.",
        "",
        "## WaveEdit Online -- the banks of the WaveEdit users (Synthesis Technology E352/E370)",
        "",
        "<https://github.com/smpldsnds/wavedit-online> (1a8d80f, 2023-10-07)",
        "CC0 1.0 Universal. Order and phases as in the original, cycles brought from 256 to 2048",
        "samples by the same script.",
        "",
        "CC0 asks for no attribution; it stands here all the same, as it does in Noctuary's own",
        "`Library/Wavetables/Classic/CREDITS-classic.md`.",
        "",
        "## Procedurally generated -- Noctuary `HarmonicGen` / `AmbientGen`",
        "",
        "`Tools/WavetableGen` writes these from a recipe and a seed: partial envelopes, chord and",
        "formant families, optimal-transport morphs between spectra. No external material of any",
        "kind enters them; the sidecar of such a table names a generator and a seed and no source.",
        "",
        "## Measured families -- `ambient_sampled` and its relatives",
        "",
        "These are spectral analyses of the user's own material: single notes generated with Stable",
        "Audio 3 medium by the user's own pipeline (`G:\\Tools\\VRAudio\\StableAudio3`) under the",
        "Stability Community License, which assigns the outputs to the user. What ships here is not",
        "audio but the harmonic envelopes measured from it -- the same relation a wavetable has to",
        "the instrument it was drawn from. The Stability Community License is revenue-capped:",
        "commercial use is free below one million US dollars of annual revenue.",
        "",
        "## The tables",
        "",
        "| # | Table | Lane | Library id | Provenance |",
        "|---|---|---|---|---|",
    ]
    for i, row in enumerate(rows):
        js = os.path.join(LIBRARY, row["id"].replace("/", os.sep) + ".json")
        kind, src = provenance(js)
        label = {"cc0": "CC0: " + src, "procedural": "generated: " + src,
                 "measured": "measured from the user's own SA3 material"}.get(kind, kind)
        text.append("| %d | %s | %s | `%s` | %s |" % (6 + i, row["name"], row["lane"], row["id"], label))
    text.append("")
    open(CREDITS, "w", encoding="utf-8", newline="\r\n").write("\n".join(text))


def check(rows):
    """The numbers the format decision rests on: size and accuracy of every alternative."""
    wav = sum(r["wav_bytes"] for r in rows)
    print("  the .wav files as they lie          %9d bytes" % wav)
    for dtype, label in ((0, "float32 coefficients"), (1, "int16 coefficients  ")):
        for floor_db in (-200.0, -140.0, -110.0, -90.0):
            blob, snr = pack(rows, floor_db, dtype, "check")
            print("  %s floor %6.0f dB %9d bytes   round trip %6.1f dB"
                  % (label, floor_db, len(blob), snr))
    frame_bytes = sum(level_length(l) + 3 for l in range(LEVELS)) * 4
    frames = sum(r["coeffs"].shape[0] for r in rows)
    print("  after the mip levels are expanded   %9d bytes (%d frames x %d)"
          % (frames * frame_bytes, frames, frame_bytes))


def reference(rows):
    """Prints the C++ literal block the self test measures against.

    The numbers come from the **source** `.wav` files and this file's own model of the C++ level
    build and Catmull-Rom read -- never from the pack. So when the self test decodes the pack and
    reproduces them, two independent paths have agreed: Python from the WAV, and C++ from the
    packed coefficients through `buildFromHarmonics()`.
    """
    from wt_select import alias_db, TOP_HARMONICS as _TH
    target_rms = 0.35355339
    print("// Generated by `python Tools/wt_pack.py --reference`: measured in Python from the source")
    print("// .wav files of Noctuary's library, not from the pack the test reads.")
    print("const LibraryRef kLibraryRef[] = {")
    for row in rows:
        c = row["coeffs"]
        loudest = max(math.sqrt(0.5 * float((np.abs(c[k]) ** 2).sum())) for k in range(c.shape[0]))
        gain = target_rms / loudest
        rms0 = gain * math.sqrt(0.5 * float((np.abs(c[0]) ** 2).sum()))
        a5 = alias_db(c[0], 523.2511)
        a6 = alias_db(c[0], 1046.502)
        print('    { "%s", %d, %.6f, %.2f, %.2f },' % (row["name"], c.shape[0], rms0, a5, a6))
    print("};")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--floor", type=float, default=-110.0,
                    help="drop a frame's harmonics below this, in dB under its loudest (default -110)")
    ap.add_argument("--dtype", type=int, default=1, choices=(0, 1),
                    help="0 = float32 coefficients, 1 = int16 with one scale a frame (default)")
    ap.add_argument("--check", action="store_true", help="measure the alternatives, write nothing")
    ap.add_argument("--reference", action="store_true",
                    help="print the C++ literal block the self test measures against, write nothing")
    args = ap.parse_args()

    t0 = time.time()
    rows = load_selection()
    print("read %d tables in %.2f s" % (len(rows), time.time() - t0))
    if args.check:
        check(rows)
        return 0
    if args.reference:
        reference(rows)
        return 0
    blob, snr = pack(rows, args.floor, args.dtype,
                     "chosen by Tools/wt_select.py from Noctuary's library")
    os.makedirs(os.path.dirname(PACK), exist_ok=True)
    open(PACK, "wb").write(blob)
    write_inl(rows)
    write_credits(rows)
    print("%s  %d bytes, round trip %.1f dB" % (PACK, len(blob), snr))
    print("%s\n%s" % (INL, CREDITS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
