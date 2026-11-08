// One validated cache segment, read-only. On the Mac it is a mapping of the cache file, so geometry
// exists once in memory and is backed by a clean file. Where files cannot be mapped, as in the
// browser, it holds a copy of the bytes, aligned as a mapping would be.
#pragma once

#include "base/types.h"
#include "format/blob.h"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace geo::scene {

class Segment {
public:
    /// Maps [offset, offset + size) of `fd`, which stays usable after the descriptor closes.
    /// Returns null and sets `error` if it cannot be mapped or fails validation.
    static std::shared_ptr<Segment> open(int fd, u64 offset, u64 size, std::string& error);

    /// Copies a segment's bytes into memory it owns, aligned to the cache's 16 KB, so every
    /// renderer can use them as it would a mapping. Returns null and sets `error` if they fail
    /// validation.
    static std::shared_ptr<Segment> copy(std::span<const u8> bytes, std::string& error);

    ~Segment();
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    const format::SegmentView& view() const { return view_; }
    void* data() const { return data_; }
    usize size() const { return size_; }
    /// Whether the bytes are a mapping of a file, rather than a copy.
    bool mapped() const { return mapped_; }

private:
    Segment() = default;

    void* data_ = nullptr;
    usize size_ = 0;
    bool mapped_ = false;
    format::SegmentView view_;
};

/// Maps every segment of a finished cache file. Returns false and sets `error` if the file is not
/// valid, which callers treat as a cache miss.
bool openCacheFile(int fd, std::vector<std::shared_ptr<Segment>>& segments, std::string& error);

/// Copies every segment of a finished cache file held in memory, validating it as openCacheFile
/// does.
bool readCacheFile(std::span<const u8> file, std::vector<std::shared_ptr<Segment>>& segments,
                   std::string& error);

} // namespace geo::scene
