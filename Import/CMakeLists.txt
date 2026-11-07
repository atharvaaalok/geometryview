file(GLOB_RECURSE import_sources CONFIGURE_DEPENDS
    import.cpp stl/*.cpp mesh/*.cpp cache/*.cpp step/*.cpp)
if(GEO_TARGET STREQUAL "wasm")
    # The STEP pipeline's driver runs OCCT in child processes; Web Workers take that role later.
    list(FILTER import_sources EXCLUDE REGEX "step/step_import\\.cpp$")
endif()
add_library(import_core STATIC ${import_sources})
target_include_directories(import_core PUBLIC .)
target_include_directories(import_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty/xxhash")
target_link_libraries(import_core
    PUBLIC geo_base geo_format
    PRIVATE geo_warnings meshoptimizer
)

if(GEO_TARGET STREQUAL "wasm")
    return()
endif()

add_executable(geometry-import helper/main.cpp)
target_link_libraries(geometry-import PRIVATE import_core geo_warnings)

# The OCCT worker is optional, so everything else builds on a machine that has not run
# ThirdParty/build_occt.sh. It is the only target that sees OCCT.
set(GEO_OCCT_ROOT "${CMAKE_SOURCE_DIR}/ThirdParty/.occt/install")
if(EXISTS "${GEO_OCCT_ROOT}/lib/libTKDESTEP.a")
    add_executable(geometry-occt-worker occt/worker_main.cpp occt/mesh_shape.cpp)
    set_target_properties(geometry-occt-worker PROPERTIES GEO_ALLOW_EXCEPTIONS ON)
    target_include_directories(geometry-occt-worker PRIVATE .)
    target_include_directories(geometry-occt-worker SYSTEM PRIVATE
        "${GEO_OCCT_ROOT}/include/opencascade")
    file(GLOB occt_libraries CONFIGURE_DEPENDS "${GEO_OCCT_ROOT}/lib/*.a")
    target_link_libraries(geometry-occt-worker PRIVATE
        geo_base geo_warnings
        ${occt_libraries} ${occt_libraries} ${occt_libraries}
    )
else()
    message(WARNING "OCCT not found. Run ThirdParty/build_occt.sh to import STEP files.")
endif()
