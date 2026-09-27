"""Builds the voice pack `Core/data/voices.phosvx` and its credits `Core/data/CREDITS-voices.md`.

Round "fx-psychedelia" (19.09.2026). The phrases are listed in `Tools/voice_selection.json` (file, the
phrase's first and last word from a word-level transcript, where its last word starts and where its
first word ends); this script cuts them out of the AmbientSynth archive, cleans them and packs them.
The byte layout is normative in `Core/include/phos/Vocal.h`.

Usage:  python Tools/voice_pack.py [archive folder]
        (default G:\\Tools\\VRAudio\\AmbientSynth\\Library\\Archive; needs ffmpeg on the PATH)

Per phrase:
  * cut from 80 ms before the first word to 150 ms after the last (transcript times are word-level
    estimates and clip consonants when taken literally),
  * trim silence at both ends (10 ms frames more than 45 dB under the loudest frame, 30 ms kept),
  * level: the RMS of the frames within 30 dB of the loudest to -16 dBFS, the peak then held under
    -1 dBFS -- phrases from a 1960s capsule and from a 1950s film should arrive equally loud,
  * 8 ms raised-cosine fades,
  * 16 kHz mono, 4-bit IMA ADPCM (a quarter of 16-bit PCM; Vocal.h says why 16 kHz is enough).
"""
import json
import os
import re
import struct
import subprocess
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELECTION = os.path.join(ROOT, "Tools", "voice_selection.json")
PACK = os.path.join(ROOT, "Core", "data", "voices.phosvx")
CREDITS = os.path.join(ROOT, "Core", "data", "CREDITS-voices.md")
RATE = 16000

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "quiet", "-i", path, "-ac", "1", "-ar", str(RATE), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def ima_encode(pcm):
    """Standard IMA ADPCM encoder; the decoder in Vocal.cpp is its exact inverse step for step."""
    pred, index = int(pcm[0]), 0
    pred0 = pred
    nibbles = []
    for x in pcm:
        step = STEP[index]
        diff = int(x) - pred
        nib = 0
        if diff < 0:
            nib = 8
            diff = -diff
        if diff >= step:
            nib |= 4
            diff -= step
        if diff >= step >> 1:
            nib |= 2
            diff -= step >> 1
        if diff >= step >> 2:
            nib |= 1
        # Reconstruct exactly as the decoder does, so the encoder tracks the decoder's state.
        d = step >> 3
        if nib & 4:
            d += step
        if nib & 2:
            d += step >> 1
        if nib & 1:
            d += step >> 2
        pred = pred - d if nib & 8 else pred + d
        pred = max(-32768, min(32767, pred))
        index = max(0, min(88, index + INDEX[nib & 7]))
        nibbles.append(nib)
    if len(nibbles) & 1:
        nibbles.append(0)
    data = bytes((nibbles[i] & 0xF) | ((nibbles[i + 1] & 0xF) << 4) for i in range(0, len(nibbles), 2))
    return pred0, 0, data


def clean(x, a, b):
    """The phrase between a and b seconds: cut, trimmed, levelled, faded. Returns samples and the offset."""
    s0 = max(0, int((a - 0.08) * RATE))
    s1 = min(len(x), int((b + 0.15) * RATE))
    y = x[s0:s1].copy()
    y -= y.mean()
    frame = RATE // 100
    n = len(y) // frame
    e = np.array([np.mean(y[i * frame:(i + 1) * frame] ** 2) + 1e-12 for i in range(n)])
    db = 10 * np.log10(e / e.max())
    on = np.where(db > -45.0)[0]
    lo = max(0, on[0] * frame - int(0.03 * RATE))
    hi = min(len(y), (on[-1] + 1) * frame + int(0.03 * RATE))
    y = y[lo:hi]
    active = [i for i in range(n) if db[i] > -30.0]
    rms = np.sqrt(np.mean(np.concatenate([x[s0 + i * frame:s0 + (i + 1) * frame] for i in active]) ** 2))
    y *= 10 ** (-16 / 20) / max(rms, 1e-9)
    peak = np.max(np.abs(y))
    if peak > 10 ** (-1 / 20):
        y *= 10 ** (-1 / 20) / peak
    f = int(0.008 * RATE)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(f) / f)
    y[:f] *= ramp
    y[-f:] *= ramp[::-1]
    return y, (s0 + lo) / RATE


