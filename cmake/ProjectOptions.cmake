# Compile options shared by all first-party code.

set(GEO_SANITIZE "" CACHE STRING "Comma-separated sanitizers, for example address,undefined")

add_library(geo_options INTERFACE)

# A target sets GEO_ALLOW_EXCEPTIONS to opt out: tests, because doctest throws to stop a test case,
# and the OCCT adapter, because OCCT reports errors with exceptions.
set(geo_no_exceptions "$<NOT:$<BOOL:$<TARGET_PROPERTY:GEO_ALLOW_EXCEPTIONS>>>")

target_compile_options(geo_options INTERFACE
    $<$<AND:$<COMPILE_LANGUAGE:CXX,OBJCXX>,${geo_no_exceptions}>:-fno-exceptions>
    $<$<AND:$<COMPILE_LANGUAGE:CXX,OBJCXX>,${geo_no_exceptions}>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<AND:$<COMPILE_LANGUAGE:CXX,OBJCXX>,$<CONFIG:Release>>:-O3>
    $<$<AND:$<COMPILE_LANGUAGE:CXX,OBJCXX>,$<CONFIG:Release>>:-flto=thin>
)
target_link_options(geo_options INTERFACE
    $<$<AND:$<CONFIG:Release>,$<LINK_LANGUAGE:C,CXX,OBJCXX>>:-flto=thin>
)

# The WebGPU renderer spike of Phase 0, natively on Dawn or as WebAssembly. Off by default, so
# the app never depends on Dawn.
option(GEO_WEBGPU_SPIKE "Build the WebGPU renderer spike" OFF)
# The WebGPU renderer of the universal viewer, natively on Dawn (to test it against Metal) or as
# WebAssembly. Off by default, so the Mac app never depends on Dawn.
option(GEO_WEBGPU "Build the WebGPU renderer" OFF)

if(GEO_TARGET STREQUAL "wasm")
    # SIMD128 everywhere; relaxed SIMD and 64-bit memory are left out because Safari lacks them.
    target_compile_options(geo_options INTERFACE -msimd128)
    option(GEO_WASM_THREADS "Build WebAssembly with threads (needs cross-origin isolation)" OFF)
    if(GEO_WASM_THREADS)
        target_compile_options(geo_options INTERFACE -pthread)
        # dlmalloc, Emscripten's default, takes one lock, but the parallel stages now allocate
        # little, and it imports as fast as mimalloc did (10M triangle STL: 2.38 s against
        # 2.40 s). It reuses freed memory far better, which matters since WebAssembly memory never
        # shrinks: that import peaks at 1.23-1.37 GB of heap against mimalloc's 1.69-1.71 GB.
        target_link_options(geo_options INTERFACE -pthread -sMALLOC=dlmalloc)
    endif()
else()
    target_compile_options(geo_options INTERFACE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-mcpu=apple-m1>)
    # Scoped by linker language, because swiftc rejects these clang driver flags.
    target_link_options(geo_options INTERFACE
        $<$<AND:$<CONFIG:Release>,$<LINK_LANGUAGE:C,CXX,OBJCXX>>:-Wl,-dead_strip>)
endif()

if(GEO_SANITIZE)
    target_compile_options(geo_options INTERFACE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fsanitize=${GEO_SANITIZE}>
        $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fno-omit-frame-pointer>
    )
    # The app links with swiftc, which spells the flag differently.
    target_link_options(geo_options INTERFACE
        $<$<LINK_LANGUAGE:C,CXX,OBJCXX>:-fsanitize=${GEO_SANITIZE}>
        $<$<LINK_LANGUAGE:Swift>:-sanitize=${GEO_SANITIZE}>
    )
endif()

# Warnings apply to first-party code only, so vendored sources stay unmodified.
add_library(geo_warnings INTERFACE)
target_compile_options(geo_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wpedantic>
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wshadow>
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wconversion>
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wno-sign-conversion>
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Werror>
)
