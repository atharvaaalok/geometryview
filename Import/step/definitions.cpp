#include "step/definitions.h"

#include "base/parallel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <format>
#include "base/simd.h"
#include <unordered_map>

#define XXH_INLINE_ALL
#include "base/xxhash.h"

namespace geo::import::step {
namespace {

constexpr usize kFingerprintBuffer = 4096;

// Hashes a record's meaning: its text with whitespace, strings and reference numbers removed. Names
// differ between otherwise identical solids, and line wrapping moves with the digits of an id.
u64 fingerprint(std::string_view record) {
    const char* p = record.data();
    const char* end = p + record.size();
    while (p < end && *p != '=') {
        ++p;
    }

    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    char buffer[kFingerprintBuffer];
    usize used = 0;
    for (; p < end; ++p) {
        const char c = *p;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        if (c == '\'') {
            ++p;
            while (p < end && *p != '\'') {
                ++p;
            }
        } else if (c == '#') {
            while (p + 1 < end && p[1] >= '0' && p[1] <= '9') {
                ++p;
            }
        }
        buffer[used++] = c;
        if (used == kFingerprintBuffer) {
            XXH3_64bits_update(&state, buffer, used);
            used = 0;
        }
    }
    if (record.size() < kFingerprintBuffer) {
        return XXH3_64bits(buffer, used);
    }
    XXH3_64bits_update(&state, buffer, used);
    return XXH3_64bits_digest(&state);
}

struct BodyHash {
    XXH128_hash_t hash{};
    std::array<double, 3> lo{}, hi{};
};

// The three numbers of a CARTESIAN_POINT, or of a DIRECTION with `kind`.
bool readPoint(const Part21& index, u32 record, simd_double3& point,
               Kind kind = Kind::CARTESIAN_POINT) {
    if (record == kNoRecord || index.kinds[record] != kind) {
        return false;
    }
    ParamCursor cursor(index.text(record));
    double xyz[3] = {0, 0, 0};
    if (!cursor.skip() || !cursor.enterList()) {
        return false;
    }
    for (double& value : xyz) {
        if (!cursor.inList() || !cursor.readReal(value)) {
            return false;
        }
    }
    point = simd_make_double3(xyz[0], xyz[1], xyz[2]);
    return true;
}

struct PointBounds {
    simd_double3 lo = {INFINITY, INFINITY, INFINITY};
    simd_double3 hi = {-INFINITY, -INFINITY, -INFINITY};

