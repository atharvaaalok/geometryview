// What a STEP file contains: its assembly tree, its bodies and where they are placed. This replaces
// OCCT's XCAF layer and comes straight from the record index, so it costs milliseconds.
#pragma once

#include "base/types.h"
#include "step/part21.h"

#include <simd/simd.h>
#include <string>
#include <vector>

namespace geo::import::step {

inline constexpr u32 kNoColor = 0;

/// A linear map and a translation, in millimetres.
struct Affine {
    simd_double3x3 linear = matrix_identity_double3x3;
    simd_double3 translation = {0.0, 0.0, 0.0};
};

Affine compose(const Affine& outer, const Affine& inner);
Affine inverse(const Affine& transform);

enum class NodeKind : u8 { Assembly, Part, Body };

/// One row of the assembly tree. Nodes are in depth-first order, so a node's subtree is the range
/// [this node, subtreeEnd), and the occurrences beneath it are a range too.
struct TreeNode {
    u32 parent = kNoRecord;
    u32 subtreeEnd = 0;
    u32 firstOccurrence = 0;
    u32 occurrenceEnd = 0;
    u32 nameOffset = 0;
    u32 nameLength = 0;
    NodeKind kind = NodeKind::Part;
};

/// A face of a body. `flipped` says the owning shell or face use reverses it.
struct FaceUse {
    u32 record = kNoRecord;
    u32 color = kNoColor;
    u16 shell = 0; // which of the body's shells; shell 0 is the outer one
    bool flipped = false;
};

/// A distinct solid or surface model.
struct Body {
    u32 record = kNoRecord;
    u32 context = kNoRecord; // the representation context, which carries the file's units
    u32 firstFace = 0;
    u32 faceCount = 0;
    u32 color = kNoColor;
    u16 shellCount = 0;
    bool closed = false;
    bool hasVoids = false; // shells after the first are cavities
};

/// A body placed in the assembly.
struct BodyOccurrence {
    u32 body = 0;
    u32 node = 0;
    Affine world;
};

struct Structure {
    std::vector<TreeNode> nodes;
    std::vector<Body> bodies;
    std::vector<FaceUse> faces; // grouped by body
    std::vector<BodyOccurrence> occurrences;
    std::string names; // UTF-8, indexed by nodes
    std::vector<std::string> warnings;
    /// The file has geometry this reader does not slice, so it needs a full OCCT read.
    bool needsWholeFile = false;
};

/// The length of one file unit in millimetres, for a representation context.
double lengthFactor(const Part21& index, u32 context);

Structure readStructure(const Part21& index);

} // namespace geo::import::step
