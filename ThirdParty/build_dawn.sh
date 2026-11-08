#!/usr/bin/env bash
# Builds the pinned Dawn, Google's WebGPU implementation, for Metal only, into ThirdParty/.dawn.
# Phase 0's renderer spike runs natively against it: the control that separates WebGPU's own cost
# from the browser's, and the Mac's measure for moving onto WebGPU (decision 8). The release is
# the one Emscripten 7.0's emdawnwebgpu port is built from, so native and web share one webgpu.h.
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
tag="v20260423.175430"
commit="b975919dfb45406ca17162e4d44c74a650caa679"

work="$root/.dawn"
source="$work/src"
build="$work/build"
install="$work/install"

if [[ ! -d "$source" ]]; then
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" \
        https://github.com/google/dawn.git "$source"
fi
actual="$(git -C "$source" rev-parse HEAD)"
if [[ "$actual" != "$commit" ]]; then
    echo "error: Dawn $tag resolved to $actual, expected $commit" >&2
    exit 1
fi

# Dawn fetches its own pinned dependencies (abseil, SPIR-V headers and tools, and so on).
cmake -S "$source" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_INSTALL_PREFIX="$install" \
    -DDAWN_FETCH_DEPENDENCIES=ON \
    -DDAWN_ENABLE_INSTALL=ON \
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=SHARED \
    -DDAWN_ENABLE_METAL=ON \
    -DDAWN_ENABLE_VULKAN=OFF \
    -DDAWN_ENABLE_NULL=OFF \
    -DDAWN_ENABLE_DESKTOP_GL=OFF \
    -DDAWN_ENABLE_OPENGLES=OFF \
    -DDAWN_USE_GLFW=OFF \
    -DDAWN_BUILD_SAMPLES=OFF \
    -DDAWN_BUILD_TESTS=OFF \
    -DDAWN_BUILD_BENCHMARKS=OFF \
    -DTINT_BUILD_TESTS=OFF \
    -DTINT_BUILD_CMD_TOOLS=OFF \
    -DTINT_BUILD_SPV_READER=OFF \
    -DTINT_BUILD_SPV_WRITER=OFF \
    -DTINT_BUILD_GLSL_WRITER=OFF \
    -DTINT_BUILD_GLSL_VALIDATOR=OFF \
    -DTINT_BUILD_HLSL_WRITER=OFF
cmake --build "$build"
cmake --install "$build"
echo "Dawn $tag installed in $install."
