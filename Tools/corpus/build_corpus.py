"""Builds the melodic statistics of Phosphene from a local MIDI corpus.

Reads the psytrance MIDI loops under a root folder, assigns each file a role from its path (acid,
lead, arp), estimates its key, extracts the top voice on a sixteenth grid and counts:

  * pitch transitions of order 0, 1 and 2 over the interval to the tonic in semitones
    (-12 .. +24, so a Phrygian flat second stays a flat second), for a variable-order model;
  * rhythm: the probability of an onset on each of the 16 steps given whether the previous step had
    one, and note lengths in steps;
  * articulation: accent (a velocity clearly above the loop's median) and slide (a note overlapping
    the next) per step;
  * the ambitus of each line.

The output is Core/src/CorpusTables.cpp: counts only -- statistics of scale-degree successions and
step positions, from which no loop can be read back. The MIDI files themselves never leave this
machine. Nothing is written for a role with too little data.

Usage:
    python Tools/corpus/build_corpus.py --root M:/Midi --out Core/src/CorpusTables.cpp [--report]
    python Tools/corpus/build_corpus.py --root M:/Midi --memorisation out/render.mid

The second form measures memorisation: every bar of the acid, lead and arp tracks of a Phosphene MIDI
export is compared with every bar of the corpus of its role, and the share of generated bars with an
identical bar in the corpus is printed (bars with fewer than three distinct pitches are left out,
since a bar of repeated roots matches half the corpus by necessity).
"""
import argparse
import math
import os
import re
import struct
import sys
from collections import Counter, defaultdict

PACKS = ["EMP - Psytrance Bundle", "PSYTRANCE MIDI BUNDLE", "TOTAL_MIDI_PSY_TRANCE"]
ROLES = ["acid", "lead", "arp"]
REL_MIN, REL_MAX = -12, 24
ALPHABET = REL_MAX - REL_MIN + 1

# Krumhansl-Kessler minor key profile (Krumhansl 1990).
KK_MINOR = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]
NOTE_NAMES = {"c": 0, "c#": 1, "db": 1, "d": 2, "d#": 3, "eb": 3, "e": 4, "f": 5, "f#": 6, "gb": 6,
              "g": 7, "g#": 8, "ab": 8, "a": 9, "a#": 10, "bb": 10, "b": 11}


# ------------------------------------------------------------------------------------------------ MIDI

