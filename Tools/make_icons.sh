#!/bin/zsh
# Makes the icons from App/Resources/AppIcon.png, a 1024 px master: the GP52C flange rendered in
# Studio by gcli at azimuth 45° and elevation 32°, on a rounded charcoal tile.
#
#   Tools/make_icons.sh <out.icns>     the app's icon, as the build makes it
#   Tools/make_icons.sh --web          the web viewer's icons, in Web/viewer/icons
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
out=${1:?usage: make_icons.sh <out.icns> | --web}
set=${out:r}.iconset
mkdir -p $set
for size in 16 32 128 256 512; do
  sips -z $size $size $master --out $set/icon_${size}x${size}.png >/dev/null
  sips -z $((size * 2)) $((size * 2)) $master --out $set/icon_${size}x${size}@2x.png >/dev/null
done
iconutil -c icns $set -o $out
