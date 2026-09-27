"""Phosphene -- the picture GitHub shows when the project is linked: docs/social-preview.png (1280 x 640).

    python Tools/make_preview.py

As for Noctuary (AmbientSynth/Tools/make_preview.py) and Ephemeris (BerlinSchoolGenerator/Tools/manual/
make_preview.py): the icon, the name and one sentence on the left, and on the right four of the README's
pictures in a 2 x 2 grid -- the Set tab, the arrange timeline, the mixer and a voice. The pictures are the
plugin's own (docs/screenshots, written by PHOS_MANUAL); a tab taller than the window is cut at the window's
height. GitHub takes the result by hand: Settings, General, Social preview.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
W, H = 1280, 640
BG = (11, 10, 18)            # the panel's own background (phosui::bg0)
NAME = (164, 107, 255)       # the plugin's colour, UV violet (phosui::accent)
TEXT = (236, 234, 246)
SMALL = (163, 157, 194)
FRAME = (43, 38, 65)
SHOTS = ["tab-0-set.png", "tab-1-arrange.png", "tab-14-mixer-master.png", "tab-10-pad.png"]


def font(name, size):
    try:
        return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)
    except OSError:
        return ImageFont.load_default()


img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)

# The grid: four window-sized pictures (1290 x 860 at design size), each cut to the window.
gx0, gap = 452, 10
cw = (W - gx0 - 26 - gap) // 2
ch = int(cw * 860 / 1290)
gy0 = (H - (2 * ch + gap)) // 2
for i, name in enumerate(SHOTS):
    shot = Image.open(os.path.join(ROOT, "docs", "screenshots", name)).convert("RGB")
    shot = shot.crop((0, 0, shot.width, min(shot.height, int(shot.width * 860 / 1290))))
    tile = shot.resize((cw, ch), Image.LANCZOS)
    x, y = gx0 + (i % 2) * (cw + gap), gy0 + (i // 2) * (ch + gap)
    img.paste(tile, (x, y))
    d.rectangle([x - 1, y - 1, x + cw, y + ch], outline=FRAME, width=2)

icon = Image.open(os.path.join(ROOT, "docs", "logo", "rings", "icon.png")).convert("RGBA").resize((150, 150), Image.LANCZOS)
img.paste(icon, (52, 70), icon)
d.text((54, 236), "PHOSPHENE", font=font("segoeuib.ttf", 46), fill=NAME)
y = 312
for line in ("Composes and synthesizes", "complete psytrance sets:", "Goa, Full-On, Progressive,", "Dark Forest, Hi-Tech."):
    d.text((56, y), line, font=font("segoeui.ttf", 24), fill=TEXT)
    y += 34
for line in ("Standalone and VST3 for Windows,", "Meta Quest -- free and open source"):
    d.text((56, y + 16), line, font=font("segoeui.ttf", 18), fill=SMALL)
    y += 25

out = os.path.join(ROOT, "docs", "social-preview.png")
img.save(out, optimize=True)
print("wrote", os.path.relpath(out, ROOT), img.size)
