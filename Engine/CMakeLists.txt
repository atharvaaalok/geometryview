# Layers below the engine are separate libraries, so the import helper links no GPU code at all.
file(GLOB base_sources CONFIGURE_DEPENDS base/*.cpp)
add_library(geo_base STATIC ${base_sources})
target_include_directories(geo_base PUBLIC .)
target_link_libraries(geo_base PUBLIC geo_options PRIVATE geo_warnings)

add_library(geo_format STATIC "${CMAKE_SOURCE_DIR}/Import/format/blob.cpp")
target_include_directories(geo_format PUBLIC "${CMAKE_SOURCE_DIR}/Import")
target_link_libraries(geo_format PUBLIC geo_base PRIVATE geo_warnings)

if(GEO_TARGET STREQUAL "wasm")
    # The portable layers only: no Metal, no engine thread, no import processes.
    file(GLOB_RECURSE engine_core_sources CONFIGURE_DEPENDS
        math/*.cpp nav/*.cpp scene/*.cpp model/*.cpp render/*.cpp)
    add_library(engine_core STATIC ${engine_core_sources})
    target_include_directories(engine_core PUBLIC include PUBLIC .)
    target_include_directories(engine_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/xxhash")
    target_link_libraries(engine_core PUBLIC geo_base geo_format PRIVATE geo_warnings)
    return()
endif()

file(GLOB_RECURSE engine_sources CONFIGURE_DEPENDS
    math/*.cpp nav/*.cpp scene/*.cpp model/*.cpp render/*.cpp ipc/*.cpp gpu/*.mm gpu/*.cpp
    engine/*.cpp engine/*.mm)
add_library(engine STATIC ${engine_sources})
target_include_directories(engine PUBLIC include PRIVATE .)
target_include_directories(engine SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/xxhash")
target_link_libraries(engine
    PUBLIC geo_base geo_format
    PRIVATE geo_warnings
)
target_link_libraries(engine INTERFACE
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
    "-framework ImageIO"
    "-framework CoreGraphics"
    "-framework UniformTypeIdentifiers"
    "-framework CoreText"
    "-framework AppKit"
)

file(GLOB shader_sources CONFIGURE_DEPENDS shaders/*.metal)
file(GLOB shader_headers CONFIGURE_DEPENDS shaders/shared/*.h shaders/*.h)
set(GEO_METALLIB "${CMAKE_BINARY_DIR}/shaders/default.metallib" CACHE INTERNAL "")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shaders")
geo_add_metallib(engine_shaders
    OUTPUT "${GEO_METALLIB}"
    SOURCES ${shader_sources}
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders" "${CMAKE_SOURCE_DIR}/Import"
    DEPENDS ${shader_headers} "${CMAKE_SOURCE_DIR}/Import/format/gpu_records.h"
)
add_dependencies(engine engine_shaders)
