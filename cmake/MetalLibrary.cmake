# geo_add_metallib(<target> OUTPUT <path> SOURCES <files...> INCLUDES <dirs...> DEPENDS <files...>)
#
# Compiles Metal Shading Language sources into a single .metallib. Sources compile together so a
# change to any shared header rebuilds the whole library.
function(geo_add_metallib target)
    cmake_parse_arguments(ARG "" "OUTPUT" "SOURCES;INCLUDES;DEPENDS" ${ARGN})

    set(flags -std=metal4.0 -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET} -Wall -Werror)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND flags -gline-tables-only -frecord-sources)
    else()
        list(APPEND flags -O3)
    endif()
    foreach(dir IN LISTS ARG_INCLUDES)
        list(APPEND flags -I "${dir}")
    endforeach()

    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND xcrun -sdk macosx metal ${flags} -o "${ARG_OUTPUT}" ${ARG_SOURCES}
        DEPENDS ${ARG_SOURCES} ${ARG_DEPENDS}
        COMMENT "Compiling Metal library ${target}"
        VERBATIM
    )
    add_custom_target(${target} DEPENDS "${ARG_OUTPUT}")
endfunction()
