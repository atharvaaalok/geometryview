// Reads binary and ASCII STL.
#pragma once

#include "base/types.h"
#include "mesh/triangle_soup.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace geo::import {

/// Parses an STL file held in memory. Returns false and sets `error` if it is not a usable STL.
/// Recoverable problems, such as dropped non-finite triangles, become warnings on `soup`.
bool readStl(std::span<const u8> file, TriangleSoup& soup, std::string& error);

/// Reads a binary STL file in pieces as they arrive, so that the file is never held whole: the
/// browser reads a large file this way. Only a file whose size matches its triangle count exactly
/// is read so; any other, ASCII included, goes whole to readStl. The soup is the one readStl makes.
class StlPieces {
public:
    /// Takes the file's size and its first bytes, at least 84 of them if it has that many. False
    /// if it is not an exact binary STL, which must be read whole.
    bool begin(std::span<const u8> head, u64 fileSize);
    /// The file's next bytes, from its start, in order.
    void append(std::span<const u8> bytes);
    /// Hands over the soup once every byte has arrived. False, with `error`, as readStl would be.
    bool finish(TriangleSoup& soup, std::string& error);

private:
    u64 fileSize_ = 0;
    u64 received_ = 0;
    std::vector<float> corners_;
    std::array<u8, 50> partial_{}; // a record split between pieces
    usize partialSize_ = 0;
    usize next_ = 0; // the next triangle to fill
};

} // namespace geo::import
