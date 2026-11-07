// A cache file mapped read-only and validated.
#pragma once

#include "base/types.h"
#include "format/blob.h"

#include <span>
#include <string>

namespace geo::scene {

class MappedBlob {
public:
    MappedBlob() = default;
    ~MappedBlob();
    MappedBlob(const MappedBlob&) = delete;
    MappedBlob& operator=(const MappedBlob&) = delete;

    bool open(int fd, std::string& error);

    const format::BlobView& view() const { return view_; }
    void* data() const { return data_; }
    usize size() const { return size_; }

    /// Hands the mapping to a new owner, such as a Metal buffer's deallocator.
    void releaseOwnership() { data_ = nullptr; }

private:
    void* data_ = nullptr;
    usize size_ = 0;
    format::BlobView view_;
};

} // namespace geo::scene
