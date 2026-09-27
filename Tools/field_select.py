"""Chooses the Field track's recordings and copies the ORIGINAL files into `Core/data/field` (27.09.2026).

    python Tools/field_select.py [field recordings folder] [archive folder]      (needs ffmpeg on the PATH)

The user: "Würde es vielleicht auch noch Sinn machen, einen Sample einzubauen, der Field Recordings abspielen kann
(insbesondere für Forest?) ... Dann sollten wir aber auch tatsächlich eine neue Spur für den Sampler spendieren", on the
size "nicht unbedingt Rücksicht auf die Größe nehmen, sondern eher nach dem, was klanglich sinnvoll ist", and on the
format "Warum willst du da irgendwas umwandeln? Nimm doch die originalen Aufnahmen?" -- so nothing here converts: the
files are copied byte for byte, and the engine decodes them itself (Core/include/phos/FieldLibrary.h, dr_flac).

What goes in:
  * From Noctuary's field-recording library (Stable Audio 3, generated for Noctuary; 36 categories, seamless loops,
    24-bit 44.1 kHz stereo) the 30 categories that belong in psytrance -- forest, night, insects, water, weather, fire,
    ritual objects, caves, machines, radio and space noise; not city, traffic, harbour, crowd murmur or media crackle.
    Per category the FOUR steadiest recordings that differ most from each other: of up to 24 candidates spread over
    the category, those whose one-second loudness never falls more than 15 dB under its median (no gap) and never
    rises more than 9 dB over it (no single loud event), then the four whose spectral centroids lie furthest apart.
  * NASA's recordings from the AmbientSynth archive that are not speech (the sounds from Mars, the mission signals,
    the Webb sonifications): longer than 25 s into `field/nasa` (atmospheres, the Field track's category "NASA"),
    up to 25 s into `field/nasa/shots` (SfxType::SpaceShot). Left out, each with its reason in the credits: the
    duplicates (the user: "Ohne Dubletten") and the LCROSS song, a third party's music.

Writes `Tools/field_selection.json` (per file: category, centroid, loudness -- what Tools/field_presets.py builds the
sampler's presets from) and `Core/data/CREDITS-field.md`. The folder `Core/data/field` is not in git (1.8 GB); the
release ships it as a component of its own.
"""
import concurrent.futures as cf
import json
import os
import shutil
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from voice_pack import nasa_urls  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "Core", "data", "field")
SELECTION = os.path.join(ROOT, "Tools", "field_selection.json")
CREDITS = os.path.join(ROOT, "Core", "data", "CREDITS-field.md")
PER_CATEGORY = 4
CANDIDATES = 24
SHOT_SECONDS = 25.0
CATEGORIES = ["rainforest", "night-country", "insects", "foliage", "wetland", "mud-bubbles", "cave", "river", "sea",
              "rain-land", "rain-roof", "wind", "underwater", "ice-snow", "desert", "fire", "geothermal", "seismic",
              "ritual-objects", "abandoned", "tunnel", "metal-creak", "dark-drone-noise", "electric", "ventilation",
              "factory", "polar-station", "radio-space", "empty-space", "grain-texture"]
NASA_FOLDERS = ["NASA/Beyond", "NASA/Historical/Missions", "NASA/Historical/Beeps", "NASA/Sonification/Webb"]
NASA_LEFT_OUT = {
    "Perseverance rover driving sol 16 16 minutes.flac":
        "the whole sixteen-minute drive; its highlights are in (the user: no duplicates)",
    "Perseverance gaseous dust removal tool.flac": "the same fourteen minutes as the filtered take, which is in",
    "Sounds from Mars rover self-noise included.flac": "the same recording as the one with the rover's self-noise filtered out",
    "InSight seismometer raw short.flac": "a slice of the full-length recording, which is in",
    "LCROSS - Water on the Moon song.flac":
        "a song by a third party (Marmie) that NASA linked to -- not a NASA work, not in the public domain",
}


