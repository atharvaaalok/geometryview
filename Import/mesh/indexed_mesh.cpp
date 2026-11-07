#include "mesh/indexed_mesh.h"

#include "base/parallel_sort.h"
#include "base/trace.h"
#include "mesh/dedupe.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <simd/simd.h>

namespace geo::import {
namespace {

constexpr u32 kNone = 0xFFFFFFFF;
constexpr usize kGrain = 1 << 15;

struct Welded {
    std::vector<float> positions;
    std::vector<u32> indices;
};

simd_float3 readPosition(const std::vector<float>& positions, u32 vertex) {
    const float* p = positions.data() + usize(vertex) * 3;
    return simd_make_float3(p[0], p[1], p[2]);
}

// Merges corners with identical coordinates. Adding zero turns -0 into +0, so the two compare equal
// by bit pattern, which is what the hash sees.
Welded weldCorners(const std::vector<float>& corners) {
    const usize cornerCount = corners.size() / 3;
    const auto bitsOf = [&](usize corner, usize axis) {
        return std::bit_cast<u32>(corners[corner * 3 + axis] + 0.0f);
    };
    std::vector<u32> ids, firstCorner;
    const u32 unique = dedupe(
        cornerCount,
        [&](usize c) {
            return mixHash(u64(bitsOf(c, 0)) | u64(bitsOf(c, 1)) << 32) ^
                   mixHash(u64(bitsOf(c, 2)) + 0x9E3779B97F4A7C15ull);
        },
        [&](u32 a, u32 b) {
            return bitsOf(a, 0) == bitsOf(b, 0) && bitsOf(a, 1) == bitsOf(b, 1) &&
                   bitsOf(a, 2) == bitsOf(b, 2);
        },
        ids, firstCorner);

    Welded welded;
    welded.positions.resize(usize(unique) * 3);
    parallelRanges(unique, kGrain, [&](usize first, usize last, usize) {
        for (usize v = first; v < last; ++v) {
            const float* source = corners.data() + usize(firstCorner[v]) * 3;
            std::copy_n(source, 3, welded.positions.data() + v * 3);
        }
    });

    // Triangles that collapse to a line or point can never contribute a surface.
    welded.indices.reserve(ids.size());
    for (usize t = 0; t + 2 < ids.size(); t += 3) {
        const u32 a = ids[t], b = ids[t + 1], c = ids[t + 2];
        if (a != b && b != c && a != c) {
            welded.indices.insert(welded.indices.end(), {a, b, c});
        }
    }
    return welded;
}

// Lock-free union-find. Each root is the lowest index in its set, so the sets do not depend on the
// order of operations or the number of threads.
class DisjointSets {
public:
    explicit DisjointSets(usize count) : parent_(count) {
        parallelRanges(count, kGrain, [&](usize first, usize last, usize) {
            for (usize i = first; i < last; ++i) {
                parent_[i].store(u32(i), std::memory_order_relaxed);
            }
        });
    }

    u32 find(u32 item) {
        u32 parent = parent_[item].load(std::memory_order_relaxed);
        while (parent != item) {
            const u32 grandparent = parent_[parent].load(std::memory_order_relaxed);
            // Path halving. Losing this race only means a later find does the work.
            parent_[item].compare_exchange_weak(parent, grandparent, std::memory_order_relaxed);
            item = grandparent;
            parent = parent_[item].load(std::memory_order_relaxed);
        }
        return item;
    }