def nasa_urls(sources_md):
    """Maps a NASA file name to its source URL and page, read from the archive's SOURCES.md."""
    urls = {}
    lines = open(sources_md, encoding="utf-8").read().splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"- `(.+?)` -- ", line)
        if m and i + 1 < len(lines):
            u = re.match(r"\s+(\S+)\s+\(from (\S+)\)", lines[i + 1])
            if u:
                urls[m.group(1)] = (u.group(1), u.group(2))
    return urls


LOC_PACKS = {
    "Screening-Room": ("national-screening-room", "https://citizen-dj.labs.loc.gov/loc-national-screening-room/use/",
                       "A subset of films from the National Screening Room that were identified to have been created "
                       "by the U.S. government, thus in the public domain."),
    "Tony-Schwartz": ("tony-schwartz", "https://citizen-dj.labs.loc.gov/loc-tony-schwartz/use/",
                      "In 2007 Tony Schwartz's entire body of work was acquired by the Library of Congress, which makes "
                      "his recordings available for reuse; Citizen DJ excludes the ones with embedded material he did not own."),
    "Edison": ("edison", "https://citizen-dj.labs.loc.gov/loc-edison/use/",
               "All recordings made by the companies of Thomas A. Edison between 1890 and 1929 are in the public domain "
               "because the assets of Edison Records were transferred to the National Park Service, a federal agency, in the 1950s."),
}


