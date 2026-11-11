// gcli sheet: many cache files drawn on one sheet as the gallery draws them.
#pragma once

namespace geo::gcli {

/// gcli sheet <metallib> <out.png> [options] < tiles.jsonl. Returns the process's exit status.
int sheetCommand(int argc, const char** argv);

} // namespace geo::gcli