    void unite(u32 a, u32 b) {
        while (true) {
            a = find(a);
            b = find(b);
            if (a == b) {
                return;
            }
            u32 high = std::max(a, b);
            const u32 low = std::min(a, b);
            // Links only a current root, so a concurrent link can never be lost.
            if (parent_[high].compare_exchange_weak(high, low, std::memory_order_relaxed)) {
                return;
            }
        }
    }

private:
    std::vector<std::atomic<u32>> parent_;
};

// Replaces each element's root with a dense id in order of first appearance.
u32 denseLabels(DisjointSets& sets, usize count, std::vector<u32>& labels) {
    labels.resize(count);
    std::vector<u32> labelOfRoot(count, kNone);
    u32 next = 0;
    for (usize i = 0; i < count; ++i) {
        const u32 root = sets.find(u32(i));
        if (labelOfRoot[root] == kNone) {
            labelOfRoot[root] = next++;
        }
        labels[i] = labelOfRoot[root];
    }
    return next;
}

struct EdgeTable {
    std::vector<u32> edgeOfCorner;  // edge id per triangle corner
    std::vector<u32> firstCorner;   // one corner per edge, the same on every run
    u32 edgeCount = 0;
};

EdgeTable matchEdges(const std::vector<u32>& indices) {
    const usize cornerCount = indices.size();
    const auto keyOf = [&](usize corner) {
        const usize t = corner / 3;
        const u32 a = indices[corner];
        const u32 b = indices[t * 3 + (corner + 1) % 3];
        return u64(std::min(a, b)) | u64(std::max(a, b)) << 32;
    };
    EdgeTable table;
    table.edgeCount = dedupe(
        cornerCount, [&](usize c) { return mixHash(keyOf(c)); },
        [&](u32 a, u32 b) { return keyOf(a) == keyOf(b); }, table.edgeOfCorner, table.firstCorner);
    return table;
}

struct Regions {
    std::vector<u32> groupOfTriangle;
    std::vector<u32> faceOfGroup;
    u32 groupCount = 0;
    u32 faceCount = 0;
};

// One entry per corner that shares an edge with a triangle in a different group, for small groups.
// Sorted, so a group's entries are adjacent and equal pairs count the length of a shared border.
std::vector<u64> collectBorders(const EdgeTable& edges, const Regions& regions,
                                const std::vector<u32>& size, u32 minTriangles) {
    const usize cornerCount = edges.edgeOfCorner.size();
    const usize ranges = parallelRangeCount(cornerCount, kGrain);
    std::vector<std::vector<u64>> parts(ranges);
    parallelRanges(cornerCount, kGrain, [&](usize first, usize last, usize range) {
        std::vector<u64>& part = parts[range];
        for (usize corner = first; corner < last; ++corner) {
            const u32 other = edges.firstCorner[edges.edgeOfCorner[corner]];
            if (other == corner) {
                continue;
            }
            const u32 a = regions.groupOfTriangle[corner / 3];
            const u32 b = regions.groupOfTriangle[other / 3];
            if (a == b) {
                continue;
            }
            if (size[a] < minTriangles) {
                part.push_back(u64(a) << 32 | b);
            }
            if (size[b] < minTriangles) {
                part.push_back(u64(b) << 32 | a);
            }
        }
    });

    usize total = 0;
    for (const std::vector<u64>& part : parts) {
        total += part.size();
    }
    std::vector<u64> borders;
    borders.reserve(total);
    for (const std::vector<u64>& part : parts) {
        borders.insert(borders.end(), part.begin(), part.end());
    }
    parallelSort(borders, [](u64 border) { return border; });
    return borders;
}

// Merges groups below `minTriangles` into neighbours, preferring the longest shared border.
//
// A size cap on the merged region is what keeps faces from chaining together: without it, every small
// region's best neighbour soon becomes the one large region, and a whole surface collapses into a
// single face. The cap starts near `minTriangles`, so regions end up similar in size, and it doubles
// each pass, so a region left stranded between large neighbours is still absorbed. The last pass has
// no cap, which guarantees the bound on the number of faces.
void mergeSmallGroups(const EdgeTable& edges, Regions& regions, u32 minTriangles) {
    constexpr int kMaxPasses = 6;
    DisjointSets faces(regions.groupCount);

    std::vector<u32> size(regions.groupCount, 0);
    for (u32 group : regions.groupOfTriangle) {
        ++size[group];
    }
    const std::vector<u64> borders = collectBorders(edges, regions, size, minTriangles);

    for (int pass = 0; pass < kMaxPasses; ++pass) {
        const u64 cap = pass + 1 == kMaxPasses ? ~u64(0) : u64(minTriangles) << (pass + 1);
        bool anySmall = false;
        usize i = 0;
        while (i < borders.size()) {
            const u32 group = u32(borders[i] >> 32);
            const u32 root = faces.find(group);
            u32 best = kNone;
            usize bestLength = 0;
            while (i < borders.size() && u32(borders[i] >> 32) == group) {
                const u64 pair = borders[i];
                usize length = 0;
                while (i < borders.size() && borders[i] == pair) {
                    ++length;
                    ++i;
                }
                const u32 neighbour = faces.find(u32(pair));
                if (neighbour != root && length > bestLength &&
                    u64(size[root]) + size[neighbour] <= cap) {
                    bestLength = length;
                    best = neighbour;
                }
            }
            if (size[root] >= minTriangles) {
                continue;
            }
            anySmall = true;
            if (best != kNone) {
                const u32 combined = size[root] + size[best];
                faces.unite(root, best);
                size[faces.find(root)] = combined;
            }
        }
        if (!anySmall) {
            break;
        }
    }
    regions.faceCount = denseLabels(faces, regions.groupCount, regions.faceOfGroup);
}

Regions growRegions(const Welded& mesh, const EdgeTable& edges, const RegionOptions& options) {
    const usize triangleCount = mesh.indices.size() / 3;

    std::vector<simd_float3> normals(triangleCount);
    parallelRanges(triangleCount, kGrain, [&](usize first, usize last, usize) {
        for (usize t = first; t < last; ++t) {
            const simd_float3 a = readPosition(mesh.positions, mesh.indices[t * 3]);
            const simd_float3 b = readPosition(mesh.positions, mesh.indices[t * 3 + 1]);
            const simd_float3 c = readPosition(mesh.positions, mesh.indices[t * 3 + 2]);
            const simd_float3 n = simd_cross(b - a, c - a);
            const float length = simd_length(n);
            normals[t] = length > 0.0f ? n / length : simd_make_float3(0.0f, 0.0f, 0.0f);
        }
    });

    const float cosCrease = std::cos(options.creaseDegrees * std::numbers::pi_v<float> / 180.0f);
    StageTrace trace("grow");
    DisjointSets groups(triangleCount);
    parallelRanges(mesh.indices.size(), kGrain, [&](usize first, usize last, usize) {
        for (usize corner = first; corner < last; ++corner) {
            const u32 firstCorner = edges.firstCorner[edges.edgeOfCorner[corner]];
            if (firstCorner == corner) {
                continue;
            }
            const u32 a = u32(corner / 3), b = firstCorner / 3;
            // A zero-area triangle has no normal, and it joins whatever it touches.
            const bool degenerate = simd_length_squared(normals[a]) == 0.0f ||
                                    simd_length_squared(normals[b]) == 0.0f;
            if (degenerate || simd_dot(normals[a], normals[b]) >= cosCrease) {
                groups.unite(a, b);
            }
        }
    });
    trace.lap("union by crease");
    Regions regions;
    regions.groupCount = denseLabels(groups, triangleCount, regions.groupOfTriangle);
    trace.lap("dense labels");
    const u32 minTriangles = std::max<u32>(1, u32(triangleCount / options.maxLargeFaces));
    mergeSmallGroups(edges, regions, minTriangles);
    trace.lap("merge small groups");
    return regions;
}

float cornerAngle(simd_float3 corner, simd_float3 toNext, simd_float3 toPrevious) {
    const simd_float3 u = simd_normalize(toNext - corner);
    const simd_float3 v = simd_normalize(toPrevious - corner);
    return std::acos(std::clamp(simd_dot(u, v), -1.0f, 1.0f));
}

// Sums each triangle's unit normal into its vertices, weighted by the corner's angle. Weighting by
// angle makes the result independent of how a surface was split into triangles: two 45 degree corners
// count the same as one 90 degree corner, whereas area weighting favours the side split in two and
// shows up as faint bands on smooth surfaces. Normals here are left unnormalised.
void accumulateAngleWeightedNormals(IndexedMesh& mesh) {
    struct Contribution {
        simd_float3 normal;
        std::array<float, 3> angles;
    };
    const usize triangleCount = mesh.triangleCount();
    std::vector<Contribution> contributions(triangleCount);
    parallelRanges(triangleCount, kGrain, [&](usize first, usize last, usize) {
        for (usize t = first; t < last; ++t) {
            const simd_float3 a = readPosition(mesh.positions, mesh.indices[t * 3]);
            const simd_float3 b = readPosition(mesh.positions, mesh.indices[t * 3 + 1]);
            const simd_float3 c = readPosition(mesh.positions, mesh.indices[t * 3 + 2]);
            const simd_float3 n = simd_cross(b - a, c - a);
            const float length = simd_length(n);
            if (length > 0.0f) {
                contributions[t] = {n / length,
                                    {cornerAngle(a, b, c), cornerAngle(b, c, a),
                                     cornerAngle(c, a, b)}};
            } else {
                contributions[t] = {};
            }
        }
    });
    for (usize t = 0; t < triangleCount; ++t) {
        const Contribution& contribution = contributions[t];
        for (usize corner = 0; corner < 3; ++corner) {
            float* out = mesh.normals.data() + usize(mesh.indices[t * 3 + corner]) * 3;
            const simd_float3 weighted = contribution.normal * contribution.angles[corner];
            out[0] += weighted.x;
            out[1] += weighted.y;
            out[2] += weighted.z;
        }
    }
}

} // namespace

IndexedMesh buildIndexedMesh(const TriangleSoup& soup, const RegionOptions& options) {
    StageTrace trace("regions");
    const Welded welded = weldCorners(soup.corners);
    trace.lap("weld");
    const EdgeTable edges = matchEdges(welded.indices);
    trace.lap("match edges");
    const Regions regions = growRegions(welded, edges, options);
    trace.lap("grow regions");

    if (traceEnabled()) {
        std::vector<u32> size(regions.groupCount, 0);
        for (u32 group : regions.groupOfTriangle) {
            ++size[group];
        }
        const u32 largest = *std::max_element(size.begin(), size.end());
        std::fprintf(stderr, "trace: regions    %u smoothing groups (largest %.1f%%), %u faces\n",
                     regions.groupCount,
                     100.0 * double(largest) / double(regions.groupOfTriangle.size()),
                     regions.faceCount);
    }

    // A final vertex is a welded position within one smoothing group.
    const usize cornerCount = welded.indices.size();
    const auto keyOf = [&](usize corner) {
        return u64(welded.indices[corner]) | u64(regions.groupOfTriangle[corner / 3]) << 32;
    };
    std::vector<u32> finalIds, firstCorner;
    const u32 vertexCount = dedupe(
        cornerCount, [&](usize c) { return mixHash(keyOf(c)); },
        [&](u32 a, u32 b) { return keyOf(a) == keyOf(b); }, finalIds, firstCorner);

    IndexedMesh mesh;
    mesh.faceCount = regions.faceCount;
    mesh.indices = std::move(finalIds);
    mesh.positions.resize(usize(vertexCount) * 3);
    mesh.normals.assign(usize(vertexCount) * 3, 0.0f);
    mesh.faceOfVertex.resize(vertexCount);
    parallelRanges(vertexCount, kGrain, [&](usize first, usize last, usize) {
        for (usize v = first; v < last; ++v) {
            const u32 corner = firstCorner[v];
            const float* source = welded.positions.data() + usize(welded.indices[corner]) * 3;
            std::copy_n(source, 3, mesh.positions.data() + v * 3);
            mesh.faceOfVertex[v] = regions.faceOfGroup[regions.groupOfTriangle[corner / 3]];
        }
    });

    trace.lap("split vertices");
    accumulateAngleWeightedNormals(mesh);
    parallelRanges(vertexCount, kGrain, [&](usize first, usize last, usize) {
        for (usize v = first; v < last; ++v) {
            float* n = mesh.normals.data() + v * 3;
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length > 0.0f) {
                n[0] /= length;
                n[1] /= length;
                n[2] /= length;
            }
        }
    });
    trace.lap("normals");
    return mesh;
}

} // namespace geo::import
