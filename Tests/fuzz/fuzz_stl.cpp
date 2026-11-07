// Fuzzes the STL reader: any bytes must parse or fail cleanly, as binary or ASCII.
#include "stl/stl_reader.h"

#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    geo::import::TriangleSoup soup;
    std::string error;
    geo::import::readStl({data, size}, soup, error);
    return 0;
}
