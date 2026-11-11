// What the gallery knows about a file without drawing it: its size, what it is made of and its
// name. Facts come from a cache file, so they are the same on every platform, and they are kept
// after the cache file itself is evicted.
#pragma once

#include "base/json.h"
#include "base/types.h"
#include "scene/segment.h"

#include <array>
#include <memory>
#include <span>
#include <string>

namespace geo::scene {

struct Facts {
    /// Bumped whenever what facts hold, or how they are measured, changes; kept facts of another
    /// version are measured again.
    static constexpr u32 kVersion = 1;

    /// The model's box, in millimetres, as drawn: every body placed. Empty when nothing is known.
    std::array<double, 3> min{};
    std::array<double, 3> max{};
    /// The box was estimated from the file's structure, before any geometry was read. Estimates are
    /// boxes around placed boxes, so they can be larger than the part, never smaller.
    bool estimated = true;
    /// Whether any body has a box at all.
    bool hasBox = false;

    u64 triangles = 0;   // as drawn: a part used twice counts twice
    u64 faces = 0;       // likewise
    u32 parts = 0;       // the parts the colour cycle counts: one colour each
    u32 bodies = 0;      // placed bodies
    u32 definitions = 0; // distinct bodies
    /// The name inside the file: its top product, or the first named part.
    std::string name;
    /// The hash of the source file's bytes, as hex, or empty when it is not known.
    std::string hash;

    std::array<double, 3> size() const {
        return {max[0] - min[0], max[1] - min[1], max[2] - min[2]};
    }

    json::Value toJson() const;
    /// Reads facts written by toJson. Returns false for anything else, or another version.
    static bool fromJson(const json::Value& value, Facts& facts);

    bool operator==(const Facts&) const = default;
};

/// Measures a file from its segments: the model and whatever geometry has arrived. Without a model,
/// returns empty facts.
Facts measure(std::span<const std::shared_ptr<Segment>> segments);

/// Measures a finished cache file, its source hash included. Returns false and sets `error` if it
/// cannot be read.
bool measureCacheFile(const std::string& path, Facts& facts, std::string& error);

} // namespace geo::scene
