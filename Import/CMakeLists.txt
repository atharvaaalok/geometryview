file(GLOB_RECURSE import_sources CONFIGURE_DEPENDS stl/*.cpp mesh/*.cpp format/*.cpp)
add_library(import_core STATIC ${import_sources})
target_include_directories(import_core PUBLIC . "${CMAKE_SOURCE_DIR}/Engine")
target_link_libraries(import_core
    PUBLIC geo_options
    PRIVATE geo_warnings meshoptimizer
)
