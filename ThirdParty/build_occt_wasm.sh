#!/usr/bin/env bash
# Builds the pinned OCCT as WebAssembly, with only what STEP reading and meshing need, into
# ThirdParty/.occt-wasm, for STEP import in Web Workers (Phase 0, W2). It reuses the source that
# build_occt.sh checked out, so run that first.
#
# Exceptions are WebAssembly's own, never JavaScript's, which are far slower. OCCT's conversion of
# signals into exceptions is off, since a worker cannot take signals; a trap ends the worker instead,
# and the import driver retries the face, as it does for a crashed process.
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
source="$root/.occt/src"
work="$root/.occt-wasm"
build="$work/build"
install="$work/install"

if [[ ! -d "$source" ]]; then
    echo "error: no OCCT source in $source; run ThirdParty/build_occt.sh first" >&2
    exit 1
fi

flags="-O3 -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -UOCC_CONVERT_SIGNALS"
# OCCT 8.0.1 uses std::lock_guard without including <mutex>, which Apple's libc++ includes on its
# behalf and Emscripten's does not.
emcmake cmake -S "$source" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$flags" \
    -DCMAKE_CXX_FLAGS="$flags -include mutex" \
    -DCMAKE_INSTALL_PREFIX="$install" \
    -DBUILD_LIBRARY_TYPE=Static \
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

echo "OCCT for WebAssembly installed at $install"
