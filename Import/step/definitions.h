// Turns bodies into definitions, the unit of geometry: bodies with byte-identical closures become
// one definition, whichever products they came from. Definitions are then cut into slices, each a
// small self-contained STEP file that OCCT can read on its own.
#pragma once

#include "base/types.h"
#include "step/part21.h"
#include "step/structure.h"

#include <array>
#include <string>
#include <vector>

namespace geo::import::step {

struct Definition {
    u32 body = 0;            // the body it was first seen as
    u32 occurrenceCount = 0; // how many times the assembly places it
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
    u32 firstFace = 0; // into Definitions::faces
    u32 faceCount = 0;
    u32 firstChunk = 0; // into Definitions::chunks
    u32 chunkCount = 0;
};

/// Faces of one definition that are read together.
struct Chunk {
    u32 definition = 0;
    u32 firstFace = 0; // into Definitions::faces
    u32 faceCount = 0;
};

struct Definitions {
    std::vector<Definition> items;
    std::vector<u32> definitionOfBody;
    std::vector<FaceUse> faces; // by definition, in a stable spatial order
    std::vector<Chunk> chunks;
};

/// Bytes of STEP text per chunk. Larger chunks spend more time in OCCT's superlinear passes, and
/// smaller ones pay its per-file overhead more often.
inline constexpr usize kChunkTargetBytes = 100 * 1024;

Definitions buildDefinitions(const Part21& index, const Structure& structure,
                             usize chunkBytes = kChunkTargetBytes);

/// Visits the records a set of roots reach, each once. Stamps persist between walks, so starting a
/// new walk costs nothing.
class ClosureWalker {
public:
    explicit ClosureWalker(const Part21& index);
    ~ClosureWalker();
    ClosureWalker(const ClosureWalker&) = delete;
    ClosureWalker& operator=(const ClosureWalker&) = delete;

    void begin();
    /// Adds `root` and what it reaches. Records are appended to `visited()`.
    void add(u32 root);
    const std::vector<u32>& visited() const { return visited_; }
    bool contains(u32 record) const { return stamps_[record] == generation_; }

private:
    const Part21& index_;
    u32* stamps_;
    u32 generation_ = 0;
    std::vector<u32> stack_;
    std::vector<u32> visited_;
};

/// Writes `faces` as a complete STEP file: the original header, the faces' closures, and a surface
/// model holding them, in `context`'s units. Reading it transfers exactly these faces.
void writeSlice(const Part21& index, std::span<const u32> faceRecords, u32 context,
                ClosureWalker& walker, std::string& text);

} // namespace geo::import::step
