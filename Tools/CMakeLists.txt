add_executable(gcli gcli/main.cpp)
target_link_libraries(gcli PRIVATE engine geo_warnings)

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
