#!/usr/bin/env python3
"""The Phosphene logo (23.09.2026): three concepts as SVG, rendered to PNG, and a sheet to compare them.

    python Tools/logo/make_logo.py                    # writes docs/logo/
    python Tools/logo/make_logo.py --install rings    # and puts the chosen one into the builds (install())

A phosphene is the light one sees with the eyes closed -- rings, dots, a glow that pulses. The three concepts
take that literally, each with the plugin's own colours (Plugin/PhospheneLookAndFeel.h: accent #5ad1ff, warm
#ff9a4d, the dark #10121a):

* **rings**  -- the launcher icon's idea drawn properly: four rings of glowing points with 8, 16, 24 and 32 of
                them (the bar's quarters, eighths, triplets and sixteenths), warm at the core and cyan outside.
* **lid**    -- a closed eye with a fan of light rising above the lid: what the name means.
* **pulse**  -- a ring whose radius is the psytrance bar itself, sixteen steps with the kick on every fourth,
                around a glowing core: a sun, an iris and a waveform at once.

The wordmark is drawn here from strokes (P, H, O, S, E, N), not set in a system font: a logo that is traced
from a licensed font is a licence question, a logo drawn from lines is not. Everything is plain SVG; the PNGs
are rendered by Edge in headless mode (the one SVG renderer every Windows machine here has), with a
transparent background.
"""
from __future__ import annotations

import math
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "logo"
EDGE = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"

BG = "#10121a"
TEXT = "#e4e8f4"
ACCENT = "#5ad1ff"
VIOLET = "#9b7bff"
MAGENTA = "#e07bff"
WARM = "#ff9a4d"
CORE = "#fff4dd"


def lerp_colour(a: str, b: str, t: float) -> str:
    a = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    b = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(a, b))


def ramp(t: float) -> str:
    """Warm at 0, magenta, violet, cyan at 1."""
    stops = [WARM, MAGENTA, VIOLET, ACCENT]
    t = min(max(t, 0.0), 1.0) * (len(stops) - 1)
    i = min(int(t), len(stops) - 2)
    return lerp_colour(stops[i], stops[i + 1], t - i)


def defs_common() -> str:
    return f"""
  <filter id="glow" x="-50%" y="-50%" width="200%" height="200%">
    <feGaussianBlur stdDeviation="6" result="b"/>
    <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
  </filter>
  <filter id="soft" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="14"/></filter>
  <radialGradient id="core" cx="0.5" cy="0.5" r="0.5">
    <stop offset="0" stop-color="{CORE}"/><stop offset="0.35" stop-color="{WARM}" stop-opacity="0.9"/>
    <stop offset="1" stop-color="{WARM}" stop-opacity="0"/>
  </radialGradient>"""


def dot(x: float, y: float, r: float, colour: str) -> str:
    return (f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{r * 1.9:.2f}" fill="{colour}" opacity="0.28"/>'
            f'<circle cx="{x:.2f}" cy="{y:.2f}" r="{r:.2f}" fill="{colour}"/>'
            f'<circle cx="{x - r * 0.25:.2f}" cy="{y - r * 0.25:.2f}" r="{r * 0.38:.2f}" fill="#ffffff" opacity="0.75"/>')


# ---------------------------------------------------------------------------------------------- the marks (512 x 512)

def mark_rings(ink: str = TEXT) -> str:
    parts = [f'<circle cx="256" cy="256" r="64" fill="url(#core)" filter="url(#soft)"/>',
             f'<circle cx="256" cy="256" r="26" fill="url(#core)"/>']
    rings = [(78, 8, 15.0), (130, 16, 11.5), (182, 24, 9.0), (230, 32, 7.0)]
    for k, (radius, count, size) in enumerate(rings):
        colour = ramp((k + 1) / len(rings))
        offset = (math.pi / count) * (k % 2)
        for i in range(count):
            a = 2 * math.pi * i / count + offset - math.pi / 2
            parts.append(dot(256 + radius * math.cos(a), 256 + radius * math.sin(a), size, colour))
    return "\n  ".join(parts)