def decode(path, rate=8000):
    raw = subprocess.run(["ffmpeg", "-v", "quiet", "-i", path, "-ac", "1", "-ar", str(rate), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def analyse(path):
    """One-second loudness and spectral centroid of a recording, at 8 kHz mono (cheap enough for hundreds)."""
    x = decode(path)
    n = len(x) // 8000
    if n < 2:
        return {"path": path, "seconds": len(x) / 8000.0, "gap": 0.0, "burst": 0.0, "med": -60.0, "centroid": 0.0}
    sec = x[: n * 8000].reshape(n, 8000)
    db = 10 * np.log10(np.mean(sec ** 2, axis=1) + 1e-12)
    med = np.median(db)
    spec = np.abs(np.fft.rfft(sec * np.hanning(8000), axis=1)) ** 2
    f = np.fft.rfftfreq(8000, 1 / 8000)
    centroid = float(np.median((spec @ f) / (spec.sum(axis=1) + 1e-12)))
    return {"path": path, "seconds": len(x) / 8000.0, "gap": float(med - db.min()), "burst": float(db.max() - med),
            "med": float(med), "centroid": centroid}


def spread_pick(rows, k):
    """Greedy farthest-point choice on the log centroid: k rows that differ most in brightness."""
    if len(rows) <= k:
        return rows
    logc = [np.log(max(r["centroid"], 1.0)) for r in rows]
    order = np.argsort(logc)
    chosen = [order[len(order) // 2]]
    while len(chosen) < k:
        best, bestd = None, -1.0
        for i in range(len(rows)):
            if i in chosen:
                continue
            d = min(abs(logc[i] - logc[j]) for j in chosen)
            if d > bestd:
                best, bestd = i, d
        chosen.append(best)
    return [rows[i] for i in sorted(chosen, key=lambda i: logc[i])]


def main():
    lib = sys.argv[1] if len(sys.argv) > 1 else r"G:\Tools\VRAudio\AmbientSynth\Library\FieldRecordings"
    archive = sys.argv[2] if len(sys.argv) > 2 else r"G:\Tools\VRAudio\AmbientSynth\Library\Archive"
    files = sorted(f for f in os.listdir(lib) if f.lower().endswith(".flac"))
    jobs = []
    for cat in CATEGORIES:
        mine = [f for f in files if f.startswith("fr-%s-" % cat)]
        if not mine:
            print("no recordings for", cat)
            continue
        step = max(1, len(mine) // CANDIDATES)
        jobs += [(cat, os.path.join(lib, f)) for f in mine[::step][:CANDIDATES]]
    nasa = []
    seen = set()   # a file the archive keeps in two folders is taken once
    for folder in NASA_FOLDERS:
        for f in sorted(os.listdir(os.path.join(archive, folder))):
            if f.lower().endswith(".flac") and f not in NASA_LEFT_OUT and f not in seen:
                seen.add(f)
                nasa.append((folder, os.path.join(archive, folder, f)))
    # Four decoders at a time: the machine stays usable (the user's rule).
    with cf.ThreadPoolExecutor(4) as ex:
        found = list(ex.map(lambda j: analyse(j[1]), jobs))
        nasaRows = list(ex.map(lambda j: analyse(j[1]), nasa))

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(os.path.join(OUT, "nasa", "shots"))
    rows = []
    for cat in CATEGORIES:
        cand = [r for (c, _), r in zip(jobs, found) if c == cat]
        steady = [r for r in cand if r["gap"] <= 15.0 and r["burst"] <= 9.0]
        if len(steady) < PER_CATEGORY:
            steady = sorted(cand, key=lambda r: r["gap"] + r["burst"])[:max(PER_CATEGORY, len(steady))]
        for r in spread_pick(steady, PER_CATEGORY):
            name = os.path.basename(r["path"])
            shutil.copy2(r["path"], os.path.join(OUT, name))
            rows.append({"file": name, "category": cat, "seconds": round(r["seconds"], 1), "centroid": round(r["centroid"]),
                         "loudness": round(r["med"], 1), "gap": round(r["gap"], 1), "burst": round(r["burst"], 1)})
            print("%-18s %-34s %5.0f Hz  %5.1f s" % (cat, name, r["centroid"], r["seconds"]), flush=True)
    urls = nasa_urls(os.path.join(archive, "SOURCES.md"))
    for (folder, path), r in zip(nasa, nasaRows):
        name = os.path.basename(path)
        shot = r["seconds"] <= SHOT_SECONDS
        shutil.copy2(path, os.path.join(OUT, "nasa", "shots" if shot else "", name))
        u = urls.get(name, ("", ""))
        rows.append({"file": ("nasa/shots/" if shot else "nasa/") + name, "category": "nasa-shot" if shot else "nasa",
                     "folder": folder, "seconds": round(r["seconds"], 1), "centroid": round(r["centroid"]),
                     "loudness": round(r["med"], 1), "gap": round(r["gap"], 1), "burst": round(r["burst"], 1),
                     "url": u[0], "listed": u[1]})
        print("%-18s %-34s %5.1f s" % ("nasa shot" if shot else "nasa", name[:34], r["seconds"]), flush=True)
    json.dump(rows, open(SELECTION, "w", encoding="utf-8", newline="\n"), indent=1, ensure_ascii=False)

    total = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(OUT) for f in fs)
    field = [r for r in rows if not r["category"].startswith("nasa")]
    atm = [r for r in rows if r["category"] == "nasa"]
    shots = [r for r in rows if r["category"] == "nasa-shot"]
    out = ["# Field recordings -- sources and credits", ""]
    out.append("`Core/data/field` holds %d recordings, %.2f GB, the original files unchanged (FLAC), chosen by"
               % (len(rows), total / 1e9))
    out.append("`Tools/field_select.py`: %d field recordings in %d categories, %d NASA atmospheres and %d NASA shots."
               % (len(field), len(CATEGORIES), len(atm), len(shots)))
    out.append("")
    out.append("## Field recordings")
    out.append("")
    out.append("Generated with Stable Audio 3 for Noctuary (the AmbientSynth project, `Library/FieldRecordings`), by the")
    out.append("same author as Phosphene; no third party's recording is in them. Four per category: the steadiest, most")
    out.append("different of up to 24 candidates.")
    out.append("")
    for cat in CATEGORIES:
        names = [r["file"] for r in field if r["category"] == cat]
        out.append("- %s: %s" % (cat, ", ".join("`%s`" % n for n in names)))
    out.append("")
    out.append("## NASA")
    out.append("")
    out.append("Works of the United States government, not subject to copyright (NASA Media Usage Guidelines); the Webb")
    out.append("sonifications are NASA products credited NASA, ESA, CSA, STScI. NASA's guidelines ask that its material not be")
    out.append("used to imply endorsement: Phosphene uses these recordings as sound, and nothing in it claims NASA's approval.")
    out.append("Credit: NASA. Taken from the AmbientSynth archive, whose `SOURCES.md` records where each file came from.")
    out.append("")
    for title, items in (("Atmospheres (longer than 25 s; the Field track's category NASA)", atm),
                         ("Shots (up to 25 s; the effect Space Shot)", shots)):
        out.append("### " + title)
        out.append("")
        for r in items:
            src = (" -- %s (listed on %s)" % (r["url"], r["listed"])) if r["url"] else ""
            out.append("- `%s` (%s, %.1f s)%s" % (os.path.basename(r["file"]), r["folder"], r["seconds"], src))
        out.append("")
    out.append("### Left out")
    out.append("")
    for f, why in NASA_LEFT_OUT.items():
        out.append("- `%s`: %s." % (f, why))
    out.append("")
    open(CREDITS, "w", encoding="utf-8", newline="\r\n").write("\n".join(out) + "\n")
    print("%d files, %.2f GB -> %s" % (len(rows), total / 1e9, OUT))


if __name__ == "__main__":
    main()
