"""Phosphene -- the Windows icon. Since 23.09.2026 it comes from the logo, not from the launcher icon.

    python Deploy/make_icon.py        # the same as: python Tools/logo/make_logo.py --install rings

Until that date the .ico was derived from the Quest launcher icon's largest mipmap. The logo round replaced both
with one source, Tools/logo/make_logo.py: the ".ico" now holds the rings drawn for small sizes at 16 to 48 pixels
(the full mark's 80 points turn into a speckle there) and the full rings from 64 up. Deriving it from the
launcher again, as this script used to, would throw the small drawing away -- so it only calls the logo script.

The .ico stays committed (Deploy/phosphene.ico): the release build must not depend on Python being installed.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.exit(subprocess.call([sys.executable, str(ROOT / "Tools" / "logo" / "make_logo.py"), "--install", "rings"]))
