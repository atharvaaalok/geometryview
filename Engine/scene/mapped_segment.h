// One cache segment mapped read-only and validated.
#pragma once

#include "base/types.h"
#include "format/blob.h"

#include <memory>
#include <string>
#include <vector>

namespace geo::scene {

class MappedSegment {
public:
    /// Maps [offset, offset + size) of `fd`, which stays usable after the descriptor closes.
    /// Returns null and sets `error` if it cannot be mapped or fails validation.
    static std::shared_ptr<MappedSegment> open(int fd, u64 offset, u64 size, std::string& error);

    ~MappedSegment();
    MappedSegment(const MappedSegment&) = delete;
    MappedSegment& operator=(const MappedSegment&) = delete;

    const format::SegmentView& view() const { return view_; }
    void* data() const { return data_; }
    usize size() const { return size_; }

private:
    MappedSegment() = default;

    void* data_ = nullptr;
    usize size_ = 0;
    format::SegmentView view_;
};

/// Maps every segment of a finished cache file. Returns false and sets `error` if the file is not
/// valid, which callers treat as a cache miss.
bool openCacheFile(int fd, std::vector<std::shared_ptr<MappedSegment>>& segments,
                   std::string& error);

} // namespace geo::scene
