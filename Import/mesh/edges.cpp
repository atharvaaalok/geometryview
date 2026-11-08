#include "mesh/edges.h"

#include "base/parallel.h"
#include "base/parallel_sort.h"
#include "base/trace.h"

#include "base/simd.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace geo::import {
namespace {

// Faces whose normals along a shared edge differ by more than this meet at a crease. Smooth CAD
// edges agree to well under a degree, and meshes split smoothing groups at far larger angles.
constexpr float kCreaseCosine = 0.9659258f; // cos 15 degrees
constexpr u32 kNone = 0xFFFFFFFF;

// A triangle side that no other triangle shares by vertex index. Interior sides of a face always
// share their vertices, so only these can lie on a feature edge.
struct OpenSide {
    u32 a, b; // mesh vertices, in the triangle's order
};

struct Segment {
    u32 a, b;   // mesh vertices on side A, ordered along the chain once chained
    u32 pa, pb; // welded points at a and b
    u32 faceA, faceB;
    u32 edgeClass;
};

simd_float3 meshVector(const std::vector<float>& values, u32 index) {
    const float* p = values.data() + usize(index) * 3;
    return simd_make_float3(p[0], p[1], p[2]);
}

std::array<u32, 3> gridOf(const IndexedMesh& mesh, const EdgeLayout& layout, u32 vertex) {
    return layout.grid.of(meshVector(mesh.positions, vertex));
}

// Triangle sides with no partner by vertex index, gathered in parallel.
//
// Another triangle shares side a-b when it holds both a and b. Any two corners of a triangle are
// next to each other, so that is when, at a or at b, one of its corners is followed by the other
// end. Each vertex therefore lists the vertex that follows it in each of its corners, and a side
// is looked up in its two ends' lists, read in order, without visiting the triangles themselves.
std::vector<OpenSide> findOpenSides(const IndexedMesh& mesh) {
    const usize vertexCount = mesh.vertexCount();
    const usize triangleCount = mesh.triangleCount();
    const std::vector<u32>& indices = mesh.indices;

    // The vertex after each corner, listed by the corner's vertex.
    std::vector<u32> start(vertexCount + 1, 0);
    for (const u32 index : indices) {
        ++start[index + 1];
    }
    std::partial_sum(start.begin(), start.end(), start.begin());
    std::vector<u32> following(indices.size());
    std::vector<u32> fill(start.begin(), start.end() - 1);
    for (usize t = 0; t < triangleCount; ++t) {
        for (usize k = 0; k < 3; ++k) {
            following[fill[indices[t * 3 + k]]++] = indices[t * 3 + (k + 1) % 3];
        }
    }

    // Corners at `from` followed by `to`, across the mesh.
    const auto count = [&](u32 from, u32 to) {
        u32 found = 0;
        for (u32 i = start[from]; i < start[from + 1]; ++i) {
            found += following[i] == to ? 1 : 0;
        }
        return found;
    };
    std::vector<std::vector<OpenSide>> perRange(parallelRangeCount(triangleCount, 1 << 14));
    parallelRanges(triangleCount, 1 << 14, [&](usize first, usize last, usize range) {
        std::vector<OpenSide>& out = perRange[range];
        for (usize t = first; t < last; ++t) {
            const u32* v = indices.data() + t * 3;
            for (usize k = 0; k < 3; ++k) {
                const u32 a = v[k], b = v[(k + 1) % 3];
                // What this triangle itself adds to the counts, to leave only the others.
                u32 own = 0, mine = 0;
                for (usize j = 0; j < 3; ++j) {
                    const u32 p = v[j], q = v[(j + 1) % 3];
                    own += (p == a && q == b) + (p == b && q == a);
                    mine += p == a;
                }
                // A side from a vertex to itself, in a collapsed triangle, is shared by any
                // other triangle at that vertex.
                const bool shared = a == b ? start[a + 1] - start[a] > mine
                                           : count(b, a) + count(a, b) > own;
                if (!shared) {
                    out.push_back({a, b});
                }
            }
        }
    });
    std::vector<OpenSide> sides;
    for (const std::vector<OpenSide>& part : perRange) {
        sides.insert(sides.end(), part.begin(), part.end());
    }
    return sides;
}

// A 16-bit grid position fits a key exactly. A 24-bit one is hashed, so two points would weld only
// if their 64-bit hashes collided.
u64 pointKey(const std::array<u32, 3>& grid, bool wide) {
    if (!wide) {
        return u64(grid[0]) | u64(grid[1]) << 16 | u64(grid[2]) << 32;
    }
    u64 h = u64(grid[0]) | u64(grid[1]) << 24 | u64(grid[2] & 0xFFFF) << 48;
    h ^= u64(grid[2] >> 16) * 0x9E3779B97F4A7C15ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 29;
    return h;
}

struct Keyed {
    u64 key;
    u32 index;
};

// Pairs open sides that lie on the same grid points, and classifies each resulting edge.
std::vector<Segment> matchSides(const IndexedMesh& mesh, const EdgeLayout& layout,
                                const std::vector<OpenSide>& sides) {
    // Weld the sides' ends by grid position. An end's index is its side times two, plus one for b.
    const bool wide = layout.grid.wide;
    std::vector<Keyed> ends(sides.size() * 2);
    parallelRanges(sides.size(), 1 << 14, [&](usize first, usize last, usize) {
        for (usize s = first; s < last; ++s) {
            for (u32 e = 0; e < 2; ++e) {
                const u32 vertex = e == 0 ? sides[s].a : sides[s].b;
                ends[s * 2 + e] = {pointKey(gridOf(mesh, layout, vertex), wide), u32(s * 2 + e)};
            }
        }
    });
    parallelSort(
        ends, [](const Keyed& k) { return k.key; }, [](const Keyed& k) { return k.index; });
    std::vector<u32> pointOf(ends.size());
    u32 points = 0;
    for (usize i = 0; i < ends.size(); ++i) {
        if (i > 0 && ends[i].key != ends[i - 1].key) {
            ++points;
        }
        pointOf[ends[i].index] = points;
    }

    // Group sides by the unordered pair of points they join.
    std::vector<Keyed> pairs(sides.size());
    for (usize s = 0; s < sides.size(); ++s) {
        const u32 p = pointOf[s * 2], q = pointOf[s * 2 + 1];
        pairs[s] = {u64(std::min(p, q)) << 32 | std::max(p, q), u32(s)};
    }
    parallelSort(
        pairs, [](const Keyed& k) { return k.key; }, [](const Keyed& k) { return k.index; });

    const auto normal = [&](u32 vertex) { return meshVector(mesh.normals, vertex); };
    std::vector<Segment> segments;
    for (usize i = 0; i < pairs.size();) {
        usize j = i + 1;
        while (j < pairs.size() && pairs[j].key == pairs[i].key) {
            ++j;
        }
        const u32 firstSide = pairs[i].index;
        const OpenSide& first = sides[firstSide];
        const u32 pa = pointOf[firstSide * 2], pb = pointOf[firstSide * 2 + 1];
        if (pa == pb) { // collapsed onto one grid point: too short to draw
            i = j;
            continue;
        }
        Segment segment{first.a, first.b, pa, pb, mesh.faceOfVertex[first.a], kNone, kEdgeBoundary};
        if (j - i >= 2) {
            // The other side, matched end to end. Further sides of a non-manifold edge are
            // dropped, because one line already shows it.
            const OpenSide& second = sides[pairs[i + 1].index];
            const bool sameWay = pointOf[pairs[i + 1].index * 2] == pa;
            const u32 otherA = sameWay ? second.a : second.b;
            const u32 otherB = sameWay ? second.b : second.a;
            const bool crease = simd_dot(normal(first.a), normal(otherA)) < kCreaseCosine ||
                                simd_dot(normal(first.b), normal(otherB)) < kCreaseCosine;
            segment.faceB = mesh.faceOfVertex[second.a];
            if (segment.faceA == segment.faceB && !crease) {
                i = j; // a seam
                continue;
            }
            segment.edgeClass = crease ? kEdgeCrease : kEdgeSmooth;
        }
        segments.push_back(segment);
        i = j;
    }
    return segments;
}

// Orders segments into chains of one kind of edge between the same faces, oriented end to end.
std::vector<std::vector<u32>> chainSegments(std::vector<Segment>& segments, u32 pointCount) {
    std::vector<u32> start(pointCount + 1, 0);
    for (const Segment& s : segments) {
        ++start[s.pa + 1];
        ++start[s.pb + 1];
    }
    std::partial_sum(start.begin(), start.end(), start.begin());
    std::vector<u32> incident(segments.size() * 2);
    std::vector<u32> fill(start.begin(), start.end() - 1);
    for (u32 s = 0; s < segments.size(); ++s) {
        incident[fill[segments[s].pa]++] = s;
        incident[fill[segments[s].pb]++] = s;
    }

    const auto sameKind = [&](const Segment& x, const Segment& y) {
        const bool sameFaces = (x.faceA == y.faceA && x.faceB == y.faceB) ||
                               (x.faceA == y.faceB && x.faceB == y.faceA);
        return sameFaces && x.edgeClass == y.edgeClass;
    };
    // The segment continuing a chain through `point`, if exactly one does.
    const auto next = [&](u32 point, u32 from) -> u32 {
        if (start[point + 1] - start[point] != 2) {
            return kNone;
        }
        const u32 other =
            incident[start[point]] == from ? incident[start[point] + 1] : incident[start[point]];
        return other != from && sameKind(segments[other], segments[from]) ? other : kNone;
    };
    const auto orientFrom = [&](u32 s, u32 point) {
        Segment& segment = segments[s];
        if (segment.pa != point) {
            std::swap(segment.a, segment.b);
            std::swap(segment.pa, segment.pb);
        }
    };

    std::vector<u8> used(segments.size(), 0);
    std::vector<std::vector<u32>> chains;
    std::vector<u32> backward;
    for (u32 seed = 0; seed < segments.size(); ++seed) {
        if (used[seed]) {
            continue;
        }
        used[seed] = 1;
        std::vector<u32> chain{seed};
        for (u32 at = segments[seed].pb, from = seed;;) {
            const u32 s = next(at, from);
            if (s == kNone || used[s]) {
                break;
            }
            used[s] = 1;
            orientFrom(s, at);
            chain.push_back(s);
            at = segments[s].pb;
            from = s;
        }
        backward.clear();
        for (u32 at = segments[seed].pa, from = seed;;) {
            const u32 s = next(at, from);
            if (s == kNone || used[s]) {
                break;
            }
            used[s] = 1;
            // Runs toward the seed, so it must end where the chain so far begins.
            if (segments[s].pb != at) {
                std::swap(segments[s].a, segments[s].b);
                std::swap(segments[s].pa, segments[s].pb);
            }
            backward.push_back(s);
            at = segments[s].pa;
            from = s;
        }
        if (!backward.empty()) {
            std::reverse(backward.begin(), backward.end());
            chain.insert(chain.begin(), backward.begin(), backward.end());
        }
        chains.push_back(std::move(chain));
    }
    return chains;
}

// Interleaves the low 10 bits of x, y and z.
u32 morton3(u32 x, u32 y, u32 z) {
    const auto spread = [](u32 v) {
        v &= 0x3FF;
        v = (v | (v << 16)) & 0x030000FF;
        v = (v | (v << 8)) & 0x0300F00F;
        v = (v | (v << 4)) & 0x030C30C3;
        v = (v | (v << 2)) & 0x09249249;
        return v;
    };
    return spread(x) | spread(y) << 1 | spread(z) << 2;
}

} // namespace