def read_vlq(d, i):
    v = 0
    while True:
        b = d[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, i


def read_midi_tracks(path):
    """Returns (ppq, tracks): one entry per MTrk chunk, keeping what the chunk says about itself.

    Each track is a dict with ``notes`` (the melodic notes, as :func:`read_midi` yields them),
    ``name`` (the first track-name meta event, 0x03), ``programs`` (the GM program numbers the track
    selects) , ``channels`` and ``drum_notes`` (note-ons on channel 10, which are percussion and are
    not in ``notes``).

    **Why per track.** A third of the VORTEX bundle's files are not loops but whole arrangements --
    bass, lead, pad and drums side by side in one file. Merging them and taking the top voice, which
    is what the file-level reader does, produces a line that no instrument ever played. The track is
    the unit that has a role, and the track name and the program change are two label sources that
    sit inside the file and that a detector reading only the path never sees.

    The reader follows running status (MIDI 1.0 Detailed Specification, section 2.1.2): a channel
    event may omit its status byte and inherit the last one. Three rules keep a malformed track from
    corrupting the file's note list rather than aborting the whole read, each of them a case seen in
    the bought packs and covered by ``test_build_corpus.py``:

    * **A System Exclusive message cancels running status** (same section). Without that, the bytes
      after an F0 message are decoded under the status that preceded it and the file gains notes
      nobody wrote.
    * **A data byte whose status was never established ends the track.** 26 files of the Star Samples
      super pack ("DMS ... Single Patches") are synthesiser patch dumps: one SysEx and then trailing
      bytes. There is no status to decode them under, and no amount of guessing makes one; the track
      stops there and the rest of the file is kept. The old reader masked ``None`` and raised
      ``TypeError``, which threw away every track of the file including the good ones.
    * **An event running past the end of its chunk ends the track**, instead of reading the header
      bytes of the next MTrk as note data.
    """
    d = open(path, "rb").read()
    if d[:4] != b"MThd":
        return None, []
    _, ntrk, div = struct.unpack(">HHH", d[8:14])
    if div & 0x8000:
        return None, []
    i = 8 + struct.unpack(">I", d[4:8])[0]
    tracks = []
    for _ in range(ntrk):
        if d[i:i + 4] != b"MTrk":
            break
        ln = struct.unpack(">I", d[i + 4:i + 8])[0]
        j, end, t, run = i + 8, i + 8 + ln, 0, None
        open_notes = {}
        tr = {"notes": [], "name": "", "programs": [], "channels": set(), "drum_notes": 0}
        while j < end:
            dt, j = read_vlq(d, j)
            t += dt
            st = d[j]
            if st == 0xFF:              # meta event: SMF-only, carries no status and cancels none
                ty = d[j + 1] if j + 1 < end else 0
                l, j2 = read_vlq(d, j + 2)
                if ty == 0x03 and not tr["name"]:
                    tr["name"] = d[j2:j2 + l].decode("latin1", "replace")
                j = j2 + l
                continue
            if st in (0xF0, 0xF7):
                l, j2 = read_vlq(d, j + 1)
                j = j2 + l
                run = None              # SysEx cancels running status
                continue
            if st & 0x80:
                if st >= 0xF0:          # other system messages have no place in an SMF track
                    break
                run = st
                j += 1
            if run is None:
                break                   # a data byte no status ever covered: undecodable from here
            hi, ch = run & 0xF0, run & 0x0F
            n = 1 if hi in (0xC0, 0xD0) else 2
            if j + n > end:
                break                   # the event is cut off by the end of the chunk
            a = d[j]
            b = d[j + 1] if n == 2 else 0
            j += n
            if hi == 0xC0:
                tr["programs"].append(a)
            if ch == 9:
                if hi == 0x90 and b > 0:
                    tr["drum_notes"] += 1
                continue
            if hi in (0x80, 0x90):
                tr["channels"].add(ch)
            if hi == 0x90 and b > 0:
                open_notes.setdefault(a, []).append((t, b))
            elif hi == 0x80 or (hi == 0x90 and b == 0):
                if open_notes.get(a):
                    s, v = open_notes[a].pop(0)
                    tr["notes"].append((s, t, a, v))
        tr["channels"] = sorted(tr["channels"])
        tracks.append(tr)
        i = end
    return div, tracks


def read_midi(path):
    """Returns (ppq, notes) with notes as (start_tick, end_tick, pitch, velocity), drums excluded.

    The file-level view: every track's notes merged and sorted. It is what the stage-A tables and the
    stage-B tokens are built from, and it stays the definition it always was; :func:`read_midi_tracks`
    is the same parse with the tracks kept apart.
    """
    div, tracks = read_midi_tracks(path)
    if not div:
        return div, []
    notes = []
    for tr in tracks:
        notes += tr["notes"]
    return div, sorted(notes)


# ------------------------------------------------------------------------------------------------ analysis

def role_of(path):
    # Only the file name and its own folder: pack names mix roles ("Riffs & Arps Midi Pack").
    parts = path.lower().replace("\\", "/").split("/")
    p = "/".join(parts[-2:])
    if any(w in p for w in ("bass", "kick", "drum", "perc", "step and hold", "pads", "pad.mid", "pad ", "stab", "chord")):
        return None
    name = parts[-1]
    if "arp" in p or name.startswith("grid"):
        return "arp"
    if "lead" in p or "melod" in p:
        return "lead"
    if "acid" in p or "303" in p or "riff" in p or "freaky spores" in p or "/seq" in p or "sequence" in p:
        return "acid"
    return None


def key_from_name(path):
    name = os.path.splitext(os.path.basename(path))[0]
    m = re.findall(r"(?:^|[ _\-])([A-Ga-g](?:#|b)?)(?:m|min|minor)?(?=$|[ _\-])", name)
    if m:
        return NOTE_NAMES.get(m[-1].lower())
    folder = os.path.basename(os.path.dirname(os.path.dirname(path)))
    m = re.findall(r"_([A-G](?:#|b)?)m_", folder + "_")
    return NOTE_NAMES.get(m[-1].lower()) if m else None


def key_profile_score(hist, tonic):
    xs = [hist[(tonic + i) % 12] for i in range(12)]
    mx, my = sum(xs) / 12, sum(KK_MINOR) / 12
    num = sum((x - mx) * (y - my) for x, y in zip(xs, KK_MINOR))
    den = math.sqrt(sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in KK_MINOR)) or 1.0
    return num / den


