file(GLOB_RECURSE import_sources CONFIGURE_DEPENDS import.cpp stl/*.cpp mesh/*.cpp cache/*.cpp)
add_library(import_core STATIC ${import_sources})
target_include_directories(import_core PUBLIC . "${CMAKE_SOURCE_DIR}/Engine")
target_include_directories(import_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/xxhash")
target_link_libraries(import_core
    PUBLIC geo_options
    PRIVATE geo_warnings meshoptimizer
)