    void add(simd_double3 point) {
        lo = simd_min(lo, point);
        hi = simd_max(hi, point);
    }
    bool empty() const { return lo.x > hi.x; }
};

// Adds the reach of a circle or round surface, whose vertices alone can be a single point.
void addCurvedExtent(const Part21& index, u32 record, PointBounds& bounds) {
    const std::span<const u32> refs = index.refsOf(record);
    if (refs.empty() || refs[0] == kNoRecord || index.kinds[refs[0]] != Kind::AXIS2_PLACEMENT_3D) {
        return;
    }
    const std::span<const u32> placement = index.refsOf(refs[0]);
    simd_double3 center;
    if (placement.empty() || !readPoint(index, placement[0], center)) {
        return;
    }
    ParamCursor cursor(index.text(record));
    double first = 0.0, second = 0.0;
    if (!cursor.skip() || !cursor.skip() || !cursor.readReal(first)) {
        return;
    }
    cursor.readReal(second);
    const Kind kind = index.kinds[record];
    const double reach = kind == Kind::TOROIDAL_SURFACE ? first + second : std::max(first, second);
    if (reach > 0.0 && std::isfinite(reach)) {
        bounds.add(center - reach);
        bounds.add(center + reach);
    }
}

// Adds the reach of a surface of revolution whose curve is a circle or an ellipse, as its curve's
// box swept about the axis. Only for a body whose vertices say nothing of its size: for any other,
// a whole turn about a far axis could overstate a small face as much as a whole circle would.
void addRevolvedExtent(const Part21& index, u32 record, PointBounds& bounds) {
    const std::span<const u32> refs = index.refsOf(record);
    if (refs.size() < 2 || refs[0] == kNoRecord || refs[1] == kNoRecord ||
        index.kinds[refs[1]] != Kind::AXIS1_PLACEMENT) {
        return;
    }
    PointBounds curve;
    const Kind curveKind = index.kinds[refs[0]];
    if (curveKind == Kind::CIRCLE || curveKind == Kind::ELLIPSE) {
        addCurvedExtent(index, refs[0], curve);
    }
    const std::span<const u32> axis = index.refsOf(refs[1]);
    simd_double3 origin, direction;
    if (curve.empty() || axis.size() < 2 || !readPoint(index, axis[0], origin) ||
        !readPoint(index, axis[1], direction, Kind::DIRECTION) || simd_length(direction) == 0.0) {
        return;
    }
    direction = simd_normalize(direction);
    double radius = 0.0, low = INFINITY, high = -INFINITY;
    for (int corner = 0; corner < 8; ++corner) {
        const simd_double3 point = {corner & 1 ? curve.hi.x : curve.lo.x,
                                    corner & 2 ? curve.hi.y : curve.lo.y,
                                    corner & 4 ? curve.hi.z : curve.lo.z};
        const simd_double3 offset = point - origin;
        const double along = simd_dot(offset, direction);
        radius = std::max(radius, simd_length(offset - along * direction));
        low = std::min(low, along);
        high = std::max(high, along);
    }
    // A disc of that radius about the axis reaches radius * sqrt(1 - d_i^2) along each axis i.
    const simd_double3 reach =
        radius * simd_make_double3(std::sqrt(1.0 - direction.x * direction.x),
                                   std::sqrt(1.0 - direction.y * direction.y),
                                   std::sqrt(1.0 - direction.z * direction.z));
    for (const double along : {low, high}) {
        bounds.add(origin + along * direction - reach);
        bounds.add(origin + along * direction + reach);
    }
}

// Bounds of the points that shape `records`. Curves can bulge beyond their vertices, so this is an
// estimate, good for framing and for choosing tolerances. With `curves`, round surfaces count too,
// which matters for a body with few vertices, such as a closed sphere. Circles count only when they
// are no larger than the vertices' own extent: a shallow arc of a huge circle is common, and its
// whole circle would wildly overstate the body. Where the vertices are all at one point, as on a
// ring whose one face closes on itself, they give no extent: then circles count whole, and a
// surface of revolution counts as swept about its axis. Bounds of a single point made the
// tolerances those of a body one unit across, about 70 times too fine for a 47 mm seal.
PointBounds boundsOf(const Part21& index, std::span<const u32> records, bool curves) {
    PointBounds bounds;
    for (const u32 record : records) {
        if (index.kinds[record] == Kind::VERTEX_POINT) {
            const std::span<const u32> refs = index.refsOf(record);
            simd_double3 point;
            if (!refs.empty() && readPoint(index, refs[0], point)) {
                bounds.add(point);
            }
        }
    }
    if (curves) {
        const double extent = bounds.empty() ? 0.0 : simd_length(bounds.hi - bounds.lo);
        PointBounds rounded = bounds;
        for (const u32 record : records) {
            const Kind kind = index.kinds[record];
            if (kind == Kind::SPHERICAL_SURFACE || kind == Kind::TOROIDAL_SURFACE) {
                addCurvedExtent(index, record, rounded);
            } else if (kind == Kind::CIRCLE || kind == Kind::ELLIPSE) {
                PointBounds circle;
                addCurvedExtent(index, record, circle);
                if (!circle.empty() &&
                    (extent == 0.0 || simd_length(circle.hi - circle.lo) <= 2.0 * extent)) {
                    rounded.add(circle.lo);
                    rounded.add(circle.hi);
                }
            } else if (kind == Kind::SURFACE_OF_REVOLUTION && extent == 0.0) {
                addRevolvedExtent(index, record, rounded);
            }
        }
        bounds = rounded;
    }
    if (bounds.empty()) {
        // No vertices at all: use every point the body mentions.
        for (const u32 record : records) {
            simd_double3 point;
            if (readPoint(index, record, point)) {
                bounds.add(point);
            }
        }
    }
    return bounds;
}

// Walks in discovery order, numbering records as it finds them, so the hash depends on shape alone
// and not on the file's ids.
class CanonicalHasher {
public:
    explicit CanonicalHasher(const Part21& index)
        : index_(index), walker_(index), order_(index.recordCount()) {}

