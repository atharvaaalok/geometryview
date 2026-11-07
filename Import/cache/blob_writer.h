// Writes cache files as a stream of segments, so geometry can be shown while it is still arriving.
#pragma once

#include "base/types.h"
#include "format/blob.h"
#include "mesh/packer.h"

#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace geo::import {

/// The model of a file: what it contains and how it is arranged.
struct ModelData {
    std::vector<format::DefinitionInfo> definitions;
    std::vector<format::OccurrenceRecord> occurrences;
    std::vector<format::NodeRecord> nodes;
    std::string strings;
};

/// Told about each segment once it is safely in the file.
class SegmentListener {
public:
    virtual ~SegmentListener() = default;
    virtual void segmentWritten(u64 offset, u64 size) = 0;
    /// Reports how much of the work is done, in units only the ratio of which matters.
    virtual void progress(u32 done, u32 total) {
        (void)done;
        (void)total;
    }
};

/// Appends segments to an open file. Thread-safe. Callers write to a temporary file and rename it
/// after `finish`, so a reader of the final name never sees a partial file.
class BlobStream {
public:
    BlobStream(int fd, SegmentListener* listener);

    /// Writes the model at once, so it can be shown ahead of any geometry.
    bool writeModel(const ModelData& model);

    /// Buffers one definition's geometry, writing a segment when the policy below says so.
    /// `faces` has one entry per packed face, parallel to `definition.originalFace`.
    bool addDefinition(u32 index, u32 level, PackedDefinition&& definition,
                       std::vector<format::FaceRecord>&& faces);

    /// Writes buffered geometry and then the header. The hash can be set this late so that hashing
    /// a large file never delays showing it.
    bool finish(const std::array<u8, 16>& sourceHash);

    const std::string& error() const { return error_; }
    u64 bytesWritten() const { return offset_; }

private:
    struct Pending {
        u32 index;
        u32 level;
        PackedDefinition definition;
        std::vector<format::FaceRecord> faces;
    };

    bool shouldFlush() const;
    bool flush();
    bool writeAll(const void* data, u64 size, u64 offset);
    bool recordSegment(u64 offset, u64 size);

    std::mutex mutex_;
    int fd_;
    SegmentListener* listener_;
    format::Header header_{};
    u64 offset_ = format::kAlignment;
    std::string error_;

    std::vector<Pending> pending_;
    u64 pendingBytes_ = 0;
    double started_;
    double lastFlush_;
};

} // namespace geo::import
