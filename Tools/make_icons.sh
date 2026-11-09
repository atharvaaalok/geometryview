#!/bin/zsh
# Makes the icons from App/Resources/AppIcon.png, a 1024 px master: the GP52C flange rendered in
# Studio by gcli at azimuth 45° and elevation 32°, on a rounded charcoal tile.
#
#   Tools/make_icons.sh <out.icns>     the app's icon, as the build makes it
#   Tools/make_icons.sh --web          the web viewer's icons, in Web/viewer/icons
#   Tools/make_icons.sh --ios          the iPhone shell's icon, in iOS/Shell/Assets.xcassets
set -e
root=${0:A:h:h}
master=$root/App/Resources/AppIcon.png
if [[ $1 == --web ]]; then
  icons=$root/Web/viewer/icons
  mkdir -p $icons
  for size in 32 180 192 512; do
    sips -z $size $size $master --out $icons/icon-$size.png >/dev/null
  done
  exit 0
fi
if [[ $1 == --ios ]]; then
  # iOS rounds the corners itself, so its icon is the tile alone, square and opaque: the tile
  # (824 px at 100,100 in the master) scaled to 1024, its rounded corners and their soft edge
  # filled with the backdrop's colour on that row, which the system's mask then hides.
  icon=$root/iOS/Shell/Assets.xcassets/AppIcon.appiconset/icon-1024.png
  python3 -I - $master $icon <<'PY'
import sys
from PIL import Image
tile = Image.open(sys.argv[1]).convert("RGBA").crop((100, 100, 924, 924)).resize((1024, 1024), Image.LANCZOS)
out = tile.convert("RGB")
for y in range(tile.height):
    inside = [x for x in range(tile.width) if tile.getpixel((x, y))[3] == 255]
    left, right = (inside[0], inside[-1]) if inside else (tile.width // 2, tile.width // 2)
    edge = tile.getpixel((min(left + 8, tile.width - 1), y))[:3] if inside else (0, 0, 0)
    out.paste(edge, (0, y, left + 8, y + 1))
    out.paste(edge, (right - 7, y, tile.width, y + 1))
out.save(sys.argv[2], optimize=True)
PY
  exit 0
fi
out=${1:?usage: make_icons.sh <out.icns> | --web | --ios}
set=${out:r}.iconset
mkdir -p $set
for size in 16 32 128 256 512; do
  sips -z $size $size $master --out $set/icon_${size}x${size}.png >/dev/null
  sips -z $((size * 2)) $((size * 2)) $master --out $set/icon_${size}x${size}@2x.png >/dev/null
done
iconutil -c icns $set -o $out
