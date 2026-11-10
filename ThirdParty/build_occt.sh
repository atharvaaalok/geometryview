#!/usr/bin/env bash
# Builds the pinned OCCT with only what STEP reading and meshing need, into ThirdParty/.occt.
# OCCT is linked by the import worker alone, never by the viewer.
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
tag="V8_0_1"
commit="b8f597c677811d1f9f4d8a97f5ae2825c0353a42"
library_type="${OCCT_LIBRARY_TYPE:-Static}"

work="$root/.occt"
source="$work/src"
build="$work/build"
install="$work/install"
# __FILE__, which OCCT's asserts print, names files from OCCT's root, not this machine's folders.
prefix_map="-fmacro-prefix-map=$source/=occt/"

if [[ ! -d "$source" ]]; then
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" \
        https://github.com/Open-Cascade-SAS/OCCT.git "$source"
fi
actual="$(git -C "$source" rev-parse HEAD)"
if [[ "$actual" != "$commit" ]]; then
    echo "error: OCCT $tag resolved to $actual, expected $commit" >&2
    exit 1
fi

cmake -S "$source" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_C_FLAGS="-mcpu=apple-m1 $prefix_map" \
    -DCMAKE_CXX_FLAGS="-mcpu=apple-m1 $prefix_map" \
    -DCMAKE_INSTALL_PREFIX="$install" \
    -DBUILD_LIBRARY_TYPE="$library_type" \
    -DBUILD_MODULE_FoundationClasses=OFF \
    -DBUILD_MODULE_ModelingData=OFF \
    -DBUILD_MODULE_ModelingAlgorithms=OFF \
    -DBUILD_MODULE_Visualization=OFF \
    -DBUILD_MODULE_ApplicationFramework=OFF \
    -DBUILD_MODULE_DataExchange=OFF \
    -DBUILD_MODULE_Draw=OFF \
    -DBUILD_ADDITIONAL_TOOLKITS="TKDESTEP;TKMesh" \
    -DBUILD_DOC_Overview=OFF \
    -DBUILD_SAMPLES_QT=OFF \
    -DBUILD_GTEST=OFF \
    -DUSE_TK=OFF \
    -DUSE_FREETYPE=OFF \
    -DUSE_OPENGL=OFF \
    -DUSE_GLES2=OFF \
    -DUSE_XLIB=OFF \
    -DUSE_TBB=OFF \
    -DUSE_VTK=OFF \
    -DUSE_RAPIDJSON=OFF \
    -DUSE_DRACO=OFF \
    -DUSE_FREEIMAGE=OFF \
    -DUSE_FFMPEG=OFF \
    -DUSE_OPENVR=OFF \
    -DINSTALL_SAMPLES=OFF \
    -DINSTALL_TEST_CASES=OFF
cmake --build "$build"
cmake --install "$build"

echo "OCCT $tag installed at $install"
