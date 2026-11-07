file(GLOB_RECURSE import_sources CONFIGURE_DEPENDS import.cpp stl/*.cpp mesh/*.cpp cache/*.cpp)
add_library(import_core STATIC ${import_sources})
target_include_directories(import_core PUBLIC .)
target_include_directories(import_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/xxhash")
target_link_libraries(import_core
    PUBLIC geo_base geo_format
    PRIVATE geo_warnings meshoptimizer
)

add_executable(geometry-import helper/main.cpp)
target_link_libraries(geometry-import PRIVATE import_core geo_warnings)
