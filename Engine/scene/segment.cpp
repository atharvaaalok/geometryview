#include "scene/segment.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace geo::scene {

std::shared_ptr<Segment> Segment::open(int fd, u64 offset, u64 size,
                                                   std::string& error) {
    if (size == 0 || size % format::kAlignment != 0 || offset % format::kAlignment != 0) {
        error = "A cache segment is misaligned.";
        return nullptr;
    }
    // In a 32-bit address space, as in WebAssembly, a segment this large cannot be mapped at all.
    if (size > u64(SIZE_MAX)) {
        error = "A cache segment is too large for this device.";
        return nullptr;
    }
    void* mapping = mmap(nullptr, usize(size), PROT_READ, MAP_SHARED, fd, off_t(offset));
    if (mapping == MAP_FAILED) {
        error = std::format("A cache segment could not be mapped: {}.", std::strerror(errno));
        return nullptr;
    }
    std::shared_ptr<Segment> segment(new Segment);
    segment->data_ = mapping;
    segment->size_ = usize(size);
    segment->mapped_ = true;
    if (!format::parseSegment({static_cast<const u8*>(mapping), usize(size)}, segment->view_,
                              error)) {
        return nullptr;
    }
    return segment;
}

std::shared_ptr<Segment> Segment::copy(std::span<const u8> bytes, std::string& error) {
    if (bytes.empty() || bytes.size() % format::kAlignment != 0) {
        error = "A cache segment is misaligned.";
        return nullptr;
    }
    void* memory = std::aligned_alloc(format::kAlignment, bytes.size());
    if (memory == nullptr) {
        error = "There is not enough memory for a cache segment.";
        return nullptr;
    }
    std::memcpy(memory, bytes.data(), bytes.size());
    std::shared_ptr<Segment> segment(new Segment);
    segment->data_ = memory;
    segment->size_ = bytes.size();
    if (!format::parseSegment({static_cast<const u8*>(memory), bytes.size()}, segment->view_,
                              error)) {
        return nullptr;
    }
    return segment;
}

Segment::~Segment() {
    if (data_ == nullptr) {
        return;
    }
    if (mapped_) {
        munmap(data_, size_);
    } else {
        std::free(data_);
    }
}

bool openCacheFile(int fd, std::vector<std::shared_ptr<Segment>>& segments,
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
        std::shared_ptr<Segment> segment =
            Segment::open(fd, entry.offset, entry.size, error);
        if (!segment) {
            return false;
        }
        segments.push_back(std::move(segment));
    }
    return true;
}

bool readCacheFile(std::span<const u8> file, std::vector<std::shared_ptr<Segment>>& segments,
                   std::string& error) {
    std::span<const format::SegmentEntry> entries;
    if (!format::parseHeader(file, entries, error)) {
        return false;
    }
    segments.clear();
    for (const format::SegmentEntry& entry : entries) {
        std::shared_ptr<Segment> segment =
            Segment::copy(file.subspan(usize(entry.offset), usize(entry.size)), error);
        if (!segment) {
            return false;
        }
        segments.push_back(std::move(segment));
    }
    return true;
}

} // namespace geo::scene
