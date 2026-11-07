file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS unit/*.cpp unit/*.mm)
add_executable(geo_tests ${test_sources})
target_include_directories(geo_tests PRIVATE "${CMAKE_SOURCE_DIR}/Engine")
add_dependencies(geo_tests geometry-import engine_shaders)
target_compile_definitions(geo_tests PRIVATE
    GEO_TEST_HELPER_PATH="$<TARGET_FILE:geometry-import>"
    GEO_TEST_SHADER_LIBRARY="${GEO_METALLIB}")
target_link_libraries(geo_tests PRIVATE engine import_core doctest geo_warnings)
set_target_properties(geo_tests PROPERTIES GEO_ALLOW_EXCEPTIONS ON)
if(TARGET geometry-occt-worker)
    add_dependencies(geo_tests geometry-occt-worker)
    target_compile_definitions(geo_tests PRIVATE
        GEO_TEST_WORKER_PATH="$<TARGET_FILE:geometry-occt-worker>")
endif()
add_test(NAME unit COMMAND geo_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME layering COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/Tools/check_layers.py")
