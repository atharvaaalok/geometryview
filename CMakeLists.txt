cmake_minimum_required(VERSION 3.29)

# "mac" builds the native app with its Metal engine. "wasm" builds the portable engine and import
# code as WebAssembly, with their unit tests, for the universal viewer.
set(GEO_TARGET "mac" CACHE STRING "What to build: mac or wasm")
set_property(CACHE GEO_TARGET PROPERTY STRINGS mac wasm)

if(GEO_TARGET STREQUAL "wasm")
    project(GeometryView VERSION 0.4.1 LANGUAGES C CXX)
    # No C++ modules are used, and the module scanner cannot resolve Emscripten's SIMD headers.
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
else()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple silicon only")
    project(GeometryView VERSION 0.4.1 LANGUAGES C CXX OBJCXX Swift)
    set(CMAKE_OBJCXX_STANDARD 23)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ProjectOptions)
if(NOT GEO_TARGET STREQUAL "wasm")
    include(MetalLibrary)
endif()

enable_testing()

add_subdirectory(ThirdParty)
add_subdirectory(Engine)
add_subdirectory(Import)
add_subdirectory(Tools)
if(NOT GEO_TARGET STREQUAL "wasm")
    add_subdirectory(App)
endif()
add_subdirectory(Tests)
