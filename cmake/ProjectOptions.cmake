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

if(GEO_TARGET STREQUAL "wasm")
    # SIMD128 everywhere; relaxed SIMD and 64-bit memory are left out because Safari lacks them.
    target_compile_options(geo_options INTERFACE -msimd128)
    option(GEO_WASM_THREADS "Build WebAssembly with threads (needs cross-origin isolation)" OFF)
    if(GEO_WASM_THREADS)
        target_compile_options(geo_options INTERFACE -pthread)
        # The default allocator takes one global lock, which serialises the parallel stages.
        target_link_options(geo_options INTERFACE -pthread -sMALLOC=mimalloc)
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
