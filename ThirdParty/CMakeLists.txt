file(GLOB meshopt_sources CONFIGURE_DEPENDS meshoptimizer/src/*.cpp)
add_library(meshoptimizer STATIC ${meshopt_sources})
target_include_directories(meshoptimizer SYSTEM PUBLIC meshoptimizer/src)
target_link_libraries(meshoptimizer PRIVATE geo_options)
target_compile_options(meshoptimizer PRIVATE -w)

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE doctest)
