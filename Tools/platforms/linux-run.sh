#!/bin/sh
# Inside the Linux container: starts the census and viewer servers, then runs one headless check.
#   linux-run.sh <url path, e.g. /?label=x on the census, or :7342/viewer/?...> [headless options]
# GEOMETRY_ADAPTER chooses the software WebGPU adapter: swiftshader (default) or lavapipe.
set -e
node Web/census/serve.mjs 7341 >/tmp/census.log 2>&1 &
node Web/spike/serve.mjs 7342 >/tmp/spike.log 2>&1 &
sleep 1
case "${GEOMETRY_ADAPTER:-swiftshader}" in
  swiftshader) adapter="--use-webgpu-adapter=swiftshader" ;;
  lavapipe) adapter="--use-vulkan=native" ;;
esac
export GEOMETRY_CHROME_FLAGS="--no-sandbox --disable-dev-shm-usage --enable-features=Vulkan $adapter $GEOMETRY_CHROME_FLAGS"
url="$1"; shift
# Headless Chromium on Linux cannot present WebGPU to a canvas; a window on a virtual display can.
if [ "${GEOMETRY_HEADFUL:-1}" = "1" ]; then
  export GEOMETRY_HEADFUL=1 DISPLAY=:99
  Xvfb :99 -screen 0 1920x1200x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
  sleep 1
fi
node Tools/platforms/headless.mjs "http://127.0.0.1$url" "$@"
