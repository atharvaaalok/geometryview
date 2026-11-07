// gcli: the headless command-line front end used for rendering, benchmarks and tests.
#include "engine.h"

#include <print>

int main() {
    std::println("gcli {}", geo_engine_version());
    return 0;
}
