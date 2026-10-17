"""Phosphene's factory sets (17.10.2026): 256 sets to begin a night with -> Core/src/FactorySetsData.inc.

A Phosphene set starts in a style and runs along an energy arc (compose.arc) over its length (compose.set_minutes);
its tracks blend into each other. The bank has each of the five start styles through the five arcs, ten sets an arc
(250), and six long nights of five hours, the most a set is planned for. A set is what a player would save after an
evening of curating: a seed, its style, its arc, its length, and a few of the composer's knobs drawn near their
defaults -- the tempo's range, a track's length, how much acid, lead, arp, pad and effects, the percussion's density,
the melodies' temperature, the squelches and the gates. The composer picks every track's sounds as always.

Every set a name of its own (two words of light, geometry and ritual), drawn without repeats; the draws are seeded, so
the bank is the same every time. A set plays exactly its length (the plan fills compose.set_minutes), so the table
carries it without measuring.

    python Tools/sets/make_sets.py
"""
import pathlib
import random

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "Core" / "src" / "FactorySetsData.inc"

STYLES = ["Goa", "Full-On", "Progressive", "Dark Forest", "Hi-Tech"]
ARCS = {   # ArcNames with the lengths each is played at, minutes
    "Warm-up": [45, 60, 90, 120],
    "Peak-Time": [45, 60, 90, 120],
    "Morning": [60, 90, 120, 180],
    "Closing": [45, 60, 90, 120],
    "Flat": [30, 45, 60, 90],
}
PER_ARC = 10
LONG = "Long night"
LONG_ARCS = ["Peak-Time", "Morning", "Warm-up", "Peak-Time", "Morning", "Closing"]

FIRST = ["Fractal", "Prism", "Lucid", "Astral", "Spiral", "Mystic", "Solar", "Ultraviolet", "Infrared", "Liquid",
         "Electric", "Cosmic", "Lotus", "Jungle", "Mandala", "Neon", "Quantum", "Crystal", "Dream", "Ancient", "Sacred",
         "Hidden", "Twisted", "Burning", "Glowing", "Tribal", "Phantom", "Ether", "Plasma", "Nebula", "Vortex",
         "Serpent", "Desert", "Forest", "Temple", "Galactic", "Orbital", "Morphic", "Sonic", "Golden", "Emerald",
         "Indigo", "Velvet", "Silver", "Hyper", "Inner", "Outer", "Endless"]
SECOND = ["Gateway", "Spiral", "Vision", "Dimension", "Portal", "Ritual", "Journey", "Pulse", "Garden", "Machine",
          "Signal", "Frequency", "Matrix", "Labyrinth", "Oracle", "Mantra", "Dragon", "Sunrise", "Eclipse", "Equinox",
          "Dawn", "Nexus", "Fields", "Code", "Geometry", "Rain", "Fire", "Tribe", "Gathering", "Awakening", "Paradox",
          "Echo", "Sphere", "Horizon", "Wave", "Bloom", "Waters", "Visions", "Circuit", "Ascent", "Spectrum",
          "Mirage", "Kingdom", "Engine", "Flight", "Current", "Totem", "Sanctuary"]


def length_text(minutes):
    """A set's length as people say it."""
    if minutes < 60 or minutes % 60:
        return f"{minutes} min"
    h = minutes // 60
    return "1 hour" if h == 1 else f"{h} hours"


def names(rng, count):
    """Two words each, never the same word twice in a name, never a name twice."""
    out, seen = [], set()
    while len(out) < count:
        a, b = rng.choice(FIRST), rng.choice(SECOND)
        if a == b or a + " " + b in seen:
            continue
        seen.add(a + " " + b)
        out.append(a + " " + b)
    return out


def sets():
    """The 256 sets: (name, style, group, note, text, seconds)."""
    rng = random.Random(17102029)
    plan = []
    for s, style in enumerate(STYLES):
        for arc, lengths in ARCS.items():
            for k in range(PER_ARC):
                plan.append((style, arc, arc, lengths[k % len(lengths)]))
        # its long nights after its arcs (one each, Goa two), so the browser lists them under the style
        for k, arc in enumerate(LONG_ARCS):
            if k % len(STYLES) == s:
                plan.append((style, LONG, arc, 300))
    out = []
    for (style, group, arc, minutes), name in zip(plan, names(rng, len(plan))):
        knobs = [
            ("compose.set_minutes", f"{minutes}"),
            ("compose.tempo_range", f"{rng.uniform(0.5, 3.0):.1f}"),
            ("compose.track_bars", f"{rng.choice([192, 224, 256, 288, 320])}"),
            ("compose.track_variation", f"{rng.uniform(0.35, 0.7):.2f}"),
            ("compose.acid_amount", f"{rng.uniform(0.4, 0.85):.2f}"),
            ("compose.lead_amount", f"{rng.uniform(0.35, 0.7):.2f}"),
            ("compose.arp_amount", f"{rng.uniform(0.35, 0.7):.2f}"),
            ("compose.pad_amount", f"{rng.uniform(0.5, 0.85):.2f}"),
            ("compose.sfx_amount", f"{rng.uniform(0.5, 0.85):.2f}"),
            ("compose.perc_density", f"{rng.uniform(0.45, 0.75):.2f}"),
            ("compose.melody_temperature", f"{rng.uniform(0.85, 1.2):.2f}"),
            ("compose.squelch_chance", f"{rng.uniform(0.15, 0.45):.2f}"),
            ("compose.gate_chance", f"{rng.uniform(0.2, 0.5):.2f}"),
        ]
        seed = rng.randrange(1, 2 ** 31)
        text = ["phosset 2", f"seed={seed}", f"style={style}", f"arc={arc}"]
        text += [f"{k}={v}" for k, v in knobs]
        note = length_text(minutes) + (f", {arc} from {style}" if group == LONG else f", from {style}")
        out.append((name, style, group, note, "\n".join(text) + "\n", minutes * 60.0))
    return out


def c_string(s):
    """A C string literal, a line of the set to a line of the source."""
    lines = s.split("\n")
    parts = [line.replace("\\", "\\\\").replace('"', '\\"') + "\\n" for line in lines[:-1]]
    return "\n".join(f'      "{p}"' for p in parts)


def main():
    rows = sets()
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("// The factory's sets (FactorySets.h), written by Tools/sets/make_sets.py -- not by hand.\n")
        for name, style, group, note, text, seconds in rows:
            f.write(f'    {{ "{name}", "{style}", "{group}", "{note}",\n{c_string(text)},\n      {seconds:.1f} }},\n')
    print(f"{len(rows)} sets -> {OUT}")


if __name__ == "__main__":
    main()
