// What the import helper tells the viewer, as framed messages on its stdout: a header, then `size`
// bytes of payload. Shared by both sides, so they cannot disagree.
#pragma once

#include "base/types.h"

namespace geo::format {

enum class HelperMessage : u32 {
    Segment = 1,  // SegmentMessage: a segment is safely in the cache file
    Progress = 2, // ProgressMessage
    Warning = 3,  // UTF-8 text
    Done = 4,     // no payload: the cache file is complete
    Failed = 5,   // UTF-8 text a person can read
};

struct MessageHeader {
    u32 kind;
    u32 size;
};

struct SegmentMessage {
    u64 offset;
    u64 size;
};

struct ProgressMessage {
    u32 done;
    u32 total;
};

/// Longer payloads are a protocol error, which bounds what the viewer ever allocates for one.
inline constexpr u32 kMaxMessagePayload = 4096;

} // namespace geo::format
