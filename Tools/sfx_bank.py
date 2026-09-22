"""Generates the effect preset bank (23.09.2026, round "SFX"): Core/src/SfxBankTables.cpp and docs/sfx_bank.tsv.

**Why.** The user: "Die kurzen Zips und Zaps bei den SFX wiederholen sich viiiiel zu oft [...] Da brauchen
wir DEUTLICH mehr verschiedene Effekte [...] Ich bevorzuge dabei auch etwas flaechigere und laengere
Effekte [...] mindestens 1024 besser 2048 Presets." Until this round every effect type was one fixed
synthesis with a little per-event jitter, so every zap was the last one. Now each type of the effects
strip has a *family* of presets -- twelve parameters that steer its synthesis (Sfx.h, SfxPreset) -- and
the composer picks one per event, never the same twice in a track (Form.cpp, makeFormSfx).

**What the literature says.** Psytrance production writing names risers built from three layers (a
tonal sweep, filtered noise, a rhythmic element) with staggered peaks, downlifters, impacts, whooshes
and sweeps, and *atmospheres* as a background layer that takes over in breakdowns (Myloops, "Designing
trance risers, impacts and transition effects"; Psychedelic Island, "Deconstructing classic psytrance
tracks"). Hence the new Atmosphere type gets the biggest family, and the bank's durations lean long:
about 60 % of the presets are two bars or longer, a quarter mid-length, the short one-shots the rest.

**Determinism.** Everything comes from one seed (kSeed): running the script again writes the same
tables. The bank is compiled in, so no data file can be missing at run time; the TSV is the readable copy.

Usage:
    python Tools/sfx_bank.py [--out Core/src/SfxBankTables.cpp] [--tsv docs/sfx_bank.tsv]
"""
import argparse
import os
import random

kSeed = 20260923

# SfxType order (Sfx.h). Atmosphere is appended after the texture types.
TYPES = ["Riser", "Downlifter", "Impact", "Sweep", "FormantShot", "ReverseSwell", "Zap",
         "Squelch", "Bubble", "Stutter", "SubDrop", "ReverseCrash",
         "FormantVoice", "AlienChatter", "SpokenWord", "VoiceChop",
         "Bowl", "Didgeridoo", "JawHarp", "Atmosphere"]

# Family sizes: 2048 in all. Long, pad-like families are the biggest.
FAMILY = {
    "Atmosphere": 512,
    "ReverseSwell": 256,
    "Riser": 256,
    "Sweep": 256,
    "Downlifter": 128,
    "ReverseCrash": 128,
    "Impact": 128,
    "Squelch": 128,   # the climax's gap squelches and the candy: the busiest short type, so the biggest short family
    "FormantShot": 96,
    "Zap": 96,
    "Bubble": 64,
}
assert sum(FAMILY.values()) == 2048, sum(FAMILY.values())

FIELDS = ["lengthScale", "toneMix", "filterLo", "filterHi", "resonance", "envShape",
          "pitchInterval", "detune", "motionRate", "panSpeed", "wet", "metal"]

INTERVALS = [0, 7, 12, 5, 10, 1, -12, 3, 8]   # root, fifth, octave, fourth, b7, b2, octave down, b3, b6