def mark_lid(ink: str = TEXT) -> str:
    """@p ink: the lid's colour, the wordmark's (dark on a light background)."""
    parts = []
    cx, cy = 256, 318   # where the fan rises from: the middle of the lid
    parts.append(f'<ellipse cx="{cx}" cy="{cy - 10}" rx="120" ry="70" fill="url(#core)" filter="url(#soft)"/>')
    # Four arcs of light over the lid, more points the further out (5, 7, 9, 11), inside 205..335 degrees so that
    # none of them touches the lid's ends.
    for j, (radius, count, size) in enumerate([(92, 5, 12.5), (140, 7, 10.0), (186, 9, 8.0), (230, 11, 6.2)]):
        colour = ramp(0.25 + 0.75 * j / 3)
        for i in range(count):
            a = math.radians(205 + 130 * i / (count - 1))
            parts.append(dot(cx + radius * math.cos(a), cy + radius * math.sin(a) * 0.95, size, colour))
    # The closed lid: a soft arc, and five short lashes below it.
    parts.append(f'<path d="M 70 {cy} Q 256 {cy + 118} 442 {cy}" fill="none" stroke="{ink}" stroke-width="15" stroke-linecap="round"/>')
    for t in (0.18, 0.34, 0.5, 0.66, 0.82):
        x = (1 - t) ** 2 * 70 + 2 * (1 - t) * t * 256 + t * t * 442
        y = (1 - t) ** 2 * cy + 2 * (1 - t) * t * (cy + 118) + t * t * cy
        dx = 2 * (1 - t) * (256 - 70) + 2 * t * (442 - 256)
        dy = 2 * (1 - t) * 118 + 2 * t * (-118)
        n = math.hypot(dx, dy)
        nx, ny = -dy / n, dx / n   # the normal pointing down
        if ny < 0:
            nx, ny = -nx, -ny
        parts.append(f'<line x1="{x + nx * 10:.1f}" y1="{y + ny * 10:.1f}" x2="{x + nx * 34:.1f}" y2="{y + ny * 34:.1f}" '
                     f'stroke="{ink}" stroke-width="11" stroke-linecap="round"/>')
    return "\n  ".join(parts)


def mark_pulse(ink: str = TEXT) -> str:
    parts = [f'<circle cx="256" cy="256" r="80" fill="url(#core)" filter="url(#soft)"/>',
             f'<circle cx="256" cy="256" r="36" fill="url(#core)"/>']
    # The bar as a ring: sixteen steps, each a note with a sharp attack and an exponential decay, the kick on every
    # fourth taller than the rolling bass between.
    pts = []
    n = 960
    for i in range(n + 1):
        u = i / n
        step = u * 16
        s = int(step) % 16
        ph = step - int(step)
        amp = 1.0 if s % 4 == 0 else 0.45
        env = amp * math.sin(math.pi * ph) ** 2   # a smooth swell per step: the pulse, not a saw blade
        r = 158 + 52 * env
        a = 2 * math.pi * u - math.pi / 2
        pts.append((256 + r * math.cos(a), 256 + r * math.sin(a)))
    d = "M " + " L ".join(f"{x:.2f} {y:.2f}" for x, y in pts) + " Z"
    parts.append('<linearGradient id="ring" x1="0" y1="0" x2="1" y2="1">'
                 f'<stop offset="0" stop-color="{ACCENT}"/><stop offset="0.55" stop-color="{VIOLET}"/><stop offset="1" stop-color="{MAGENTA}"/></linearGradient>')
    parts.append(f'<path d="{d}" fill="none" stroke="url(#ring)" stroke-width="12" stroke-linejoin="round" filter="url(#glow)"/>')
    parts.append(f'<circle cx="256" cy="256" r="112" fill="none" stroke="{ACCENT}" stroke-opacity="0.35" stroke-width="3"/>')
    for i in range(16):
        a = 2 * math.pi * i / 16 - math.pi / 2
        size = 7.5 if i % 4 == 0 else 4.5
        parts.append(dot(256 + 112 * math.cos(a), 256 + 112 * math.sin(a), size, ACCENT if i % 4 == 0 else VIOLET))
    return "\n  ".join(parts)


