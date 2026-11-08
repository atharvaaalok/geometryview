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
    # The OCCT worker as WebAssembly, when ThirdParty/build_occt_wasm.sh has run. For now it runs
    # under Node, to replay slices saved from the native worker.
    set(GEO_OCCT_WASM_ROOT "${CMAKE_SOURCE_DIR}/ThirdParty/.occt-wasm/install")
    if(EXISTS "${GEO_OCCT_WASM_ROOT}/lib/libTKDESTEP.a")
        add_executable(geometry-occt-worker occt/worker_main.cpp occt/mesh_shape.cpp)
        set_target_properties(geometry-occt-worker PROPERTIES GEO_ALLOW_EXCEPTIONS ON)
        target_include_directories(geometry-occt-worker PRIVATE .)
        target_include_directories(geometry-occt-worker SYSTEM PRIVATE
            "${GEO_OCCT_WASM_ROOT}/include/opencascade")
        # WebAssembly's own exceptions, as OCCT was built with; JavaScript's are far slower.
        target_compile_options(geometry-occt-worker PRIVATE -fwasm-exceptions)
        file(GLOB occt_wasm_libraries CONFIGURE_DEPENDS "${GEO_OCCT_WASM_ROOT}/lib/*.a")
        target_link_libraries(geometry-occt-worker PRIVATE
            geo_base geo_warnings
            -Wl,--start-group ${occt_wasm_libraries} -Wl,--end-group)
        target_link_options(geometry-occt-worker PRIVATE
            -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1
            -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sEXIT_RUNTIME=1)

        # The same meshing as a module for a Web Worker, called directly by the page's script.
        add_executable(geometry-occt-web occt/web_worker.cpp occt/mesh_shape.cpp)
        set_target_properties(geometry-occt-web PROPERTIES GEO_ALLOW_EXCEPTIONS ON SUFFIX ".mjs")
        target_include_directories(geometry-occt-web PRIVATE .)
        target_include_directories(geometry-occt-web SYSTEM PRIVATE
            "${GEO_OCCT_WASM_ROOT}/include/opencascade")
        target_compile_options(geometry-occt-web PRIVATE -fwasm-exceptions)
        target_link_libraries(geometry-occt-web PRIVATE
            geo_base geo_warnings
            -Wl,--start-group ${occt_wasm_libraries} -Wl,--end-group)
        target_link_options(geometry-occt-web PRIVATE
            -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sALLOW_MEMORY_GROWTH=1
            # Each worker's own cap, so one that runs out fails by itself rather than taking
            # the page past what iOS allows.
            -sMAXIMUM_MEMORY=512MB -sABORTING_MALLOC=0
            -sSTACK_SIZE=8MB -sENVIRONMENT=worker -sMODULARIZE=1
            -sEXPORT_ES6=1 -sEXPORT_NAME=createOcct
            -sEXPORTED_FUNCTIONS=_occt_init,_occt_mesh,_occt_stats,_occt_payload,_occt_trap,_occt_exhaust,_malloc,_free
            -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF64)
    endif()

    # The STEP importer for the browser: the portable StepImport, driven by the page, whose Web
    # Workers run the OCCT jobs.
    add_executable(geometry-step-web web/step_supervisor.cpp)
    target_include_directories(geometry-step-web PRIVATE .)
    target_link_libraries(geometry-step-web PRIVATE import_core geo_warnings)
    set_target_properties(geometry-step-web PROPERTIES SUFFIX ".mjs")
    target_link_options(geometry-step-web PRIVATE
        -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=4MB
        -sENVIRONMENT=web,worker -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createStepImporter
        -sEXPORTED_FUNCTIONS=_step_open,_step_begin,_step_next,_step_text,_step_text_size,_step_request,_step_payload,_step_reply,_step_crash,_step_finish,_step_blob,_step_blob_size,_step_error,_step_stats
        -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPF64,UTF8ToString)
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