def main():
    archive = sys.argv[1] if len(sys.argv) > 1 else r"G:\Tools\VRAudio\AmbientSynth\Library\Archive"
    rows = json.load(open(SELECTION, encoding="utf-8"))
    urls = nasa_urls(os.path.join(archive, "SOURCES.md"))
    body = bytearray()
    total = 0.0
    credits = []
    cache = {}
    for r in rows:
        path = os.path.join(archive, r["file"])
        if path not in cache:
            cache[path] = load(path)
        y, t0 = clean(cache[path], r["start"], r["end"])
        pcm = np.clip(np.round(y * 32767), -32768, 32767).astype(np.int64)
        throw_at = max(0, min(len(pcm), int(round((r["last_word"] - t0) * RATE))))
        chop_start = max(0, int(round((r["start"] - 0.02 - t0) * RATE)))
        chop_len = max(1, min(len(pcm) - chop_start, int(round((r["first_word_end"] + 0.03 - r["start"]) * RATE))))
        pred, index, data = ima_encode(pcm)
        name = os.path.splitext(os.path.basename(r["file"]))[0]
        nb = name.encode("utf-8")[:255]
        body += struct.pack("<BB", r["category"], len(nb)) + nb
        body += struct.pack("<IIIIhBBI", len(pcm), throw_at, chop_start, chop_len, pred, index, 0, len(data)) + data
        total += len(pcm) / RATE
        credits.append((r, name, len(pcm) / RATE))
    blob = b"PHOSVX01" + struct.pack("<II", len(rows), RATE) + bytes(body) + b"PHOSVXE1"
    open(PACK, "wb").write(blob)

    out = []
    out.append("# Voices -- sources and credits")
    out.append("")
    out.append("`Core/data/voices.phosvx` holds %d spoken phrases, %.1f s in all, %d bytes (16 kHz mono, 4-bit IMA ADPCM),"
               % (len(rows), total, len(blob)))
    out.append("built by `Tools/voice_pack.py` from `Tools/voice_selection.json`: 31 phrases chosen by hand in round")
    out.append("\"fx-psychedelia\" (19.09.2026), and since 27.09.2026 every further spoken phrase of NASA's historical")
    out.append("recordings and of the radio series Quiet, Please (the user: \"Zumindest NASA und Radio sollten wir komplett")
    out.append("nehmen\"), cut from a word-level transcript (`Tools/voice_transcribe.py`, `Tools/voice_select.py`).")
    out.append("Every phrase was cut from a file of the AmbientSynth archive (`AmbientSynth/Library/Archive`, whose")
    out.append("`SOURCES.md` records where each file came from); the rights below are the sources' own statements.")
    out.append("")
    out.append("## NASA (public domain)")
    out.append("")
    out.append("Works of the United States government, not subject to copyright (NASA Media Usage Guidelines). NASA's")
    out.append("guidelines ask that its material not be used to imply endorsement: Phosphene uses these phrases as")
    out.append("sound, and nothing in it claims NASA's approval. Credit: NASA.")
    out.append("")
    for r, name, sec in credits:
        if r["category"] != 0:
            continue
        u = urls.get(os.path.basename(r["file"]), ("", ""))
        out.append("- \"%s\" (%.1f s) -- from `%s`, %s (listed on %s)" % (r["text"], sec, os.path.basename(r["file"]), u[0], u[1]))
    out.append("")
    out.append("## Library of Congress, Citizen DJ (free to use and reuse)")
    out.append("")
    out.append("Suggested credit, which Phosphene follows: **Citizen DJ Project, Library of Congress.**")
    out.append("")
    for key, (pack, url, rights) in LOC_PACKS.items():
        items = [(r, name, sec) for r, name, sec in credits if r["category"] == 1 and ("\\" + key + "\\") in r["file"]]
        if not items:
            continue
        out.append("### Pack `%s` (%s)" % (pack, url))
        out.append("")
        out.append("Rights, in the Library's words: " + rights)
        out.append("")
        for r, name, sec in items:
            out.append("- \"%s\" (%.1f s) -- clip `%s`" % (r["text"], sec, name))
        out.append("")
    radio = [(r, name, sec) for r, name, sec in credits if r["file"].startswith("Radio\\")]
    if radio:
        out.append("## Quiet, Please (radio series, 1947-49)")
        out.append("")
        out.append("Internet Archive item https://archive.org/details/Quiet_Please, tagged public domain by its uploader.")
        out.append("Wyllis Cooper (writer, producer) died in 1955: his scripts are in the public domain in the EU since 2026;")
        out.append("the recordings' neighbouring rights in the EU expired fifty years after publication. The US status of the")
        out.append("broadcasts has not been established; the user decided to ship them (27.09.2026).")
        out.append("")
        for r, name, sec in radio:
            out.append("- \"%s\" (%.1f s) -- clip `%s`" % (r["text"], sec, name))
        out.append("")
    out.append("## What was left out, and why")
    out.append("")
    out.append("- **LoC/Joe-Smith** (250 interview clips): reusable with attribution, but the speakers are identifiable")
    out.append("  figures of the music business, many alive; their voices inside generated tracks could read as an")
    out.append("  endorsement. Not used.")
    out.append("- **LoC/Variety-Stage** and **LoC/Edison**: public domain, but comic sketches and songs whose lines do")
    out.append("  not work as spoken word over a psytrance groove. The one Edison line tried (\"the energy of that")
    out.append("  machine\") failed the check below: the cylinder is too noisy for word timestamps.")
    out.append("")
    out.append("**The check.** The 31 phrases of 19.09.2026 were decoded again by an independent decoder and transcribed")
    out.append("once more (faster-whisper, base model); the phrases added on 27.09.2026 carry the words of the large-v3")
    out.append("transcript they were cut from.")
    out.append("- **NASA/Beyond, NASA/Historical/Beeps, Missions, NASA/Sonification**: recordings and sonifications, not")
    out.append("  speech -- they play on the Field track (`CREDITS-field.md`). The LCROSS song is third-party music.")
    out.append("- **The EchoThief impulse responses and everything under AmbientSynth's own sample folders** are outside")
    out.append("  this archive and were not considered.")
    out.append("")
    open(CREDITS, "w", encoding="utf-8", newline="\r\n").write("\n".join(out) + "\n")
    print("%d phrases, %.1f s, %d bytes -> %s" % (len(rows), total, len(blob), PACK))


if __name__ == "__main__":
    main()
