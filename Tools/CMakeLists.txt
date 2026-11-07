add_executable(gcli gcli/main.cpp)
target_include_directories(gcli PRIVATE "${CMAKE_SOURCE_DIR}/Engine")
target_link_libraries(gcli PRIVATE engine import_core geo_warnings)

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
