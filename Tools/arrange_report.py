"""Measures a rendered set against the user's arrangement rules of 19.09.2026 (round "arrangement").

Reads the bar log that `phos_render --bar-log FILE.tsv` writes (one line per bar and sounding track; with
`--score-only` the score alone, without audio) and prints, per style and over every complete track in it:

  - where the sections sit (1-based bars) and how often that is the rule's position,
  - the bar on which the set's first track's kick enters,
  - the snare roll's spacing in every bar of the big buildup,
  - the output power per section (owner bars only) and drop 2's margin over drop 1, over every other
    section and over every other eight-bar window of its track,
  - the percussion lanes and planned layers per eight-bar block of intro and outro,
  - what sounds on beat 4 of the last bar of every buildup.

Usage:  python arrange_report.py LOG.tsv [LOG2.tsv ...]
"""
import sys
from collections import defaultdict

import numpy as np

# Part enum of Score.h, for the parts bit masks.
PARTS = ["kick", "bass", "perc", "acid", "lead", "counter", "arp", "stab", "pad", "drone", "sfx", "texture", "vocal"]
SFX = ["Riser", "Downlifter", "Impact", "Sweep", "FormantShot", "ReverseSwell", "Zap", "Squelch", "Bubble", "Stutter",
       "SubDrop", "ReverseCrash", "FormantVoice", "AlienChatter", "SpokenWord", "VoiceChop", "Bowl", "Didgeridoo", "JawHarp"]
TEMPLATES = ["Full-On", "Progressive", "Goa", "Dark Forest"]


def names(mask, table):
    return "+".join(n for i, n in enumerate(table) if mask >> i & 1) or "-"


def load(path):
    rows = []
    with open(path) as f:
        head = f.readline().rstrip("\n").split("\t")
        for line in f:
            v = line.rstrip("\n").split("\t")
            rows.append({k: (x if k == "section" else float(x)) for k, x in zip(head, v)})
    return rows


def report(path):
    rows = load(path)
    tracks = defaultdict(list)
    for r in rows:
        tracks[int(r["track"])].append(r)
    last = max(tracks)
    complete = [t for t in tracks if t != last]      # the last track of a log is cut off
    print(f"== {path}: {len(complete)} complete tracks")
    positions = defaultdict(int)
    margins1, marginsAll, marginsWin = [], [], []
    powers = defaultdict(list)
    roll = defaultdict(list)
    beat4 = defaultdict(int)
    intro_blocks = defaultdict(list)
    outro_blocks = defaultdict(list)
    for t in complete:
        tr = sorted(tracks[t], key=lambda r: r["intrack"])
        body = int(tr[0]["body"])
        secs = []
        for r in tr:
            if not secs or secs[-1][2] != int(r["secindex"]):
                secs.append([r["section"], int(r["intrack"]), int(r["secindex"]), 0, int(r["climax"])])
            secs[-1][3] += 1
        positions[(TEMPLATES[body], " ".join(f"{s[0]}{s[1] + 1}-{s[1] + s[3]}" for s in secs))] += 1
        own = [r for r in tr if r["owner"] == 1 and r["power_db"] > -199]
        if own:
            p = defaultdict(list)
            for r in own:
                p[(int(r["secindex"]), r["section"], int(r["climax"]))].append(10 ** (r["power_db"] / 10))
            sec_db = {k: 10 * np.log10(np.mean(v)) for k, v in p.items()}
            for k, v in sec_db.items():
                powers[(k[1] + ("2" if k[2] else ""))].append(v)
            climax = [v for k, v in sec_db.items() if k[2]]
            drops = [v for k, v in sec_db.items() if k[1] == "Drop" and not k[2]]
            others = [v for k, v in sec_db.items() if not k[2]]
            if climax and drops:
                margins1.append(climax[0] - max(drops))
                marginsAll.append(climax[0] - max(others))
                # Every other eight-bar window of the track (owner bars, outside drop 2).
                pw = [(int(r["intrack"]), 10 ** (r["power_db"] / 10), int(r["climax"])) for r in own]
                wins = []
                for i in range(0, len(pw) - 7):
                    w = pw[i:i + 8]
                    if any(x[2] for x in w):
                        continue
                    wins.append(10 * np.log10(np.mean([x[1] for x in w])))
                if wins:
                    marginsWin.append(climax[0] - max(wins))
        # The big buildup's roll: the snare's spacing per bar.
        for s in secs:
            if s[0] == "Build" and s[3] >= 16:
                for r in tr:
                    if int(r["secindex"]) == s[2] and r["snare"] > 0:
                        roll[int(r["barinsec"])].append(r["snaregap"])
        # Beat 4 of every buildup's last bar.
        for s in secs:
            if s[0] == "Build":
                r = [x for x in tr if int(x["intrack"]) == s[1] + s[3] - 1][0]
                beat4[(names(int(r["beat4parts"]), PARTS), names(int(r["beat4sfx"]), SFX))] += 1
        # Intro and outro per eight-bar block: lanes heard (the bar's own score, both tracks over the overlap)
        # and the layers this track planned.
        for s in secs:
            if s[0] in ("Intro", "Outro"):
                blocks = intro_blocks if s[0] == "Intro" else outro_blocks
                for g in range(s[3] // 8):
                    rs = [x for x in tr if int(x["secindex"]) == s[2] and g * 8 <= int(x["barinsec"]) < g * 8 + 8]
                    lanes = set()
                    parts = 0
                    for x in rs:
                        m = int(x["lanes"])
                        lanes |= {i for i in range(16) if m >> i & 1}
                        parts |= int(x["planparts"])
                    kicks = sum(int(x["kicks"]) for x in rs)
                    blocks[g].append((len(lanes), rs[0]["layers"], kicks > 0, bin(parts).count("1")))
    for k, n in sorted(positions.items(), key=lambda x: -x[1]):
        print(f"  {n:3d} x {k[0]}: {k[1]}")
    if roll:
        print("  big buildup, snare spacing per bar (beats):", " ".join(f"{b + 1}:{np.median(v):.3f}" for b, v in sorted(roll.items())))
    for k, n in sorted(beat4.items(), key=lambda x: -x[1]):
        print(f"  beat 4 of a buildup's last bar: parts {k[0]}, effects {k[1]}: {n}")
    for label, blocks in (("intro", intro_blocks), ("outro", outro_blocks)):
        print(f"  {label} blocks (lanes heard / planned layers / kick / planned parts, medians):",
              "  ".join(f"{g * 8 + 1}-{g * 8 + 8}: {np.median([b[0] for b in v]):.0f}/{np.median([b[1] for b in v]):.0f}/"
                        f"{np.mean([b[2] for b in v]) * 100:.0f}%/{np.median([b[3] for b in v]):.0f}" for g, v in sorted(blocks.items())))
    if powers:
        print("  power per section (dBFS, median over tracks):", "  ".join(f"{k} {np.median(v):.2f}" for k, v in sorted(powers.items())))
    if margins1:
        print(f"  drop 2 over drop 1: median {np.median(margins1):+.2f} dB, min {min(margins1):+.2f}; over every other section: "
              f"median {np.median(marginsAll):+.2f}, min {min(marginsAll):+.2f}; over every other 8-bar window: "
              f"median {np.median(marginsWin):+.2f}, min {min(marginsWin):+.2f} dB; negative in {sum(m <= 0 for m in marginsWin)} of {len(marginsWin)}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        report(p)
