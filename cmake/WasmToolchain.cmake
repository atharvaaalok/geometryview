# Finds Emscripten, wherever it is installed, and hands over to its own toolchain file.
find_program(GEO_EM_CONFIG em-config REQUIRED)
execute_process(COMMAND "${GEO_EM_CONFIG}" EMSCRIPTEN_ROOT
                OUTPUT_VARIABLE GEO_EMSCRIPTEN_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
include("${GEO_EMSCRIPTEN_ROOT}/cmake/Modules/Platform/Emscripten.cmake")
