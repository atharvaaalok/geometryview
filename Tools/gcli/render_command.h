// gcli render: one cache file drawn to a PNG, or many in one run.
#pragma once

namespace geo::gcli {

/// gcli render <metallib> <blob> <png> [positional options | --flags], or
/// gcli render <metallib> --batch [--same-scale] < shots. Returns the process's exit status.
int renderCommand(int argc, const char** argv);

} // namespace geo::gcli
