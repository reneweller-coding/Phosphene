"""Phosphene -- the Windows icon, made from the headset app's launcher icon.

    python Deploy/make_icon.py

The Quest app already carries a launcher icon at five densities (Quest/res/mipmap-*/ic_launcher.png);
the Windows side had none, so the installer had nothing to put on the setup and the shortcut showed
the generic one. Rather than draw a second picture that would drift away from the first, the icon is
derived from the largest mipmap.

Why a script and not just a committed .ico: the .ico *is* committed (Deploy/phosphene.ico) because
the release build must not depend on Python being installed. This file is here so that the next
person can see where those pixels came from and remake them if the launcher icon changes.

An .ico holds several sizes; Windows picks per context (16 in a title bar, 32 in a shortcut, 256 in
the large-icon view of Explorer). Anything above 48 is stored PNG-compressed, which is what Pillow
does on its own and what every Windows since Vista reads.
"""
import os
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
SRC = os.path.join(ROOT, "Quest", "res", "mipmap-xxxhdpi", "ic_launcher.png")
DST = os.path.join(HERE, "phosphene.ico")
SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]

if not os.path.isfile(SRC):
    sys.exit("no launcher icon at %s" % SRC)

img = Image.open(SRC).convert("RGBA")
# Square it off before scaling: a non-square source would otherwise be squashed into every entry.
if img.width != img.height:
    side = min(img.width, img.height)
    left = (img.width - side) // 2
    top = (img.height - side) // 2
    img = img.crop((left, top, left + side, top + side))
img.save(DST, format="ICO", sizes=SIZES)
print("%s  %d bytes, %d sizes, from %dx%d" % (DST, os.path.getsize(DST), len(SIZES), img.width, img.height))
