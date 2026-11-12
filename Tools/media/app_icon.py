"""The app icon's master, App/Resources/AppIcon.png: four nesting dolls cut in half and seen head-on,
rings within rings, in Illustration on a charcoal tile. GeometryView draws it from the showcase's
dolls (Tools/showcase/library/dolls.py), in macOS's 1024 px template: an 824 px tile with rounded
corners over a soft shadow, which macOS 26 masks and edges with glass itself.

    python3 Tools/media/app_icon.py [out.png]       App/Resources/AppIcon.png by default

It needs a release build (gcli, its OCCT worker and the shaders) and the showcase's folder
(Tools/showcase/make.py library). Then `Tools/make_icons.sh --web` and `--ios` make the browser
viewer's and the iPhone shell's icons from the master; the Mac app's is made from it as it builds.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build/release"
GCLI, SHADERS = BUILD / "Tools/gcli", BUILD / "shaders/default.metallib"
WORKER = BUILD / "Import/geometryview-occt-worker"
DOLLS = ROOT / "build/showcase/library/Things That Come in Sizes/06_nesting_dolls/matryoshka/004 Matryoshka 4.step"
WORK = ROOT / "build/media/icon-work"

TILE, CANVAS, RADIUS = 824, 1024, 185   # macOS's icon grid
FILL = 0.76                             # how much of the tile the dolls fill
CHARCOAL = (34, 35, 40)
VIEW = "--az 90 --el 8 --section x0.5"  # the cut seen head-on


def render(blob: Path, png: Path, flags: str, size: str) -> dict:
    out = subprocess.run([str(GCLI), "render", str(SHADERS), "--batch", "--size", size, "--supersample", "2",
                          "--report"], input=f'"{blob}" "{png}" {flags}\n', text=True, capture_output=True,
                         check=True).stdout
    return json.loads(out.splitlines()[0])


def ground() -> Image.Image:
    """The charcoal tile, a little lighter at the top, as icons are lit."""
    tile = Image.new("RGBA", (TILE, TILE))
    d = ImageDraw.Draw(tile)
    for y in range(TILE):
        k = 1.08 - 0.14 * y / TILE
        d.line((0, y, TILE, y), fill=tuple(min(255, int(c * k)) for c in CHARCOAL) + (255,))
    return tile


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "App/Resources/AppIcon.png"
    WORK.mkdir(parents=True, exist_ok=True)
    blob = WORK / "dolls.geoblob"
    subprocess.run([str(GCLI), "import", str(DOLLS), str(blob), str(WORKER), "2"], check=True,
                   stdout=subprocess.DEVNULL)
    # Frame the dolls so they fill FILL of the tile, then draw them on a clear ground, unshadowed.
    probe = render(blob, WORK / "probe.png", VIEW, "1000x1000")
    x, y, w, h = probe["part_rect"]
    wpp = probe["world_per_pixel"] * max(w, h) / (FILL * TILE)
    shot = render(blob, WORK / "dolls.png", f"{VIEW} --shadow off --transparent --world-per-pixel {wpp:.6f}",
                  "1400x1400")
    x, y, w, h = shot["part_rect"]
    cx, cy = x + w / 2, y + h / 2
    dolls = Image.open(WORK / "dolls.png").convert("RGBA").crop(
        (round(cx - TILE / 2), round(cy - TILE / 2), round(cx + TILE / 2), round(cy + TILE / 2)))
    tile = ground()
    tile.alpha_composite(dolls)
    mask = Image.new("L", (TILE, TILE), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, TILE - 1, TILE - 1), RADIUS, fill=255)
    tile.putalpha(mask)
    m = (CANVAS - TILE) // 2
    icon = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    shadow = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((m, m + 12, m + TILE, m + TILE + 12), RADIUS, fill=(0, 0, 0, 90))
    icon.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14)))
    icon.alpha_composite(tile, (m, m))
    icon.save(out, optimize=True)
    print(f"{out}: {CANVAS} x {CANVAS}, {out.stat().st_size / 1e3:.0f} KB")


if __name__ == "__main__":
    main()
