#!/bin/bash
# Builds GeometryView for other people's Macs: the app, signed with a Developer ID, notarized and
# stapled, zipped for a GitHub release; and the native engine, signed and notarized the same way,
# packed into the Claude Desktop extension. Results go in build/release-dist.
#
#   Tools/release/mac_release.sh           the real thing (needs the setup below)
#   Tools/release/mac_release.sh --check   builds and signs ad hoc, verifies, stops before notarizing
#
# Once, with an Apple Developer Program membership:
#   1. Xcode ▸ Settings ▸ Accounts ▸ Manage Certificates ▸ + ▸ Developer ID Application.
#   2. xcrun notarytool store-credentials geometryview --apple-id <email> --team-id <team>
#      (it asks for an app-specific password, made at account.apple.com).
#   3. export GEOMETRYVIEW_SIGN_ID="Developer ID Application: <name> (<team>)"
#      export GEOMETRYVIEW_NOTARY_PROFILE=geometryview
set -euo pipefail
cd "$(dirname "$0")/../.."

check=false
[[ "${1:-}" == "--check" ]] && check=true
version=$(node -p 'require("./Launcher/package.json").version')

if $check; then
  identity=-
  timestamp=--timestamp=none
else
  identity=${GEOMETRYVIEW_SIGN_ID:?Set GEOMETRYVIEW_SIGN_ID to your Developer ID Application identity}
  profile=${GEOMETRYVIEW_NOTARY_PROFILE:?Set GEOMETRYVIEW_NOTARY_PROFILE to your notarytool profile}
  timestamp=--timestamp
  security find-identity -v -p codesigning | grep -qF "$identity" ||
    { echo "No signing identity named $identity in the keychain." >&2; exit 1; }
fi

cmake --preset release >/dev/null
cmake --build --preset release --target GeometryView gcli geometryview-occt-worker
rm -rf build/release-dist
mkdir -p build/release-dist/native
app=build/release-dist/GeometryView.app
ditto build/release/App/GeometryView.app "$app"

# Inside out: the helpers, then the app, each with the hardened runtime. The helpers keep the
# sandbox they inherit from the app.
codesign --force --sign "$identity" --options runtime $timestamp \
  --entitlements App/Resources/Helper.entitlements \
  "$app/Contents/MacOS/geometryview-import" "$app/Contents/MacOS/geometryview-occt-worker"
codesign --force --sign "$identity" --options runtime $timestamp \
  --entitlements App/Resources/GeometryView.entitlements "$app"
codesign --verify --deep --strict --verbose=2 "$app"

# The native engine the launcher runs, unsandboxed: gcli, the OCCT worker and the shaders.
native=build/release-dist/native
cp build/release/Tools/gcli build/release/Import/geometryview-occt-worker build/release/shaders/default.metallib "$native/"
codesign --force --sign "$identity" --options runtime $timestamp "$native/gcli" "$native/geometryview-occt-worker"
codesign --verify --strict --verbose=2 "$native/gcli" "$native/geometryview-occt-worker"

if $check; then
  codesign --display --entitlements - "$app" 2>/dev/null | grep -E "key|true" | head -20
  echo "Checked: built and signed ad hoc in build/release-dist. Notarizing needs a Developer ID."
  exit 0
fi

# Notarize and staple the app, then zip it as people download it.
ditto -c -k --keepParent "$app" build/release-dist/submit-app.zip
xcrun notarytool submit build/release-dist/submit-app.zip --keychain-profile "$profile" --wait
xcrun stapler staple "$app"
spctl --assess --type execute --verbose=2 "$app"
ditto -c -k --keepParent "$app" "build/release-dist/GeometryView-$version-mac.zip"

# Notarize the engine: bare executables can't be stapled, so Gatekeeper checks them online.
ditto -c -k "$native" build/release-dist/submit-native.zip
xcrun notarytool submit build/release-dist/submit-native.zip --keychain-profile "$profile" --wait
GEOMETRYVIEW_PACK_NATIVE="$PWD/$native" node Launcher/scripts/pack.mjs
cp Launcher/dist/geometryview.mcpb "build/release-dist/geometryview-$version-mac.mcpb"
rm -f build/release-dist/submit-app.zip build/release-dist/submit-native.zip
ls -la build/release-dist
