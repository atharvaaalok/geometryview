# Compile options shared by all first-party code.

set(GEO_SANITIZE "" CACHE STRING "Comma-separated sanitizers, for example address,undefined")

add_library(geo_options INTERFACE)

target_compile_options(geo_options INTERFACE
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-mcpu=apple-m1>
    $<$<AND:$<COMPILE_LANGUAGE:CXX,OBJCXX>,$<CONFIG:Release>>:-O3>
    $<$<AND:$<COMPILE_LANGUAGE:CXX,OBJCXX>,$<CONFIG:Release>>:-flto=thin>
)
target_link_options(geo_options INTERFACE
    $<$<CONFIG:Release>:-flto=thin>
    $<$<CONFIG:Release>:-Wl,-dead_strip>
)

if(GEO_SANITIZE)
    target_compile_options(geo_options INTERFACE
        $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fsanitize=${GEO_SANITIZE}>
        $<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fno-omit-frame-pointer>
    )
    target_link_options(geo_options INTERFACE -fsanitize=${GEO_SANITIZE})
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