def estimate_key(notes, path):
    hist = [0.0] * 12
    for s, e, p, v in notes:
        hist[p % 12] += max(1, e - s)
    scores = [key_profile_score(hist, k) for k in range(12)]
    best = max(range(12), key=lambda k: scores[k])
    named = key_from_name(path)
    if named is not None and scores[named] >= 0.9 * scores[best]:
        return named, "name"
    return best, "profile"


def top_line(ppq, notes):
    """Top voice on a sixteenth grid: list of steps, each None or (pitch, velocity, length_steps, slide)."""
    step = ppq / 4.0
    if not notes:
        return []
    total = int(math.ceil(max(e for _, e, _, _ in notes) / step))
    bars = max(1, int(round(total / 16.0)))
    steps = [None] * (bars * 16)
    by_step = defaultdict(list)
    for s, e, p, v in notes:
        k = int(round(s / step))
        if 0 <= k < len(steps):
            by_step[k].append((p, v, s, e))
    onsets = sorted(by_step)
    for idx, k in enumerate(onsets):
        p, v, s, e = max(by_step[k])
        nxt = onsets[idx + 1] if idx + 1 < len(onsets) else None
        length = max(1, int(round((e - s) / step)))
        slide = nxt is not None and e > nxt * step + 1
        steps[k] = (p, v, length, slide)
    return steps