def mark_rings_small(ink: str = TEXT) -> str:
    """The rings for 16 to 48 pixels: the full mark's 80 points become a speckle there, so the small one keeps the
    core, eight large points and twelve smaller ones -- the same idea, drawn for the size it is seen at."""
    parts = [f'<circle cx="256" cy="256" r="92" fill="url(#core)" filter="url(#soft)"/>',
             f'<circle cx="256" cy="256" r="46" fill="url(#core)"/>']
    for k, (radius, count, size, t) in enumerate([(138, 8, 32.0, 0.45), (218, 12, 24.0, 1.0)]):
        offset = (math.pi / count) * (k % 2)
        for i in range(count):
            a = 2 * math.pi * i / count + offset - math.pi / 2
            parts.append(dot(256 + radius * math.cos(a), 256 + radius * math.sin(a), size, ramp(t)))
    return "\n  ".join(parts)


MARKS = {"rings": mark_rings, "lid": mark_lid, "pulse": mark_pulse}

# ---------------------------------------------------------------------------------------------- the wordmark

# Letters on a cap height of 100, drawn as strokes: (path, advance width).
LETTERS = {
    "P": ("M 0 100 V 0 H 38 A 26 26 0 0 1 38 52 H 0", 64),
    "H": ("M 0 0 V 100 M 60 0 V 100 M 0 50 H 60", 60),
    "O": ("M 42 0 A 42 50 0 1 0 42.01 0 Z", 84),
    "S": ("M 58 16 C 51 5 40 0 30 0 C 14 0 2 10 2 25 C 2 41 16 46 30 50 C 45 54 58 59 58 75 C 58 90 46 100 30 100 C 19 100 8 95 2 84", 60),
    "E": ("M 58 0 H 0 V 100 H 58 M 0 50 H 46", 58),
    "N": ("M 0 100 V 0 L 60 100 V 0", 60),
}
TRACKING = 36


def wordmark(x0: float, y0: float, scale: float, colour: str, word: str = "PHOSPHENE") -> tuple[str, float]:
    parts, x = [], 0.0
    for ch in word:
        path, adv = LETTERS[ch]
        parts.append(f'<path transform="translate({x0 + x * scale:.2f} {y0:.2f}) scale({scale:.4f})" d="{path}" fill="none" '
                     f'stroke="{colour}" stroke-width="{11 / 1:.1f}" stroke-linecap="round" stroke-linejoin="round"/>')
        x += adv + TRACKING
    return "\n  ".join(parts), (x - TRACKING) * scale


# ---------------------------------------------------------------------------------------------- files

def svg(w: int, h: int, body: str, background: str | None = None, view: str | None = None) -> str:
    bg = f'<rect width="100%" height="100%" fill="{background}"/>' if background else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="{view or f"0 0 {w} {h}"}">\n'
            f"<defs>{defs_common()}\n</defs>\n{bg}\n  {body}\n</svg>\n")


def app_icon(name: str, small: bool = False) -> str:
    """The mark on the plugin's dark rounded square, for launchers and the Windows icon."""
    mark = mark_rings_small() if small else MARKS[name]()
    body = (f'<rect x="0" y="0" width="512" height="512" rx="112" fill="{BG}"/>'
            f'<g transform="translate(40 40) scale(0.84375)">{mark}</g>')
    return svg(512, 512, body)


def lockup(name: str, text: str, background: str | None) -> tuple[str, int, int]:
    w, h = 1480, 360
    words, width = wordmark(0, 0, 1.0, text)
    mark = f'<g transform="translate(20 20) scale({320 / 512:.4f})">{MARKS[name](text)}</g>'
    words, width = wordmark(400, 130, 1.0, text)
    return svg(w, h, mark + "\n  " + words, background), w, h


