// What the import supervisor and an OCCT worker say to each other over a pair of pipes. This header
// includes nothing from OCCT, so the supervisor stays free of it.
#pragma once

#include "base/types.h"

namespace geo::import::worker {

// Fixed descriptors, so OCCT writing to stdout can never corrupt the protocol.
inline constexpr int kRequestFd = 3;
inline constexpr int kReplyFd = 4;
inline constexpr int kSourceFd = 5;

inline constexpr u32 kMagic = 0x4F435754; // "OCWT"

enum class RequestKind : u32 {
    Slice = 1,     // `textBytes` of STEP text follow: mesh its faces
    WholeFile = 2, // read the source descriptor with OCCT's full reader
};

struct Request {
    u32 magic;
    u32 kind;
    u64 textBytes;
    double linearDeflection; // millimetres. Zero derives tolerances from the shape's size.
    double angularDeflection;
    double minSize;
};

enum class ReplyStatus : u32 {
    Ok = 0,
    Failed = 1,      // OCCT raised an exception or read nothing
    OutOfMemory = 2, // an allocation failed: the slice may mesh coarser, or in a fresh worker
};

struct Reply {
    u32 magic;
    u32 status;
    u32 faceCount;
    u32 unmappedFaces; // faces with no matching STEP entity
    u32 unmeshedFaces; // faces OCCT could not triangulate
    u32 groupCount;    // bodies, for WholeFile
    u64 payloadBytes;
    double readSeconds;
    double transferSeconds;
    double meshSeconds;
};

// The payload is `faceCount` of these, each followed by `vertexCount` positions and normals as
// three floats each, then `triangleCount` triples of u32.
struct FaceHeader {
    u32 sourceId; // the STEP entity id, or 0 if unknown
    u32 group;    // the body it belongs to, for WholeFile
    u32 vertexCount;
    u32 triangleCount;
};

} // namespace geo::import::worker
