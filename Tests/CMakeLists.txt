file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS unit/*.cpp)
add_executable(geo_tests ${test_sources})
target_include_directories(geo_tests PRIVATE "${CMAKE_SOURCE_DIR}/Engine")
target_link_libraries(geo_tests PRIVATE engine doctest geo_warnings)
add_test(NAME unit COMMAND geo_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME layering COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/Tools/check_layers.py")
