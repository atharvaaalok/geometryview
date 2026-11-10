#!/bin/zsh
# Pictures the Mac app's window with a model open and its structure showing, for the README:
#   Tools/media/mac_window.sh <model> <out.png> [WxH]
# The app's saved settings are exported first and imported after, so its look, its Recent list
# and its folders are as they were. Uses the release build.
set -e
root=${0:A:h:h:h}
app=$root/build/release/App/GeometryView.app
prefs=$HOME/Library/Containers/com.aalok.geometryview/Data/Library/Preferences/com.aalok.geometryview
picture=$HOME/Library/Containers/com.aalok.geometryview/Data/tmp/chrome.png
saved=$(mktemp -t geometryview-prefs).plist
defaults export $prefs $saved
rm -f $picture
open -n -a $app ${1:A} --args --snapshot --card structure --window ${3:-1440x900}
for i in {1..60}; do [[ -f $picture ]] && break; sleep 0.5; done
sleep 3
pkill -f "$app/Contents/MacOS/GeometryView" 2>/dev/null || true
defaults import $prefs $saved
[[ -f $picture ]] && cp $picture $2 && echo "saved $2" || { echo "no picture"; exit 1; }