    XXH128_hash_t hash(u32 root, u64 salt, std::span<const FaceUse> faces) {
        walker_.begin();
        walker_.add(root);
        const std::vector<u32>& records = walker_.visited();
        for (usize i = 0; i < records.size(); ++i) {
            order_[records[i]] = u32(i);
        }

        XXH3_state_t state;
        XXH3_128bits_reset(&state);
        XXH3_128bits_update(&state, &salt, sizeof(salt));
        words_.clear();
        for (const u32 record : records) {
            const u64 print = fingerprint(index_.text(record));
            words_.push_back(u32(print));
            words_.push_back(u32(print >> 32));
            for (const u32 ref : index_.refsOf(record)) {
                words_.push_back(ref == kNoRecord ? kNoRecord : order_[ref]);
            }
            if (words_.size() >= 1 << 16) {
                XXH3_128bits_update(&state, words_.data(), words_.size() * sizeof(u32));
                words_.clear();
            }
        }
        // Two solids differing only in the colours of their faces must stay distinct.
        for (const FaceUse& face : faces) {
            words_.push_back(face.color);
        }
        XXH3_128bits_update(&state, words_.data(), words_.size() * sizeof(u32));
        return XXH3_128bits_digest(&state);
    }

    const std::vector<u32>& lastClosure() const { return walker_.visited(); }

private:
    const Part21& index_;
    ClosureWalker walker_;
    std::vector<u32> order_;
    std::vector<u32> words_;
};

u32 interleaveBits(u32 v) {
    v &= 0x3FF;
    v = (v | (v << 16)) & 0x030000FF;
    v = (v | (v << 8)) & 0x0300F00F;
    v = (v | (v << 4)) & 0x030C30C3;
    v = (v | (v << 2)) & 0x09249249;
    return v;
}

u32 mortonCode(simd_double3 point, const PointBounds& bounds) {
    const simd_double3 extent = simd_max(bounds.hi - bounds.lo, simd_double3(1e-30));
    const simd_double3 unit = simd_clamp((point - bounds.lo) / extent, 0.0, 1.0) * 1023.0;
    return interleaveBits(u32(unit.x)) | interleaveBits(u32(unit.y)) << 1 |
           interleaveBits(u32(unit.z)) << 2;
}

} // namespace

ClosureWalker::ClosureWalker(const Part21& index)
    : index_(index), stamps_(static_cast<u32*>(std::calloc(index.recordCount() + 1, sizeof(u32)))) {
}

ClosureWalker::~ClosureWalker() {
    std::free(stamps_);
}

void ClosureWalker::begin() {
    ++generation_;
    visited_.clear();
}

void ClosureWalker::add(u32 root) {
    if (root == kNoRecord || stamps_[root] == generation_) {
        return;
    }
    stamps_[root] = generation_;
    stack_.push_back(root);
    while (!stack_.empty()) {
        const u32 record = stack_.back();
        stack_.pop_back();
        visited_.push_back(record);
        const std::span<const u32> refs = index_.refsOf(record);
        for (usize i = refs.size(); i-- > 0;) {
            const u32 next = refs[i];
            if (next != kNoRecord && stamps_[next] != generation_) {
                stamps_[next] = generation_;
                stack_.push_back(next);
            }
        }
    }
}

Definitions buildDefinitions(const Part21& index, const Structure& structure, usize chunkBytes) {
    const usize bodyCount = structure.bodies.size();
    std::vector<BodyHash> hashes(bodyCount);

    // Bodies are claimed one at a time, so large and small ones share the cores evenly. Each worker
    // holds arrays as long as the file has records, but only touches the pages it uses.
    std::atomic<usize> nextBody{0};
    const usize workers = parallelRangeCount(bodyCount, 1);
    parallelFor(workers, [&](usize) {
        CanonicalHasher hasher(index);
        while (true) {
            const usize b = nextBody.fetch_add(1, std::memory_order_relaxed);
            if (b >= bodyCount) {
                return;
            }
            const Body& body = structure.bodies[b];
            const double unit = lengthFactor(index, body.context);
            u64 salt = 0;
            std::memcpy(&salt, &unit, sizeof(salt));
            const std::span<const FaceUse> faces(structure.faces.data() + body.firstFace,
                                                 body.faceCount);
            hashes[b].hash = hasher.hash(body.record, salt, faces);
            const PointBounds bounds = boundsOf(index, hasher.lastClosure(), true);
            if (!bounds.empty()) {
                for (int axis = 0; axis < 3; ++axis) {
                    hashes[b].lo[axis] = bounds.lo[axis] * unit;
                    hashes[b].hi[axis] = bounds.hi[axis] * unit;
                }
            }
        }
    });

    Definitions out;
    out.definitionOfBody.resize(bodyCount);
    struct Key {
        u64 low, high;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        usize operator()(const Key& key) const { return usize(key.low); }
    };
    std::unordered_map<Key, u32, KeyHash> byHash;
    for (u32 b = 0; b < bodyCount; ++b) {
        const Key key{hashes[b].hash.low64, hashes[b].hash.high64};
        const auto [it, inserted] = byHash.try_emplace(key, u32(out.items.size()));
        if (inserted) {
            Definition definition;
            definition.body = b;
            for (int axis = 0; axis < 3; ++axis) {
                definition.boundsMin[axis] = float(hashes[b].lo[axis]);
                definition.boundsMax[axis] = float(hashes[b].hi[axis]);
            }
            definition.faceCount = structure.bodies[b].faceCount;
            out.items.push_back(definition);
        }
        out.definitionOfBody[b] = it->second;
    }
    for (const BodyOccurrence& occurrence : structure.occurrences) {
        ++out.items[out.definitionOfBody[occurrence.body]].occurrenceCount;
    }

    u32 faceTotal = 0;
    for (Definition& definition : out.items) {
        definition.firstFace = faceTotal;
        faceTotal += definition.faceCount;
    }
    out.faces.resize(faceTotal);

    // Faces are ordered by where they are, from the file alone. So their ids do not depend on the
    // tessellation, and refining a definition later keeps every id. It also keeps neighbours in one
    // chunk, which share edges and so share text.
    std::vector<std::vector<Chunk>> chunks(out.items.size());
    std::atomic<usize> nextDefinition{0};
    const usize definitionWorkers = parallelRangeCount(out.items.size(), 1);
    parallelFor(definitionWorkers, [&](usize) {
        ClosureWalker walker(index);
        std::vector<u64> keyed;
        while (true) {
            const usize d = nextDefinition.fetch_add(1, std::memory_order_relaxed);
            if (d >= out.items.size()) {
                return;
            }
            const Definition& definition = out.items[d];
            const Body& body = structure.bodies[definition.body];
            const std::span<const FaceUse> source(structure.faces.data() + body.firstFace,
                                                  body.faceCount);

            std::vector<PointBounds> faceBounds(source.size());
            PointBounds whole;
            for (usize f = 0; f < source.size(); ++f) {
                walker.begin();
                walker.add(source[f].record);
                faceBounds[f] = boundsOf(index, walker.visited(), false);
                if (!faceBounds[f].empty()) {
                    whole.add(faceBounds[f].lo);
                    whole.add(faceBounds[f].hi);
                }
            }
            keyed.resize(source.size());
            for (usize f = 0; f < source.size(); ++f) {
                const u32 code =
                    faceBounds[f].empty()
                        ? 0
                        : mortonCode((faceBounds[f].lo + faceBounds[f].hi) * 0.5, whole);
                keyed[f] = u64(code) << 32 | u32(f);
            }
            std::sort(keyed.begin(), keyed.end());
            for (usize f = 0; f < source.size(); ++f) {
                out.faces[definition.firstFace + f] = source[u32(keyed[f])];
            }

            // Greedy chunks, measured by the text they will actually contain. A body that fits in
            // one chunk is read whole instead, as the solid OCCT would see: a face cut off from its
            // shell can be read differently, as when a spherical face takes the other side of its
            // boundary.
            walker.begin();
            usize bytes = 0, seen = 0;
            u32 first = 0;
            for (u32 f = 0; f < source.size(); ++f) {
                walker.add(out.faces[definition.firstFace + f].record);
                const std::vector<u32>& visited = walker.visited();
                for (; seen < visited.size(); ++seen) {
                    bytes += index.lengths[visited[seen]];
                }
                if (bytes >= chunkBytes) {
                    chunks[d].push_back({u32(d), definition.firstFace + first, f + 1 - first});
                    first = f + 1;
                    walker.begin();
                    bytes = 0;
                    seen = 0;
                }
            }
            if (first < source.size()) {
                chunks[d].push_back(
                    {u32(d), definition.firstFace + first, u32(source.size()) - first});
            }
            if (chunks[d].size() == 1) {
                chunks[d][0].wholeBody = true;
            }
        }
    });

    for (usize d = 0; d < out.items.size(); ++d) {
        out.items[d].firstChunk = u32(out.chunks.size());
        out.items[d].chunkCount = u32(chunks[d].size());
        out.chunks.insert(out.chunks.end(), chunks[d].begin(), chunks[d].end());
    }
    return out;
}

void writeSlice(const Part21& index, std::span<const u32> faceRecords, u32 context,
                ClosureWalker& walker, std::string& text) {
    walker.begin();
    for (const u32 face : faceRecords) {
        walker.add(face);
    }
    walker.add(context);

    usize bytes = index.header.size() + 96 + faceRecords.size() * 12;
    for (const u32 record : walker.visited()) {
        bytes += index.lengths[record] + 1;
    }
    text.clear();
    text.reserve(bytes + 256);
    text.append(index.header);

    const u64 shell = u64(index.maxId) + 1;
    std::format_to(std::back_inserter(text), "#{}=OPEN_SHELL('',(", shell);
    for (usize f = 0; f < faceRecords.size(); ++f) {
        std::format_to(std::back_inserter(text), "{}#{}", f == 0 ? "" : ",",
                       index.ids[faceRecords[f]]);
    }
    std::format_to(std::back_inserter(text),
                   "));\n#{}=SHELL_BASED_SURFACE_MODEL('',(#{}));\n"
                   "#{}=MANIFOLD_SURFACE_SHAPE_REPRESENTATION('',(#{}),#{});\n",
                   shell + 1, shell, shell + 2, shell + 1,
                   context == kNoRecord ? 0 : index.ids[context]);
    for (const u32 record : walker.visited()) {
        text.append(index.text(record));
        text.push_back('\n');
    }
    text.append("ENDSEC;\nEND-ISO-10303-21;\n");
}

void writeBodySlice(const Part21& index, u32 body, u32 context, ClosureWalker& walker,
                    std::string& text) {
    walker.begin();
    walker.add(body);
    walker.add(context);

    usize bytes = index.header.size() + 96;
    for (const u32 record : walker.visited()) {
        bytes += index.lengths[record] + 1;
    }
    text.clear();
    text.reserve(bytes + 256);
    text.append(index.header);
    std::format_to(std::back_inserter(text), "#{}=SHAPE_REPRESENTATION('',(#{}),#{});\n",
                   u64(index.maxId) + 1, index.ids[body],
                   context == kNoRecord ? 0 : index.ids[context]);
    for (const u32 record : walker.visited()) {
        text.append(index.text(record));
        text.push_back('\n');
    }
    text.append("ENDSEC;\nEND-ISO-10303-21;\n");
}

} // namespace geo::import::step
