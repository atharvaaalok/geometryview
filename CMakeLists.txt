cmake_minimum_required(VERSION 3.29)

set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Apple silicon only")

project(Geometry VERSION 0.1.0 LANGUAGES C CXX OBJCXX Swift)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(MetalLibrary)

enable_testing()

add_subdirectory(ThirdParty)
add_subdirectory(Engine)
add_subdirectory(Import)
add_subdirectory(Tools)
add_subdirectory(App)
add_subdirectory(Tests)
