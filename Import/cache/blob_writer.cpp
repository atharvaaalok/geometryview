#include "cache/blob_writer.h"

#include "format/blob.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <unistd.h>
#include <vector>

namespace geo::import {

using namespace format;

namespace {

u64 alignUp(u64 value) {
    return (value + kAlignment - 1) & ~u64(kAlignment - 1);
}

class SectionWriter {
public:
    SectionWriter(int fd, Header& header) : fd_(fd), header_(header), offset_(kAlignment) {}

    template <typename T>
    void add(SectionKind kind, std::span<const T> items) {
        if (items.empty() || failed()) {
            return;
        }
        const u64 size = items.size_bytes();
        header_.sections[header_.sectionCount++] = {u32(kind), u32(sizeof(T)), offset_, size};
        writeAll(items.data(), size, offset_);
        offset_ = alignUp(offset_ + size);
    }

    bool writeAll(const void* data, u64 size, u64 offset) {
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

    u64 end() const { return offset_; }
    bool failed() const { return !error_.empty(); }
    const std::string& error() const { return error_; }

private:
    int fd_;
    Header& header_;
    u64 offset_;
    std::string error_;
};

} // namespace

bool writeBlob(int fd, const PackedDefinition& definition, const std::array<u8, 16>& sourceHash,
               std::string& error) {
    Header header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.sourceHash = sourceHash;

    DefinitionRecord record{};
    record.boundsMin = definition.boundsMin;
    record.boundsMax = definition.boundsMax;
    record.decodeScale = definition.decodeScale;
    record.boundsScale = definition.boundsScale;
    record.flags = definition.flags;
    record.faceCount = definition.faceCount;
    record.meshletCount = u32(definition.meshlets.size());

    OccurrenceRecord occurrence{};
    occurrence.transform = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

    SectionWriter writer(fd, header);
    writer.add<DefinitionRecord>(SectionKind::Definitions, {&record, 1});
    writer.add<Meshlet>(SectionKind::Meshlets, definition.meshlets);
    writer.add<PackedVertex>(SectionKind::Vertices, definition.vertices);
    writer.add<u32>(SectionKind::HighBits, definition.highBits);
    writer.add<u32>(SectionKind::Triangles, definition.triangles);
    writer.add<OccurrenceRecord>(SectionKind::Occurrences, {&occurrence, 1});
    writer.add<u32>(SectionKind::OriginalFaces, definition.originalFace);

    header.fileSize = writer.end();
    if (!writer.failed()) {
        writer.writeAll(&header, sizeof(header), 0);
    }
    if (writer.failed()) {
        error = writer.error();
        return false;
    }
    if (ftruncate(fd, off_t(header.fileSize)) != 0) {
        error = std::format("Could not size the cache file: {}.", std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace geo::import