def analyse(root, report):
    stats = {r: {"uni": Counter(), "bi": Counter(), "tri": Counter(), "onset": [[[0, 0], [0, 0]] for _ in range(16)],
                 "accent": [[0, 0] for _ in range(16)], "slide": [[0, 0] for _ in range(16)],
                 "length": [Counter() for _ in range(16)], "ambitus": Counter(), "files": 0, "bars": []} for r in ROLES}
    keysrc = Counter()
    chords = {"files": 0, "first": Counter(), "trans": Counter()}
    stats["chords"] = chords
    for pack in PACKS:
        for dirpath, _, files in os.walk(os.path.join(root, pack)):
            for f in files:
                if not f.lower().endswith(".mid"):
                    continue
                path = os.path.join(dirpath, f)
                role = role_of(path)
                if role is None:
                    continue
                try:
                    ppq, notes = read_midi(path)
                except (IndexError, struct.error):
                    continue
                if not ppq or len(notes) < 4:
                    continue
                tonic, src = estimate_key(notes, path)
                keysrc[src] += 1
                line = top_line(ppq, notes)
                if not line:
                    continue
                st = stats[role]
                pitches = [s[0] for s in line if s]
                # A loop of one or two pitches is a rhythm, not a melody: it counts for the onsets only.
                melodic = len(set(pitches)) >= 3
                if not melodic:
                    st["rhythmOnly"] = st.get("rhythmOnly", 0) + 1
                    prev_on = 0
                    for k, s in enumerate(line):
                        on = 1 if s else 0
                        st["onset"][k % 16][prev_on][on] += 1
                        prev_on = on
                    continue
                st["files"] += 1
                median = sorted(pitches)[len(pitches) // 2]
                ref = median - ((median - tonic) % 12)          # the tonic at or below the median
                vels = sorted(s[1] for s in line if s)
                accent_thr = max(vels[len(vels) // 2] + 10, 1)
                rels = []
                prev_on = 0
                for k, s in enumerate(line):
                    pos = k % 16
                    on = 1 if s else 0
                    st["onset"][pos][prev_on][on] += 1
                    prev_on = on
                    if s:
                        rel = max(REL_MIN, min(REL_MAX, s[0] - ref))
                        rels.append(rel)
                        st["accent"][pos][1 if s[1] >= accent_thr else 0] += 1
                        st["slide"][pos][1 if s[3] else 0] += 1
                        st["length"][pos][min(s[2], 8)] += 1
                # pitch n-grams, cyclic over the loop (a loop repeats)
                n = len(rels)
                for i in range(n):
                    a, b, c = rels[i - 2], rels[i - 1], rels[i]
                    st["uni"][c] += 1
                    st["bi"][(b, c)] += 1
                    st["tri"][(a, b, c)] += 1
                st["ambitus"][min(max(rels) - min(rels), 36)] += 1
                for b in range(len(line) // 16):
                    bar = tuple((s[0] - ref) if s else None for s in line[b * 16:(b + 1) * 16])
                    st["bars"].append(bar)
                # Harmony, from arps only (an arp spells its chord): the best triad per bar, as its
                # root's interval to the tonic, and the successions of those roots from bar to bar.
                if role == "arp":
                    step = ppq / 4.0
                    nb = len(line) // 16
                    roots = []
                    for b in range(nb):
                        hist = [0.0] * 12
                        for s, e, p, v in notes:
                            a0, a1 = max(s, b * 16 * step), min(e, (b + 1) * 16 * step)
                            if a1 > a0:
                                hist[(p - tonic) % 12] += a1 - a0
                        if sum(hist) <= 0:
                            continue
                        best, bestScore = 0, -1.0
                        for r in range(12):
                            for third in (3, 4):
                                score = hist[r] * 1.0 + hist[(r + third) % 12] * 0.8 + hist[(r + 7) % 12] * 0.6
                                if score > bestScore:
                                    best, bestScore = r, score
                        roots.append(best)
                    if len(roots) >= 2:
                        chords["files"] += 1
                        chords["first"][roots[0]] += 1
                        for i2 in range(len(roots)):
                            chords["trans"][(roots[i2 - 1], roots[i2])] += 1   # cyclic: loops repeat
    if report:
        print("key source:", dict(keysrc))
        for r in ROLES:
            st = stats[r]
            print(f"{r}: {st['files']} melodic files (+{st.get('rhythmOnly', 0)} rhythm-only), {sum(st['uni'].values())} notes, {len(st['tri'])} distinct trigrams, "
                  f"median ambitus {sorted(st['ambitus'].elements())[len(list(st['ambitus'].elements()))//2] if st['ambitus'] else 0} st")
            onset = [sum(st['onset'][p][0]) and st['onset'][p][0][1] / sum(st['onset'][p][0]) for p in range(16)]
            dens = [(st['onset'][p][0][1] + st['onset'][p][1][1]) / max(1, sum(st['onset'][p][0]) + sum(st['onset'][p][1])) for p in range(16)]
            print("   onset probability per step:", " ".join(f"{x:.2f}" for x in dens))
            acc = [st['accent'][p][1] / max(1, sum(st['accent'][p])) for p in range(16)]
            sl = [st['slide'][p][1] / max(1, sum(st['slide'][p])) for p in range(16)]
            print("   accent per step:           ", " ".join(f"{x:.2f}" for x in acc))
            print("   slide per step:            ", " ".join(f"{x:.2f}" for x in sl))
            top = st["uni"].most_common(8)
            print("   commonest intervals to tonic:", ", ".join(f"{k:+d}:{100*v/sum(st['uni'].values()):.0f}%" for k, v in top))
        ch = stats["chords"]
        total = sum(ch["trans"].values())
        stay = sum(n for (a, b), n in ch["trans"].items() if a == b)
        print(f"chords: {ch['files']} multi-bar arp files, {total} bar successions, {100*stay/max(1,total):.0f} % keep the chord")
        moves = Counter()
        for (a, b), n in ch["trans"].items():
            if a != b:
                moves[(a, b)] += n
        print("   commonest changes (root interval to tonic, from -> to):",
              ", ".join(f"{a}->{b}:{n}" for (a, b), n in moves.most_common(12)))
        overall = Counter()
        for (a, b), n in ch["trans"].items():
            overall[b] += n
        print("   chord roots overall:", ", ".join(f"{r}:{100*n/max(1,total):.0f}%" for r, n in sorted(overall.items())))
    return stats


def write_tables(stats, out):
    lines = [
        "/**",
        " * @file CorpusTables.cpp",
        " * @brief GENERATED by Tools/corpus/build_corpus.py -- do not edit.",
        " *",
        " * Statistics of the local psytrance MIDI corpus: counts of pitch successions (interval to the",
        " * tonic, -12..+24 semitones) of order 0..2, onset/accent/slide/length counts per sixteenth step.",
        " * Counts only; no loop can be reconstructed from them. The MIDI files stay on the build machine.",
        " */",
        '#include "phos/Corpus.h"',
        "",
        "namespace phos {",
        "namespace {",
    ]
    for r in ROLES:
        st = stats[r]
        uni = [st["uni"].get(REL_MIN + i, 0) for i in range(ALPHABET)]
        lines.append(f"const uint32_t k_{r}_uni[{ALPHABET}] = {{ {', '.join(map(str, uni))} }};")
        bi = sorted(((b - REL_MIN) * ALPHABET + (c - REL_MIN), n) for (b, c), n in st["bi"].items())
        tri = sorted(((a - REL_MIN) * ALPHABET * ALPHABET + (b - REL_MIN) * ALPHABET + (c - REL_MIN), n) for (a, b, c), n in st["tri"].items())
        lines.append(f"const CorpusGram k_{r}_bi[] = {{ {', '.join(f'{{{k},{n}}}' for k, n in bi) or '{0,0}'} }};")
        lines.append(f"const CorpusGram k_{r}_tri[] = {{ {', '.join(f'{{{k},{n}}}' for k, n in tri) or '{0,0}'} }};")
        onset = ", ".join(f"{{{{{st['onset'][p][0][0]},{st['onset'][p][0][1]}}},{{{st['onset'][p][1][0]},{st['onset'][p][1][1]}}}}}" for p in range(16))
        lines.append(f"const uint32_t k_{r}_onset[16][2][2] = {{ {onset} }};")
        acc = ", ".join(f"{{{st['accent'][p][0]},{st['accent'][p][1]}}}" for p in range(16))
        sl = ", ".join(f"{{{st['slide'][p][0]},{st['slide'][p][1]}}}" for p in range(16))
        ln = ", ".join("{" + ",".join(str(st["length"][p].get(k, 0)) for k in range(9)) + "}" for p in range(16))
        lines.append(f"const uint32_t k_{r}_accent[16][2] = {{ {acc} }};")
        lines.append(f"const uint32_t k_{r}_slide[16][2] = {{ {sl} }};")
        lines.append(f"const uint32_t k_{r}_length[16][9] = {{ {ln} }};")
        amb = [st["ambitus"].get(i, 0) for i in range(37)]
        lines.append(f"const uint32_t k_{r}_ambitus[37] = {{ {', '.join(map(str, amb))} }};")
    ch = stats["chords"]
    trans = ", ".join("{" + ",".join(str(ch["trans"].get((a, b), 0)) for b in range(12)) + "}" for a in range(12))
    lines.append("} // namespace")
    lines.append("")
    lines.append(f"const uint32_t kCorpusChordTransitions[12][12] = {{ {trans} }};")
    lines.append(f"const uint32_t kCorpusChordFirst[12] = {{ {', '.join(str(ch['first'].get(r, 0)) for r in range(12))} }};")
    lines.append(f"const int kCorpusChordFiles = {ch['files']};")
    lines.append("")
    lines.append("const CorpusRole kCorpusRoles[kNumCorpusRoles] = {")
    for r in ROLES:
        st = stats[r]
        nbi, ntri = max(1, len(st["bi"])), max(1, len(st["tri"]))
        lines.append(f"    {{ \"{r}\", {st['files']}, k_{r}_uni, k_{r}_bi, {nbi}, k_{r}_tri, {ntri}, k_{r}_onset, k_{r}_accent, k_{r}_slide, k_{r}_length, k_{r}_ambitus }},")
    lines.append("};")
    lines += lead_template_lines(stats.get("root", "M:/Midi"))
    lines += bass_rhythm_lines(stats.get("root", "M:/Midi"))
    lines.append("")
    lines.append("} // namespace phos")
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print("written", out)


#: First line of the generated bass-rhythm block; :func:`splice_bass_rhythm` cuts the file here.
BASS_MARKER = "// ---------------------------------------------------------------------------- bass rhythm"


def lead_template_lines(root):
    """The bar-template block (22.09.2026) from :mod:`lead_templates`; imported inside for the same reason."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lead_templates                                      # noqa: E402
    return lead_templates.emit_lines(root)


def splice_lead_templates(out, root):
    """Rewrites only the lead-template block of an existing ``CorpusTables.cpp`` (before the bass block)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lead_templates                                      # noqa: E402
    with open(out, "r", encoding="utf-8") as fh:
        text = fh.read()
    marker = lead_templates.MARKER
    if marker in text:
        head, rest = text.split(marker, 1)
        tail = BASS_MARKER + rest.split(BASS_MARKER, 1)[1]
    else:
        head, rest = text.split(BASS_MARKER, 1)
        tail = BASS_MARKER + rest
    body = "\n".join(lead_template_lines(root))
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(head.rstrip("\n") + "\n\n" + body + "\n" + tail)
    print("lead template block written into", out)


def bass_rhythm_lines(root):
    """The bass onset model's block, from :mod:`bass_rhythm`.

    Imported inside the call and not at module level: ``bass_rhythm`` reaches the bass corpus through
    ``Tools/train/bass.py``, which imports ``dataset``, which imports *this* module.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import bass_rhythm                                         # noqa: E402
    return bass_rhythm.emit_lines(root)


def splice_bass_rhythm(out, root):
    """Rewrites only the bass-rhythm block of an existing ``CorpusTables.cpp``.

    The melodic tables above it take a full walk of the MIDI packs to rebuild and must not move when
    only the bass onset model is refitted, so everything from :data:`BASS_MARKER` to the closing
    namespace is replaced and nothing above it is touched. Writing the whole file (``--out`` without
    ``--bass-rhythm-only``) emits the same block through the same function, so the two routes agree
    line for line.
    """
    with open(out, "r", encoding="utf-8") as fh:
        text = fh.read()
    head = text.split(BASS_MARKER)[0].rstrip("\n")
    if head.endswith("} // namespace phos"):
        head = head[:-len("} // namespace phos")].rstrip("\n")
    body = "\n".join(bass_rhythm_lines(root))
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(head + body + "\n\n} // namespace phos\n")
    print("bass rhythm block written into", out)


# ------------------------------------------------------------------------------------------------ memorisation

def memorisation(stats, midi_path):
    ppq, _ = read_midi(midi_path)
    d = open(midi_path, "rb").read()
    # Track names and key signature from the file (Phosphene writes one track per part).
    names, keys, tracks = [], [], []
    _, ntrk, div = struct.unpack(">HHH", d[8:14])
    i = 14
    for _ in range(ntrk):
        ln = struct.unpack(">I", d[i + 4:i + 8])[0]
        j, end, t, run, name = i + 8, i + 8 + ln, 0, None, ""
        notes, open_notes = [], {}
        while j < end:
            dt, j = read_vlq(d, j)
            t += dt
            st = d[j]
            if st == 0xFF:
                ty = d[j + 1]
                l, j2 = read_vlq(d, j + 2)
                if ty == 0x03:
                    name = d[j2:j2 + l].decode("latin1")
                if ty == 0x59 and l == 2:
                    sf = struct.unpack("b", d[j2:j2 + 1])[0]
                    keys.append((t, (9 + 7 * sf) % 12))
                j = j2 + l
                continue
            if st & 0x80:
                run = st
                j += 1
            st = run
            hi = st & 0xF0
            n = 1 if hi in (0xC0, 0xD0) else 2
            a, b = d[j], (d[j + 1] if n == 2 else 0)
            j += n
            if hi == 0x90 and b > 0:
                open_notes.setdefault(a, []).append((t, b))
            elif hi == 0x80 or (hi == 0x90 and b == 0):
                if open_notes.get(a):
                    s, v = open_notes[a].pop(0)
                    notes.append((s, t, a, v))
        tracks.append((name, sorted(notes)))
        i = end
    keys.sort()
    for name, notes in tracks:
        role = name.lower()
        if role not in ROLES or not notes:
            continue
        corpus = set()
        for bar in stats[role]["bars"]:
            ps = [x for x in bar if x is not None]
            if len(set(ps)) >= 3:
                corpus.add(tuple(None if x is None else x - min(ps) for x in bar))
        line = top_line(div, notes)
        total = hits = 0
        for b in range(len(line) // 16):
            bar = line[b * 16:(b + 1) * 16]
            ps = [s[0] for s in bar if s]
            if len(set(ps)) < 3:
                continue
            key = tuple(None if s is None else s[0] - min(ps) for s in bar)
            total += 1
            hits += 1 if key in corpus else 0
        if total:
            print(f"{role}: {hits} of {total} bars ({100.0*hits/total:.2f} %) identical to a corpus bar (transposition-invariant)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="M:/Midi")
    ap.add_argument("--out")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--memorisation")
    ap.add_argument("--bass-rhythm-only", action="store_true",
                    help="refit only the bass onset model and splice it into --out")
    ap.add_argument("--lead-templates-only", action="store_true",
                    help="recount only the bar templates (22.09.2026) and splice them into --out")
    a = ap.parse_args()
    if a.lead_templates_only:
        if not a.out:
            print("--lead-templates-only needs --out", file=sys.stderr)
            return 2
        splice_lead_templates(a.out, a.root)
        return 0
    if a.bass_rhythm_only:
        if not a.out:
            print("--bass-rhythm-only needs --out", file=sys.stderr)
            return 2
        splice_bass_rhythm(a.out, a.root)
        return 0
    stats = analyse(a.root, a.report or bool(a.out))
    stats["root"] = a.root
    if a.out:
        write_tables(stats, a.out)
    if a.memorisation:
        memorisation(stats, a.memorisation)
    return 0


if __name__ == "__main__":
    sys.exit(main())
