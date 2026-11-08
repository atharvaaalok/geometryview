# The WebGPU renderer spike, natively on Dawn (GEO_TARGET mac) or in the browser (GEO_TARGET wasm).
# Built only with GEO_WEBGPU_SPIKE, so the Mac app's build never depends on Dawn.
add_library(spike_webgpu_core STATIC scene.cpp renderer.cpp)
target_include_directories(spike_webgpu_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(spike_webgpu_core PUBLIC geo_format PRIVATE geo_warnings)

if(GEO_TARGET STREQUAL "wasm")
    target_compile_options(spike_webgpu_core PUBLIC --use-port=emdawnwebgpu)
    add_executable(spike_webgpu web.cpp)
    target_link_libraries(spike_webgpu PRIVATE spike_webgpu_core)
    target_link_options(spike_webgpu PRIVATE
        --use-port=emdawnwebgpu
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB
        -sEXPORTED_FUNCTIONS=_malloc,_free,_spike_init,_spike_state,_spike_error,_spike_load,_spike_start,_spike_results
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString
        -sENVIRONMENT=web -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createSpike)
    set_target_properties(spike_webgpu PROPERTIES SUFFIX ".mjs")
else()
    find_package(Dawn CONFIG REQUIRED PATHS "${CMAKE_SOURCE_DIR}/ThirdParty/.dawn/install"
                 NO_DEFAULT_PATH)
    target_link_libraries(spike_webgpu_core PUBLIC dawn::webgpu_dawn)
    add_executable(spike_webgpu native.cpp)
    target_link_libraries(spike_webgpu PRIVATE spike_webgpu_core)
    set_target_properties(spike_webgpu PROPERTIES
        BUILD_RPATH "${CMAKE_SOURCE_DIR}/ThirdParty/.dawn/install/lib")
endif()