def preset(rng, family):
    """One preset of a family: twelve numbers inside the family's ranges (Sfx.cpp reads them)."""
    u = rng.random
    if family == "Atmosphere":
        return dict(lengthScale=1.0, toneMix=0.35 + 0.55 * u(), filterLo=0.8 + 1.6 * u(), filterHi=1.8 + 2.4 * u(),
                    resonance=0.15 + 0.6 * u(), envShape=0.1 + 0.5 * u(), pitchInterval=rng.choice([0, 7, 12, 5, 10, -12, 3]),
                    detune=0.2 + 0.8 * u(), motionRate=0.03 + 0.4 * u(), panSpeed=0.05 + 0.6 * u(), wet=0.3 + 0.6 * u(), metal=0.35 * u() ** 2)
    if family in ("Riser", "Downlifter"):
        return dict(lengthScale=1.0, toneMix=0.2 + 0.7 * u(), filterLo=0.3 + 1.2 * u(), filterHi=3.4 + 1.6 * u(),
                    resonance=0.2 + 0.7 * u(), envShape=1.2 + 1.8 * u(), pitchInterval=rng.choice([0, 0, 7, 12, 5]),
                    detune=0.1 + 0.9 * u(), motionRate=0.5 + 7.0 * u(), panSpeed=0.2 + 0.8 * u(), wet=0.2 * u(), metal=0.3 * u() ** 2)
    if family == "Sweep":
        return dict(lengthScale=0.75 + 1.75 * u(), toneMix=0.1 * u(), filterLo=0.3 + 1.2 * u(), filterHi=3.0 + 2.5 * u(),
                    resonance=0.3 + 0.65 * u(), envShape=0.6 + 1.2 * u(), pitchInterval=0.0, detune=0.0,
                    motionRate=0.25 + 1.5 * u(), panSpeed=0.1 + 0.6 * u(), wet=0.1 + 0.5 * u(), metal=0.2 * u() ** 2)
    if family == "ReverseSwell":
        return dict(lengthScale=1.0 + 2.0 * u(), toneMix=0.05 + 0.35 * u(), filterLo=2.0 + 1.5 * u(), filterHi=3.0 + 2.0 * u(),
                    resonance=0.1 + 0.4 * u(), envShape=0.6 + 2.0 * u(), pitchInterval=rng.choice([0, 7, 12, 5, 10, 3]),
                    detune=0.1 + 0.6 * u(), motionRate=0.1 + 0.6 * u(), panSpeed=0.05 + 0.4 * u(), wet=0.3 + 0.6 * u(), metal=0.4 * u() ** 2)
    if family == "ReverseCrash":
        return dict(lengthScale=0.75 + 1.5 * u(), toneMix=0.0, filterLo=3.5 + 1.5 * u(), filterHi=4.5 + 1.0 * u(),
                    resonance=0.3 + 0.6 * u(), envShape=3.0 + 4.0 * u(), pitchInterval=0.0, detune=0.0,
                    motionRate=0.1 + 0.5 * u(), panSpeed=0.1 + 0.5 * u(), wet=0.2 + 0.6 * u(), metal=0.3 + 0.7 * u())
    if family == "Impact":
        return dict(lengthScale=0.6 + 1.4 * u(), toneMix=0.3 + 0.6 * u(), filterLo=2.5 + 1.5 * u(), filterHi=0.5 + 1.0 * u(),
                    resonance=0.1 + 0.3 * u(), envShape=0.6 + 1.2 * u(), pitchInterval=rng.choice([0, 0, -12, 7]),
                    detune=0.0, motionRate=0.0, panSpeed=0.0, wet=0.2 + 0.6 * u(), metal=0.5 * u() ** 2)
    if family == "FormantShot":
        return dict(lengthScale=0.5 + 1.5 * u(), toneMix=1.0, filterLo=0.0, filterHi=0.0, resonance=0.3 + 0.6 * u(),
                    envShape=0.5 + 1.5 * u(), pitchInterval=rng.choice([0, 12, 7, 5, -5]), detune=0.0,
                    motionRate=u(), panSpeed=0.0, wet=0.1 + 0.4 * u(), metal=0.0)
    if family == "Zap":
        return dict(lengthScale=0.6 + 1.6 * u(), toneMix=1.0, filterLo=2.5 + 2.0 * u(), filterHi=0.0 + 1.5 * u(),
                    resonance=0.0, envShape=0.4 + 1.6 * u(), pitchInterval=0.0, detune=0.0,
                    motionRate=0.5 + 2.5 * u(), panSpeed=0.0, wet=0.0 + 0.4 * u(), metal=0.4 * u() ** 2)
    if family == "Squelch":
        return dict(lengthScale=0.6 + 1.6 * u(), toneMix=1.0, filterLo=3.3 + 1.0 * u(), filterHi=0.6 + 1.2 * u(),
                    resonance=0.3 + 0.7 * u(), envShape=1.0 + 2.0 * u(), pitchInterval=rng.choice([24, 19, 12, 17]),
                    detune=0.0, motionRate=0.0, panSpeed=0.0, wet=0.0 + 0.3 * u(), metal=0.0)
    if family == "Bubble":
        return dict(lengthScale=0.6 + 1.8 * u(), toneMix=1.0, filterLo=0.0, filterHi=0.0, resonance=0.0,
                    envShape=0.0, pitchInterval=0.0, detune=0.0, motionRate=3.0 + 6.0 * u(), panSpeed=0.5 + 0.5 * u(),
                    wet=0.1 + 0.4 * u(), metal=0.0)
    raise KeyError(family)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, ".."))
    ap.add_argument("--out", default=os.path.join(root, "Core", "src", "SfxBankTables.cpp"))
    ap.add_argument("--tsv", default=os.path.join(root, "docs", "sfx_bank.tsv"))
    a = ap.parse_args()
    rng = random.Random(kSeed)
    rows = []          # (typeIndex, familyIndex, preset)
    offsets = [0] * len(TYPES)
    counts = [0] * len(TYPES)
    for family in TYPES:
        n = FAMILY.get(family, 0)
        offsets[TYPES.index(family)] = len(rows)
        counts[TYPES.index(family)] = n
        for k in range(n):
            rows.append((TYPES.index(family), k, preset(rng, family)))
    lines = [
        "/**",
        " * @file SfxBankTables.cpp",
        " * @brief GENERATED by Tools/sfx_bank.py -- do not edit. The effect preset bank (Sfx.h, SfxPreset).",
        " *",
        f" * {len(rows)} presets in {sum(1 for c in counts if c)} families, from seed {kSeed}; the readable copy is docs/sfx_bank.tsv.",
        " */",
        '#include "phos/Sfx.h"',
        "",
        "namespace phos {",
        "",
        f"const SfxPreset kSfxBank[{len(rows)}] = {{",
    ]
    for t, k, p in rows:
        vals = ", ".join(f"{p[f]:.4f}f" for f in FIELDS)
        lines.append(f"    {{ {vals} }},   // {TYPES[t]} {k + 1}")
    lines.append("};")
    lines.append(f"const int kSfxBankSize = {len(rows)};")
    lines.append("const int kSfxBankOffset[kNumSfxTypes] = { " + ", ".join(map(str, offsets)) + " };")
    lines.append("const int kSfxBankCount[kNumSfxTypes] = { " + ", ".join(map(str, counts)) + " };")
    lines.append("")
    lines.append("} // namespace phos")
    with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    with open(a.tsv, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("type\tindex\t" + "\t".join(FIELDS) + "\n")
        for t, k, p in rows:
            fh.write(f"{TYPES[t]}\t{k + 1}\t" + "\t".join(f"{p[f]:.4f}" for f in FIELDS) + "\n")
    print(f"written {len(rows)} presets to {a.out} and {a.tsv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
