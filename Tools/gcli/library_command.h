// gcli library: the gallery's library run over a folder, headless.
#pragma once

namespace geo::gcli {

/// gcli library <metallib> <helper> <cache> <folder> [options]. Returns the process's exit status.
int libraryCommand(int argc, const char** argv);

} // namespace geo::gcli
