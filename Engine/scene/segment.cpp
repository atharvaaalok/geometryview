#include "scene/segment.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace geo::scene {

std::shared_ptr<Segment> Segment::open(int fd, u64 offset, u64 size, std::string& error) {
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

std::shared_ptr<Segment> Segment::resident(const format::SegmentHeader& header, u64 size,
                                           std::shared_ptr<u8[]> held,
                                           const format::SectionBytes& memory,
                                           std::shared_ptr<Placement> placement,
                                           std::string& error) {
    if (size == 0 || size % format::kAlignment != 0 || size > u64(SIZE_MAX)) {
        error = "A cache segment is misaligned.";
        return nullptr;
    }
    std::shared_ptr<Segment> segment(new Segment);
    segment->size_ = usize(size);
    segment->held_ = std::move(held);
    segment->placement_ = std::move(placement);
    if (!format::parseSegmentParts(header, size, memory, segment->view_, error)) {
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

bool openCacheFile(int fd, std::vector<std::shared_ptr<Segment>>& segments, std::string& error) {
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
        std::shared_ptr<Segment> segment = Segment::open(fd, entry.offset, entry.size, error);
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

// ---- CacheStream ------------------------------------------------------------------------------

// The segment being read: its header first, then its bytes, routed as they arrive.
struct CacheStream::Current {
    struct Held {
        u64 offset = 0; // in the segment
        u64 size = 0;
        usize at = 0; // in `held`
    };

    format::SegmentEntry entry{};
    std::array<u8, sizeof(format::SegmentHeader)> headerBytes{};
    bool begun = false; // the header is in and checked
    format::SegmentHeader header{};
    std::shared_ptr<Placement> placement;
    std::vector<u8> whole; // the segment's bytes, where it is kept in memory
    std::shared_ptr<u8[]> held;
    std::vector<Held> heldSections;
    format::SectionBytes memory{};

    // Bytes at `at` within the segment, to wherever they belong.
    void route(SegmentSink* sink, u64 at, std::span<const u8> bytes) {
        if (placement) {
            sink->write(*placement, at, bytes);
        } else {
            // Grown as bytes arrive, so a header cannot make memory be taken for bytes that never
            // come.
            if (whole.size() < usize(at) + bytes.size()) {
                whole.resize(usize(at) + bytes.size());
            }
            std::memcpy(whole.data() + usize(at), bytes.data(), bytes.size());
        }
        const u64 end = at + bytes.size();
        for (const Held& h : heldSections) {
            const u64 from = std::max(at, h.offset), to = std::min(end, h.offset + h.size);
            if (from < to) {
                std::memcpy(held.get() + h.at + usize(from - h.offset),
                            bytes.data() + usize(from - at), usize(to - from));
            }
        }
    }
};

CacheStream::CacheStream(SegmentSink* sink, Ready ready) : sink_(sink), ready_(std::move(ready)) {}
CacheStream::~CacheStream() = default;

bool CacheStream::takeHeader(std::span<const u8>& bytes, std::string& error) {
    const usize take = std::min(bytes.size(), format::kAlignment - prefix_.size());
    prefix_.insert(prefix_.end(), bytes.begin(), bytes.begin() + std::ptrdiff_t(take));
    bytes = bytes.subspan(take);
    if (prefix_.size() < format::kAlignment) {
        return true;
    }
    std::span<const format::SegmentEntry> entries;
    if (!format::parseHeaderPrefix(prefix_, entries, fileSize_, error)) {
        return false;
    }
    if (expectedSize_ != 0 && fileSize_ != expectedSize_) {
        error = "The cache file's size does not match its header.";
        return false;
    }
    entries_.assign(entries.begin(), entries.end());
    std::sort(entries_.begin(), entries_.end(),
              [](const format::SegmentEntry& a, const format::SegmentEntry& b) {
                  return a.offset < b.offset;
              });
    // Read in order, segments must follow the header and one another without overlapping.
    u64 end = format::kAlignment;
    for (const format::SegmentEntry& entry : entries_) {
        if (entry.offset < end) {
            error = "The cache file's segments overlap.";
            return false;
        }
        end = entry.offset + entry.size;
    }
    prefix_.clear();
    prefix_.shrink_to_fit();
    if (sink_ != nullptr) {
        sink_->expect(fileSize_);
    }
    return true;
}

bool CacheStream::beginSegment(std::string& error) {
    Current& c = *current_;
    std::memcpy(&c.header, c.headerBytes.data(), sizeof(c.header));
    const u64 size = c.entry.size;
    if (c.header.magic != format::kSegmentMagic || c.header.version != format::kVersion ||
        c.header.size != size || c.header.sectionCount > format::kMaxSections ||
        size > u64(SIZE_MAX)) {
        error = "A cache segment is damaged.";
        return false;
    }
    // The first section of each kind the CPU reads is kept, as parseSegmentParts reads them.
    usize total = 0;
    std::array<bool, format::kSectionKinds> seen{};
    for (u32 i = 0; i < c.header.sectionCount; ++i) {
        const format::SectionEntry& section = c.header.sections[i];
        if (section.kind >= format::kSectionKinds || seen[section.kind] ||
            !format::readOnCpu(format::SectionKind(section.kind))) {
            continue;
        }
        seen[section.kind] = true;
        if (section.offset > size || section.size > size - section.offset) {
            error = std::format("Section {} has an invalid layout.", section.kind);
            return false;
        }
        const usize at = (total + 15) & ~usize(15);
        c.heldSections.push_back({section.offset, section.size, at});
        total = at + usize(section.size);
    }
    c.held = std::shared_ptr<u8[]>(
        static_cast<u8*>(std::aligned_alloc(16, std::max<usize>((total + 15) & ~usize(15), 16))),
        std::free);
    if (!c.held) {
        error = "There is not enough memory for a cache segment.";
        return false;
    }
    for (u32 i = 0; i < c.header.sectionCount; ++i) {
        const format::SectionEntry& section = c.header.sections[i];
        if (section.kind < format::kSectionKinds && c.memory[section.kind] == nullptr) {
            for (const Current::Held& h : c.heldSections) {
                if (h.offset == section.offset && h.size == section.size) {
                    c.memory[section.kind] = c.held.get() + h.at;
                    break;
                }
            }
        }
    }
    c.placement = sink_ != nullptr ? sink_->place(size) : nullptr;
    c.begun = true;
    c.route(sink_, 0, c.headerBytes);
    return true;
}

bool CacheStream::endSegment(std::string& error) {
    Current& c = *current_;
    std::shared_ptr<Segment> segment =
        c.placement ? Segment::resident(c.header, c.entry.size, std::move(c.held), c.memory,
                                        std::move(c.placement), error)
                    : Segment::copy(c.whole, error);
    if (!segment) {
        return false;
    }
    current_.reset();
    ++next_;
    ready_(std::move(segment));
    return true;
}

bool CacheStream::append(std::span<const u8> bytes, std::string& error) {
    if (failed_) {
        error = "The cache file was already found invalid.";
        return false;
    }
    u64 position = received_;
    received_ += bytes.size();
    const auto fail = [&] {
        failed_ = true;
        current_.reset();
        return false;
    };
    while (!bytes.empty()) {
        if (position < format::kAlignment) {
            const usize before = bytes.size();
            if (!takeHeader(bytes, error)) {
                return fail();
            }
            position += before - bytes.size();
            continue;
        }
        if (next_ >= entries_.size()) {
            if (received_ > fileSize_) {
                error = "The cache file is longer than its header says.";
                return fail();
            }
            return true; // padding after the last segment
        }
        const format::SegmentEntry& entry = entries_[next_];
        if (position < entry.offset) {
            const usize skip = usize(std::min<u64>(bytes.size(), entry.offset - position));
            position += skip;
            bytes = bytes.subspan(skip);
            continue;
        }
        if (!current_) {
            current_ = std::make_unique<Current>();
            current_->entry = entry;
        }
        Current& c = *current_;
        u64 at = position - entry.offset;
        const usize take = usize(std::min<u64>(bytes.size(), entry.offset + entry.size - position));
        std::span<const u8> chunk = bytes.first(take);
        if (!c.begun) {
            const usize head = usize(std::min<u64>(take, sizeof(format::SegmentHeader) - at));
            std::memcpy(c.headerBytes.data() + usize(at), chunk.data(), head);
            chunk = chunk.subspan(head);
            at += head;
            if (at == sizeof(format::SegmentHeader) && !beginSegment(error)) {
                return fail();
            }
        }
        if (!chunk.empty()) {
            c.route(sink_, at, chunk);
        }
        position += take;
        bytes = bytes.subspan(take);
        if (position == entry.offset + entry.size && !endSegment(error)) {
            return fail();
        }
    }
    return true;
}

bool CacheStream::finish(std::string& error) {
    if (failed_) {
        error = "The cache file was already found invalid.";
        return false;
    }
    if (fileSize_ == 0 || received_ != fileSize_ || next_ != entries_.size()) {
        error = "The cache file ended early.";
        return false;
    }
    return true;
}

} // namespace geo::scene
