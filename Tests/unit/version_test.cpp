#include "engine.h"

#include <doctest/doctest.h>
#include <string_view>

TEST_CASE("the engine reports a version") {
    CHECK(std::string_view{geo_engine_version()} == "0.3.0");
}