def render(svg_path: Path, png_path: Path, w: int, h: int) -> None:
    """SVG to PNG with headless Edge. Edge now and then exits without writing the file when it is started again
    right after itself, so it is asked up to five times."""
    import time
    if png_path.exists():
        png_path.unlink()
    for _ in range(5):
        subprocess.run([EDGE, "--headless=new", "--disable-gpu", "--hide-scrollbars", f"--window-size={w},{h}",
                        "--default-background-color=00000000", f"--screenshot={png_path}", svg_path.resolve().as_uri()],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if png_path.exists():
            return
        time.sleep(1.0)
    raise RuntimeError(f"Edge wrote no {png_path}")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for name in MARKS:
        d = OUT / name
        d.mkdir(exist_ok=True)
        files = {
            "mark.svg": (svg(512, 512, MARKS[name]()), 512, 512),
            "icon.svg": (app_icon(name), 512, 512),
        }
        lk_dark, w, h = lockup(name, TEXT, BG)
        lk_light, _, _ = lockup(name, BG, "#f4f6fb")
        lk_clear, _, _ = lockup(name, TEXT, None)
        files["logo-dark.svg"] = (lk_dark, w, h)
        files["logo-light.svg"] = (lk_light, w, h)
        files["logo.svg"] = (lk_clear, w, h)
        lk_ink, _, _ = lockup(name, BG, None)
        files["logo-ink.svg"] = (lk_ink, w, h)   # transparent, dark text: for light pages (the README in light mode)
        if name == "rings":
            files["icon-small.svg"] = (app_icon(name, small=True), 512, 512)
        for fn, (text, fw, fh) in files.items():
            p = d / fn
            p.write_text(text, encoding="utf-8")
            render(p, p.with_suffix(".png"), fw, fh)
        print(f"{name}: {', '.join(files)}")
    # The comparison sheet, put together from the rendered PNGs: each concept as a lockup, as an app icon, and the
    # icon at 64, 32 and 16 pixels -- the sizes a taskbar, a file list and a browser tab show.
    from PIL import Image, ImageDraw
    sheet = Image.new("RGBA", (1240, 40 + 260 * len(MARKS)), (11, 13, 19, 255))
    draw = ImageDraw.Draw(sheet)
    y = 40
    for name in MARKS:
        draw.text((40, y), name, fill=(228, 232, 244, 255))
        logo = Image.open(OUT / name / "logo-dark.png").convert("RGBA").resize((740, 180), Image.LANCZOS)
        icon = Image.open(OUT / name / "icon.png").convert("RGBA")
        sheet.alpha_composite(logo, (40, y + 30))
        sheet.alpha_composite(icon.resize((180, 180), Image.LANCZOS), (820, y + 30))
        x = 1030
        for size in (64, 32, 16):
            sheet.alpha_composite(icon.resize((size, size), Image.LANCZOS), (x, y + 30 + (180 - size) // 2))
            x += size + 24
        y += 260
    sheet.save(OUT / "compare.png")
    print(f"sheet: {OUT / 'compare.png'}")
    return 0


def install(name: str) -> None:
    """Puts concept @p name where the builds read their icons (23.09.2026: the user chose "rings").

    - Quest/res/mipmap-*/ic_launcher.png -- the headset's launcher, five densities, from the full icon (it is seen
      large in VR);
    - Deploy/phosphene.ico -- installer and shortcut; 16 to 48 pixels from the small icon, 64 to 256 from the full;
    - Plugin/Resources/icon.png and icon-small.png -- the standalone's window and taskbar icon (JUCE ICON_BIG and
      ICON_SMALL, Plugin/CMakeLists.txt).
    """
    from PIL import Image
    d = OUT / name
    full = Image.open(d / "icon.png").convert("RGBA")
    small_src = d / "icon-small.png"
    small = Image.open(small_src).convert("RGBA") if small_src.exists() else full
    for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
        full.resize((size, size), Image.LANCZOS).save(ROOT / "Quest" / "res" / f"mipmap-{density}" / "ic_launcher.png")
    sizes = [16, 24, 32, 48, 64, 128, 256]
    images = [(small if s <= 48 else full).resize((s, s), Image.LANCZOS) for s in sizes]
    images[-1].save(ROOT / "Deploy" / "phosphene.ico", format="ICO", sizes=[(s, s) for s in sizes], append_images=images[:-1])
    res = ROOT / "Plugin" / "Resources"
    res.mkdir(exist_ok=True)
    full.save(res / "icon.png")
    small.resize((64, 64), Image.LANCZOS).save(res / "icon-small.png")
    print(f"installed {name}: Quest launcher (5 densities), Deploy/phosphene.ico ({len(sizes)} sizes), Plugin/Resources")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "--install":
        sys.exit(main() or install(sys.argv[2]) or 0)
    sys.exit(main())