FeatureEdges findFeatureEdges(const IndexedMesh& mesh, const EdgeLayout& layout) {
    FeatureEdges edges;
    if (mesh.triangleCount() == 0) {
        return edges;
    }
    StageTrace trace("edges");
    const std::vector<OpenSide> sides = findOpenSides(mesh);
    trace.lap("open sides");
    std::vector<Segment> segments = matchSides(mesh, layout, sides);
    u32 pointCount = 0;
    for (const Segment& s : segments) {
        pointCount = std::max({pointCount, s.pa + 1, s.pb + 1});
    }
    std::vector<std::vector<u32>> chains = chainSegments(segments, pointCount);
    trace.lap("match and chain");

    const u32 shift = layout.grid.wide ? 14 : 6; // to the top 10 bits of the grid
    const auto positionOf = [&](u32 vertex) {
        return layout.grid.decode(gridOf(mesh, layout, vertex));
    };
    std::vector<u64> keyed(chains.size());
    for (usize c = 0; c < chains.size(); ++c) {
        const std::array<u32, 3> g = gridOf(mesh, layout, segments[chains[c][0]].a);
        keyed[c] = u64(morton3(g[0] >> shift, g[1] >> shift, g[2] >> shift)) << 32 | c;
    }
    std::sort(keyed.begin(), keyed.end());

    edges.segments.reserve(segments.size());
    for (const u64 key : keyed) {
        float along = 0.0f;
        for (const u32 s : chains[u32(key)]) {
            const Segment& segment = segments[s];
            const u32 faceB =
                (segment.faceB == kNone ? u32(kEdgeNoFace) : layout.packedOfFace[segment.faceB]) |
                segment.edgeClass << kEdgeClassShift;
            edges.segments.push_back(
                {segment.a, segment.b, layout.packedOfFace[segment.faceA], faceB, along});
            along += simd_distance(positionOf(segment.a), positionOf(segment.b));
        }
    }

    // Chunks of consecutive segments, with conservative bounds on the 16-bit bounds grid.
    const float boundsStep = simd_reduce_max(layout.boundsScale);
    const u32 segmentCount = u32(edges.segments.size());
    edges.chunks.resize((segmentCount + kEdgeChunkSegments - 1) / kEdgeChunkSegments);
    parallelRanges(edges.chunks.size(), 1 << 10, [&](usize firstChunk, usize lastChunk, usize) {
        for (usize c = firstChunk; c < lastChunk; ++c) {
            const u32 first = u32(c) * kEdgeChunkSegments;
            const u32 count = std::min<u32>(kEdgeChunkSegments, segmentCount - first);
            simd_float3 low = simd_make_float3(INFINITY, INFINITY, INFINITY);
            simd_float3 high = -low;
            for (u32 s = first; s < first + count; ++s) {
                for (const u32 v : {edges.segments[s].a, edges.segments[s].b}) {
                    const simd_float3 p = positionOf(v);
                    low = simd_min(low, p);
                    high = simd_max(high, p);
                }
            }
            const simd_float3 center = (low + high) * 0.5f;
            const float radius = simd_distance(high, center);
            const simd_float3 grid =
                simd_clamp(simd::floor((center - layout.grid.lo) / layout.boundsScale + 0.5f),
                           simd_float3(0.0f), simd_float3(65535.0f));
            // The rounded centre can move by half a step on each axis, which the radius absorbs.
            const float gridRadius = std::ceil(radius / boundsStep) + 1.0f;
            edges.chunks[c] = {u16(grid.x), u16(grid.y),
                               u16(grid.z), u16(std::min(gridRadius, 65535.0f)),
                               first,       count};
        }
    });
    trace.lap("chunks");
    return edges;
}

void usePackedVertices(FeatureEdges& edges, std::span<const u32> packedVertex) {
    for (EdgeSegment& segment : edges.segments) {
        segment.a = packedVertex[segment.a];
        segment.b = packedVertex[segment.b];
    }
}

} // namespace geo::import
