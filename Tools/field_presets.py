"""Writes the Field track's factory presets, `Core/src/FieldPresetData.inl` (27.09.2026).

    python Tools/field_presets.py

The user, on the sampler: "Sollten wir für die vorhandenen Samples dann Presets schreiben, die einfach in den Sampler
geladen werden können?" -- yes: one preset per recording of `Tools/field_selection.json` (Tools/field_select.py), each
with its loop, its crossfade, its envelopes, its low cut and its room, set from what the recording is; and a group of
scenes, two recordings layered (A and B) the way a place sounds -- insects over a night, rain on a forest.

A preset names its recordings by file (not by variation number), so it finds its file in any library that has it
(FieldPresets.cpp, fieldVariationOf). Its style weights (StyleId order: Goa, Full-On, Progressive, Dark Forest,
Hi-Tech) are what the composer draws a track's preset by (Composer::fieldPresetFor): forest, night and insects for
Dark Forest -- the user's "insbesondere für Forest" --, sea, fire and ritual for Goa, wind, rain and water for
Progressive, machines and radio for Hi-Tech, space for Full-On.

What is set, and why:
  * loop over the whole recording with a 250 ms crossfade -- Noctuary's recordings are seamless loops, the crossfade
    only covers a seam that is not quite; NASA's are not loops, so 2 s, from a random start;
  * the low cut by category (the depth rule: under 100 Hz only kick and bass), higher for the rumbling ones -- wind,
    water, rain -- whose power sits there;
  * a gentle low pass at 11 kHz on the brightest (insects, electric noise), whose top would sit over the hats;
  * the hall send by space: caves, tunnels, empty space far, the rest near;
  * amp envelope: 2 s in, 4 s out -- a place fades in under an intro and out of a breakdown.
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SELECTION = os.path.join(ROOT, "Tools", "field_selection.json")
OUT = os.path.join(ROOT, "Core", "src", "FieldPresetData.inl")

# Params.cpp, kFieldCategorySlugs (the last is "nasa").
SLUGS = ["rainforest", "night-country", "insects", "foliage", "wetland", "mud-bubbles", "cave", "river", "sea",
         "rain-land", "rain-roof", "wind", "underwater", "ice-snow", "desert", "fire", "geothermal", "seismic",
         "ritual-objects", "abandoned", "tunnel", "metal-creak", "dark-drone-noise", "electric", "ventilation",
         "factory", "polar-station", "radio-space", "empty-space", "grain-texture", "nasa"]
NAMES = ["Rainforest", "Night Country", "Insects", "Foliage", "Wetland", "Mud Bubbles", "Cave", "River", "Sea",
         "Rain on Land", "Rain on Roof", "Wind", "Underwater", "Ice and Snow", "Desert", "Fire", "Geothermal", "Seismic",
         "Ritual Objects", "Abandoned", "Tunnel", "Metal Creak", "Dark Drone", "Electric", "Ventilation", "Factory",
         "Polar Station", "Radio Space", "Empty Space", "Grain Texture", "NASA"]

#            Goa  FullOn Prog  DForest HiTech
FOREST =   (0.2, 0.1, 0.4, 1.0, 0.1)
GOA =      (1.0, 0.4, 0.6, 0.4, 0.1)
PROG =     (0.3, 0.5, 1.0, 0.3, 0.1)
HITECH =   (0.1, 0.3, 0.1, 0.4, 1.0)
SPACE =    (0.4, 1.0, 0.3, 0.2, 0.6)
DARK =     (0.2, 0.2, 0.4, 0.6, 0.6)
STYLE = {"rainforest": FOREST, "night-country": FOREST, "insects": FOREST, "foliage": FOREST, "wetland": FOREST,
         "mud-bubbles": FOREST, "cave": FOREST, "river": (0.8, 0.3, 0.9, 0.5, 0.1), "sea": (1.0, 0.6, 0.6, 0.2, 0.1),
         "rain-land": PROG, "rain-roof": PROG, "wind": (0.3, 0.6, 1.0, 0.3, 0.2), "underwater": PROG, "ice-snow": DARK,
         "desert": (0.6, 0.3, 0.5, 0.2, 0.2), "fire": GOA, "geothermal": DARK, "seismic": DARK, "ritual-objects": GOA,
         "abandoned": DARK, "tunnel": DARK, "metal-creak": HITECH, "dark-drone-noise": DARK, "electric": HITECH,
         "ventilation": HITECH, "factory": HITECH, "polar-station": DARK, "radio-space": HITECH, "empty-space": SPACE,
         "grain-texture": HITECH, "nasa": SPACE}
LOW_CUT = {"wind": 220, "river": 180, "sea": 180, "rain-land": 200, "rain-roof": 200, "underwater": 160, "geothermal": 140,
           "seismic": 130, "dark-drone-noise": 130, "ventilation": 180, "factory": 170, "mud-bubbles": 160}
HALL = {"cave": 0.35, "tunnel": 0.35, "empty-space": 0.4, "abandoned": 0.3, "polar-station": 0.25, "nasa": 0.3,
        "radio-space": 0.25}
ADJ = ["Deep", "Warm", "Open", "Bright"]

# Scenes: two recordings as one place (A, B); B one step under A.
SCENES = [
    ("Jungle Night", "night-country", "insects"), ("Canopy Rain", "rainforest", "rain-land"),
    ("Forest Floor", "foliage", "insects"), ("Riverbank", "river", "foliage"), ("Swamp", "wetland", "mud-bubbles"),
    ("Dripping Cave", "cave", "underwater"), ("Stormy Shore", "sea", "wind"), ("Rain on the Hut", "rain-roof", "fire"),
    ("Ritual Fire", "fire", "ritual-objects"), ("Magma", "geothermal", "seismic"), ("Polar Night", "polar-station", "wind"),
    ("Ice Field", "ice-snow", "wind"), ("Dune Wind", "desert", "wind"), ("Machine Hall", "factory", "ventilation"),
    ("Rust", "abandoned", "metal-creak"), ("Power Line", "electric", "grain-texture"),
    ("Transmission", "radio-space", "empty-space"), ("Void", "empty-space", "dark-drone-noise"),
    ("Underpass", "tunnel", "dark-drone-noise"), ("Mangrove", "wetland", "insects"),
]


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def text_of(slug, centroid, nasa):
    kv = {"loop": 1, "loop_start": 0, "loop_end": 1, "loop_xfade": 2000 if nasa else 250,
          "a_start_random": 0.6 if nasa else 1.0, "amp_attack": 3000 if nasa else 2000,
          "amp_release": 5000 if nasa else 4000, "low_cut": LOW_CUT.get(slug, 150),
          "hall_send": HALL.get(slug, 0.15), "room_send": 0.1}
    if centroid > 2500:
        kv["cutoff"] = 11000
        kv["resonance"] = 0.1
    return "\\n".join("%s=%g" % (k, v) for k, v in kv.items())


def main():
    rows = json.load(open(SELECTION, encoding="utf-8"))
    out = []
    byCat = {}
    for r in rows:
        if r["category"] == "nasa-shot":
            continue
        slug = r["category"]
        stem = os.path.splitext(os.path.basename(r["file"]))[0]
        byCat.setdefault(slug, []).append((r["centroid"], stem))
    # The shipped variation of each file: its place among its category's files sorted by name, as FieldLibrary.cpp
    # sorts them (byte order) -- what a preset falls back to where the library cannot be asked.
    shipped = {slug: sorted(stem for _, stem in byCat.get(slug, [])) for slug in SLUGS}
    for slug in SLUGS:
        items = sorted(byCat.get(slug, []))
        for k, (centroid, stem) in enumerate(items):
            c = SLUGS.index(slug)
            if slug == "nasa":
                name = stem.replace(" - ", " ")
            else:
                name = "%s %s" % (ADJ[min(k, 3)] if len(items) <= 4 else "#%d" % (k + 1), NAMES[c])
            out.append((NAMES[c], name, c, stem, shipped[slug].index(stem), -1, "", 0, text_of(slug, centroid, slug == "nasa"),
                        STYLE[slug]))
    for name, a, b in SCENES:
        ia, ib = SLUGS.index(a), SLUGS.index(b)
        sa = sorted(byCat.get(a, []))
        sb = sorted(byCat.get(b, []))
        if not sa or not sb:
            continue
        # A: the category's warmer middle recording, B: its brighter one -- two places would not blend as well
        # as a ground and a detail above it.
        stemA = sa[min(1, len(sa) - 1)][1]
        stemB = sb[min(2, len(sb) - 1)][1]
        text = text_of(a, max(sa[min(1, len(sa) - 1)][0], sb[min(2, len(sb) - 1)][0]), False)
        text += "\\nb_level=-4\\nlayer_mix=0.5\\nb_start_random=1"
        style = tuple(max(x, y) for x, y in zip(STYLE[a], STYLE[b]))
        out.append(("Scenes", name, ia, stemA, shipped[a].index(stemA), ib, stemB, shipped[b].index(stemB), text, style))

    lines = ["// Generated by Tools/field_presets.py from Tools/field_selection.json -- do not edit.",
             "// group, name, category A, file A, its shipped variation, category B (-1: none), file B, its shipped variation,",
             "// settings, style weights (StyleId order)"]
    for g, n, ca, sa, va, cb, sb, vb, t, st in out:
        lines.append("{ %s, %s, %d, %s, %d, %d, %s, %d, %s, { %s } }," % (c_str(g), c_str(n), ca, c_str(sa), va, cb, c_str(sb), vb,
                                                                          '"' + t + '"', ", ".join("%.2ff" % x for x in st)))
    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print("%d presets -> %s" % (len(out), OUT))


if __name__ == "__main__":
    main()
