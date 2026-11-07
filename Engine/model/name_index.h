// Finds tree nodes by name. Every name is folded to lower case once, with a zero byte after each,
// so a scan of one buffer finds every match and no match can span two names. The scan is split
// across cores at name boundaries.
#pragma once

#include "base/types.h"
#include "format/blob.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace geo::model {

class NameIndex {
public:
    explicit NameIndex(const format::SegmentView& model);

    /// Nodes whose names contain `query`, ignoring ASCII case, in tree order. Writes up to
    /// `results.size()` of them and returns how many matched in all.
    u32 search(std::string_view query, std::span<u32> results) const;

private:
    // Scans nodes [first, last) and appends what matches.
    void scan(std::string_view needle, u32 first, u32 last, std::vector<u32>& out) const;

    std::string folded_;
    std::vector<u32> starts_; // where each node's name begins in `folded_`, plus the end
};

} // namespace geo::model
