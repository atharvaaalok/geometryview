#include "scene/mapped_segment.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace geo::scene {

std::shared_ptr<MappedSegment> MappedSegment::open(int fd, u64 offset, u64 size,
                                                   std::string& error) {
    if (size == 0 || size % format::kAlignment != 0 || offset % format::kAlignment != 0) {
        error = "A cache segment is misaligned.";
        return nullptr;
    }
    void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, off_t(offset));
    if (mapping == MAP_FAILED) {
        error = std::format("A cache segment could not be mapped: {}.", std::strerror(errno));
        return nullptr;
    }
    std::shared_ptr<MappedSegment> segment(new MappedSegment);
    segment->data_ = mapping;
    segment->size_ = size;
    if (!format::parseSegment({static_cast<const u8*>(mapping), size}, segment->view_, error)) {
        return nullptr;
    }
    return segment;
}

MappedSegment::~MappedSegment() {
    if (data_ != nullptr) {
        munmap(data_, size_);
    }
}

bool openCacheFile(int fd, std::vector<std::shared_ptr<MappedSegment>>& segments,
                   std::string& error) {
    struct stat info{};
    if (fstat(fd, &info) != 0 || usize(info.st_size) < sizeof(format::Header)) {
        error = "The cache file could not be read.";
        return false;
    }
    format::Header header;
    if (pread(fd, &header, sizeof(header), 0) != ssize_t(sizeof(header))) {
        error = "The cache file could not be read.";
        return false;
    }
    if (header.magic != format::kMagic || header.version != format::kVersion ||
        header.fileSize != u64(info.st_size) || header.segmentCount > format::kMaxSegments) {
        error = "The cache file is not valid.";
        return false;
    }
    segments.clear();
    for (u32 i = 0; i < header.segmentCount; ++i) {
        const format::SegmentEntry& entry = header.segments[i];
        if (entry.offset > u64(info.st_size) || entry.size > u64(info.st_size) - entry.offset) {
            error = "A segment lies outside the cache file.";
            return false;
        }
        std::shared_ptr<MappedSegment> segment =
            MappedSegment::open(fd, entry.offset, entry.size, error);
        if (!segment) {
            return false;
        }
        segments.push_back(std::move(segment));
    }
    return true;
}

} // namespace geo::scene
