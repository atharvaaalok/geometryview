file(GLOB_RECURSE engine_sources CONFIGURE_DEPENDS
    base/*.cpp math/*.cpp scene/*.cpp render/*.cpp gpu/*.mm gpu/*.cpp engine/*.cpp engine/*.mm)
add_library(engine STATIC ${engine_sources})
target_include_directories(engine PUBLIC include PRIVATE .)
target_link_libraries(engine
    PUBLIC geo_options
    PRIVATE geo_warnings
)
target_link_libraries(engine INTERFACE
    "-framework Foundation"
    "-framework Metal"
    "-framework QuartzCore"
)

file(GLOB shader_sources CONFIGURE_DEPENDS shaders/*.metal)
file(GLOB shader_headers CONFIGURE_DEPENDS shaders/shared/*.h shaders/*.h)
set(GEO_METALLIB "${CMAKE_BINARY_DIR}/shaders/default.metallib" CACHE INTERNAL "")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shaders")
geo_add_metallib(engine_shaders
    OUTPUT "${GEO_METALLIB}"
    SOURCES ${shader_sources}
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    DEPENDS ${shader_headers}
)
add_dependencies(engine engine_shaders)
