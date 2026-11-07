#include "cache/blob_writer.h"

#include "base/time.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <unistd.h>

namespace geo::import {

using namespace format;

namespace {

// The gap between segments grows with the time spent so far, which shows early results quickly and
// keeps the segment count small: every segment is a Metal buffer and a table rebuild in the viewer.
constexpr double kMinFlushInterval = 0.05;
constexpr double kFlushIntervalFraction = 0.25;
// Reserved so that the last segment can always be written, however long an import runs.
constexpr u32 kForcedFlushMargin = 8;

u64 alignUp(u64 value, u64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Lays out one segment in memory and writes it in a single call.
class SegmentBuilder {
public:
    template <typename T>
    void reserve(SectionKind kind, usize count) {
        if (count == 0) {
            return;
        }
        SectionEntry& entry = header_.sections[header_.sectionCount++];
        size_ = alignUp(size_, kSectionAlignment);
        entry = {u32(kind), u32(sizeof(T)), size_, u64(count) * sizeof(T)};
        size_ += entry.size;
    }

    void allocate() {
        size_ = alignUp(size_, kAlignment);
        header_.magic = kSegmentMagic;
        header_.version = kVersion;
        header_.size = size_;
        bytes_.assign(size_, 0);
        std::memcpy(bytes_.data(), &header_, sizeof(header_));
    }

    template <typename T>
    T* at(SectionKind kind) {
        for (u32 i = 0; i < header_.sectionCount; ++i) {
            if (header_.sections[i].kind == u32(kind)) {
                return reinterpret_cast<T*>(bytes_.data() + header_.sections[i].offset);
            }
        }
        return nullptr;
    }

    template <typename T>
    void copy(SectionKind kind, std::span<const T> items) {
        if (!items.empty()) {
            std::copy(items.begin(), items.end(), at<T>(kind));
        }
    }

    const std::vector<u8>& bytes() const { return bytes_; }

private:
    SegmentHeader header_{};
    u64 size_ = sizeof(SegmentHeader);
    std::vector<u8> bytes_;
};

} // namespace

BlobStream::BlobStream(int fd, SegmentListener* listener)
    : fd_(fd), listener_(listener), started_(nowSeconds()), lastFlush_(started_) {}

bool BlobStream::writeAll(const void* data, u64 size, u64 offset) {
    const auto* cursor = static_cast<const u8*>(data);
    while (size != 0) {
        const ssize_t written = pwrite(fd_, cursor, size, off_t(offset));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            error_ = std::format("Could not write the cache file: {}.", std::strerror(errno));
            return false;
        }
        cursor += written;
        offset += u64(written);
        size -= u64(written);
    }
    return true;
}

bool BlobStream::recordSegment(u64 offset, u64 size) {
    if (header_.segmentCount == kMaxSegments) {
        error_ = "The file has too many cache segments.";
        return false;
    }
    header_.segments[header_.segmentCount++] = {offset, size};
    if (listener_ != nullptr) {
        listener_->segmentWritten(offset, size);
    }
    return true;
}

bool BlobStream::writeModel(const ModelData& model) {
    std::lock_guard lock(mutex_);
    SegmentBuilder segment;
    segment.reserve<ModelInfo>(SectionKind::ModelInfo, 1);
    segment.reserve<DefinitionInfo>(SectionKind::DefinitionInfos, model.definitions.size());
    segment.reserve<OccurrenceRecord>(SectionKind::Occurrences, model.occurrences.size());
    segment.reserve<NodeRecord>(SectionKind::Nodes, model.nodes.size());
    segment.reserve<char>(SectionKind::Strings, model.strings.size());
    segment.allocate();
    segment.copy<ModelInfo>(SectionKind::ModelInfo, {&model.info, 1});
    segment.copy<DefinitionInfo>(SectionKind::DefinitionInfos, model.definitions);
    segment.copy<OccurrenceRecord>(SectionKind::Occurrences, model.occurrences);
    segment.copy<NodeRecord>(SectionKind::Nodes, model.nodes);
    segment.copy<char>(SectionKind::Strings, model.strings);

    const std::vector<u8>& bytes = segment.bytes();
    if (!writeAll(bytes.data(), bytes.size(), offset_)) {
        return false;
    }
    const u64 start = offset_;
    offset_ += bytes.size();
    return recordSegment(start, bytes.size());
}

bool BlobStream::shouldFlush() const {
    if (kMaxSegments - header_.segmentCount <= kForcedFlushMargin) {
        return false;
    }
    const double now = nowSeconds();
    const double interval = std::max(kMinFlushInterval, (now - started_) * kFlushIntervalFraction);
    return now - lastFlush_ >= interval;
}

bool BlobStream::addDefinition(u32 index, u32 level, PackedDefinition&& definition,
                               std::vector<FaceRecord>&& faces) {
    std::lock_guard lock(mutex_);
    pendingBytes_ += definition.vertices.size() * sizeof(PackedVertex);
    pending_.push_back({index, level, std::move(definition), std::move(faces)});
    return shouldFlush() ? flush() : true;
}

bool BlobStream::flush() {
    if (pending_.empty()) {
        return true;
    }
    usize meshlets = 0, vertices = 0, highBits = 0, triangles = 0, faces = 0, segments = 0,
          chunks = 0;
    for (const Pending& item : pending_) {
        segments += item.definition.edgeSegments.size();
        chunks += item.definition.edgeChunks.size();
        meshlets += item.definition.meshlets.size();
        vertices += item.definition.vertices.size();
        highBits += item.definition.highBits.size();
        triangles += item.definition.triangles.size();
        faces += item.faces.size();
    }

    SegmentBuilder segment;
    segment.reserve<DefinitionRecord>(SectionKind::Definitions, pending_.size());
    segment.reserve<Meshlet>(SectionKind::Meshlets, meshlets);
    segment.reserve<PackedVertex>(SectionKind::Vertices, vertices);
    segment.reserve<u32>(SectionKind::HighBits, highBits);
    segment.reserve<u32>(SectionKind::Triangles, triangles);
    segment.reserve<FaceRecord>(SectionKind::Faces, faces);
    segment.reserve<EdgeSegment>(SectionKind::EdgeSegments, segments);
    segment.reserve<EdgeChunk>(SectionKind::EdgeChunks, chunks);
    segment.allocate();

    auto* records = segment.at<DefinitionRecord>(SectionKind::Definitions);
    auto* meshletOut = segment.at<Meshlet>(SectionKind::Meshlets);
    auto* vertexOut = segment.at<PackedVertex>(SectionKind::Vertices);
    auto* highOut = segment.at<u32>(SectionKind::HighBits);
    auto* triangleOut = segment.at<u32>(SectionKind::Triangles);
    auto* faceOut = segment.at<FaceRecord>(SectionKind::Faces);
    auto* segmentOut = segment.at<EdgeSegment>(SectionKind::EdgeSegments);
    auto* chunkOut = segment.at<EdgeChunk>(SectionKind::EdgeChunks);

    u32 meshletAt = 0, vertexAt = 0, highAt = 0, triangleAt = 0, faceAt = 0, segmentAt = 0,
        chunkAt = 0;
    for (usize i = 0; i < pending_.size(); ++i) {
        const Pending& item = pending_[i];
        const PackedDefinition& d = item.definition;
        DefinitionRecord& record = records[i];
        record.boundsMin = d.boundsMin;
        record.boundsMax = d.boundsMax;
        record.decodeScale = d.decodeScale;
        record.boundsScale = d.boundsScale;
        record.definition = item.index;
        record.level = item.level;
        record.flags = d.flags;
        record.faceCount = u32(item.faces.size());
        record.firstMeshlet = meshletAt;
        record.meshletCount = u32(d.meshlets.size());
        record.firstVertex = vertexAt;
        record.vertexCount = u32(d.vertices.size());
        record.firstHighBits = highAt;
        record.firstTriangle = triangleAt;
        record.triangleCount = u32(d.triangles.size());
        record.firstFace = faceAt;
        record.firstEdgeChunk = chunkAt;
        record.edgeChunkCount = u32(d.edgeChunks.size());
        record.firstEdgeSegment = segmentAt;
        record.edgeSegmentCount = u32(d.edgeSegments.size());

        std::copy(d.meshlets.begin(), d.meshlets.end(), meshletOut + meshletAt);
        std::copy(d.vertices.begin(), d.vertices.end(), vertexOut + vertexAt);
        std::copy(d.highBits.begin(), d.highBits.end(), highOut + highAt);
        std::copy(d.triangles.begin(), d.triangles.end(), triangleOut + triangleAt);
        std::copy(item.faces.begin(), item.faces.end(), faceOut + faceAt);
        std::copy(d.edgeSegments.begin(), d.edgeSegments.end(), segmentOut + segmentAt);
        std::copy(d.edgeChunks.begin(), d.edgeChunks.end(), chunkOut + chunkAt);
        meshletAt += u32(d.meshlets.size());
        vertexAt += u32(d.vertices.size());
        highAt += u32(d.highBits.size());
        triangleAt += u32(d.triangles.size());
        faceAt += u32(item.faces.size());
        segmentAt += u32(d.edgeSegments.size());
        chunkAt += u32(d.edgeChunks.size());
    }

    const std::vector<u8>& bytes = segment.bytes();
    if (!writeAll(bytes.data(), bytes.size(), offset_)) {
        return false;
    }
    const u64 start = offset_;
    offset_ += bytes.size();
    pending_.clear();
    pendingBytes_ = 0;
    lastFlush_ = nowSeconds();
    return recordSegment(start, bytes.size());
}

bool BlobStream::finish(const std::array<u8, 16>& sourceHash) {
    std::lock_guard lock(mutex_);
    if (!flush()) {
        return false;
    }
    header_.magic = kMagic;
    header_.version = kVersion;
    header_.fileSize = offset_;
    header_.sourceHash = sourceHash;
    if (!writeAll(&header_, sizeof(header_), 0)) {
        return false;
    }
    if (ftruncate(fd_, off_t(offset_)) != 0) {
        error_ = std::format("Could not size the cache file: {}.", std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace geo::import
