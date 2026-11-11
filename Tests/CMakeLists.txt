if(GEO_TARGET STREQUAL "wasm")
    # The portable unit tests, run by Node. Tests of Metal, the import processes and the engine
    # thread stay native.
    set(portable_tests
        blob camera core edges face_state facts flight indexed_mesh interaction json name_index overlay packer
        parallel parallel_sort part21 pointer_controller refinement segment shell slice_ladder spsc_ring
        step_structure stl_reader style)
    list(TRANSFORM portable_tests PREPEND unit/)
    list(TRANSFORM portable_tests APPEND _test.cpp)
    add_executable(geo_tests unit/main.cpp ${portable_tests})
    target_include_directories(geo_tests PRIVATE "${CMAKE_SOURCE_DIR}/Engine")
    target_link_libraries(geo_tests PRIVATE engine_core import_core doctest geo_warnings)
    set_target_properties(geo_tests PROPERTIES GEO_ALLOW_EXCEPTIONS ON)
    target_compile_options(geo_tests PRIVATE -fwasm-exceptions)
    target_compile_definitions(geo_tests PRIVATE GEO_SANITIZED=0)
    target_link_options(geo_tests PRIVATE -fwasm-exceptions -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1
                        -sMAXIMUM_MEMORY=4GB -sEXIT_RUNTIME=1 -sSTACK_SIZE=4MB)
    if(GEO_WASM_THREADS)
        target_link_options(geo_tests PRIVATE -sPTHREAD_POOL_SIZE=16)
    endif()
    add_test(NAME unit COMMAND geo_tests)
    return()
endif()

file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS unit/*.cpp unit/*.mm)
add_executable(geo_tests ${test_sources})
target_include_directories(geo_tests PRIVATE "${CMAKE_SOURCE_DIR}/Engine")
add_dependencies(geo_tests geometryview-import engine_shaders)
# Computed here rather than with $<BOOL:>, which reads the comma in address,undefined as a second
# argument.
if(GEO_SANITIZE)
    set(geo_sanitized 1)
else()
    set(geo_sanitized 0)
endif()
target_compile_definitions(geo_tests PRIVATE
    GEO_SANITIZED=${geo_sanitized}
    GEO_TEST_HELPER_PATH="$<TARGET_FILE:geometryview-import>"
    GEO_TEST_SHADER_LIBRARY="${GEO_METALLIB}"
    GEO_TEST_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/Tests/golden"
    GEO_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/Tests/data")
target_link_libraries(geo_tests PRIVATE engine import_core doctest geo_warnings)
set_target_properties(geo_tests PROPERTIES GEO_ALLOW_EXCEPTIONS ON)
if(TARGET geometryview-occt-worker)
    add_dependencies(geo_tests geometryview-occt-worker)
    target_compile_definitions(geo_tests PRIVATE
        GEO_TEST_WORKER_PATH="$<TARGET_FILE:geometryview-occt-worker>")
endif()
if(GEO_WEBGPU)
    target_link_libraries(geo_tests PRIVATE engine_webgpu)
    target_compile_definitions(geo_tests PRIVATE GEO_HAS_WEBGPU=1)
    set_target_properties(geo_tests PROPERTIES
        BUILD_RPATH "${CMAKE_SOURCE_DIR}/ThirdParty/.dawn/install/lib")
endif()
add_test(NAME unit COMMAND geo_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME layering COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/Tools/check_layers.py")
