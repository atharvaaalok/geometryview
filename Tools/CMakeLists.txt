# probe runs the portable import stages on real files, natively and as WebAssembly, so the two
# builds can be compared bit for bit and timed against each other.
add_executable(probe probe/main.cpp)
target_include_directories(probe SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/xxhash")
target_link_libraries(probe PRIVATE import_core geo_warnings)
if(GEO_WEBGPU_SPIKE)
    add_subdirectory(spikes/webgpu)
endif()
if(GEO_TARGET STREQUAL "wasm")
    # The real file system, through Node, and room for big files.
    target_link_options(probe PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB
                        -sEXIT_RUNTIME=1 -sSTACK_SIZE=4MB)
    if(GEO_WASM_THREADS)
        target_link_options(probe PRIVATE -sPTHREAD_POOL_SIZE=16)
    endif()
    return()
endif()

add_executable(gcli gcli/main.cpp)
target_include_directories(gcli PRIVATE "${CMAKE_SOURCE_DIR}/Engine")
target_link_libraries(gcli PRIVATE engine import_core geo_warnings)

# The WebGPU renderer on Dawn, compared with Metal and timed.
if(GEO_WEBGPU)
    add_executable(geometry-webgpu webgpu/main.mm)
    target_include_directories(geometry-webgpu PRIVATE "${CMAKE_SOURCE_DIR}/Engine")
    target_link_libraries(geometry-webgpu PRIVATE engine engine_webgpu geo_warnings)
    set_target_properties(geometry-webgpu PROPERTIES
        BUILD_RPATH "${CMAKE_SOURCE_DIR}/ThirdParty/.dawn/install/lib")
endif()

# Spikes are small reproducible experiments. Their results are recorded in Docs/adr.
add_executable(spike_geometry spikes/geometry_path.mm)
target_include_directories(spike_geometry PRIVATE spikes)
target_link_libraries(spike_geometry PRIVATE
    meshoptimizer geo_options "-framework Foundation" "-framework Metal" "-framework QuartzCore")
set(spike_metallib "${CMAKE_BINARY_DIR}/shaders/spike_geometry.metallib")
geo_add_metallib(spike_geometry_shaders
    OUTPUT "${spike_metallib}"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/spikes/geometry_path.metal"
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/spikes"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/spikes/geometry_path_shared.h"
)
add_dependencies(spike_geometry spike_geometry_shaders)

add_executable(spike_residency spikes/residency.mm)
target_link_libraries(spike_residency PRIVATE
    geo_options "-framework Foundation" "-framework Metal")
