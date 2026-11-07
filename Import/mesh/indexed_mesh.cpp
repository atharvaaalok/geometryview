#include "mesh/indexed_mesh.h"

#include "mesh/dedupe.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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

// Union-find with path halving. Each root is the lowest index in its set, so results are stable.
class DisjointSets {
public:
    explicit DisjointSets(usize count) : parent_(count) {
        for (usize i = 0; i < count; ++i) {
            parent_[i] = u32(i);
        }
    }

    u32 find(u32 item) {
        while (parent_[item] != item) {
            parent_[item] = parent_[parent_[item]];
            item = parent_[item];
        }
        return item;
    }

    void unite(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent_[std::max(a, b)] = std::min(a, b);
        }
    }

private:
    std::vector<u32> parent_;
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
    std::vector<u32> edgeOfCorner; // edge id per triangle corner
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
    std::vector<u32> firstCorner;
    table.edgeCount = dedupe(
        cornerCount, [&](usize c) { return mixHash(keyOf(c)); },
        [&](u32 a, u32 b) { return keyOf(a) == keyOf(b); }, table.edgeOfCorner, firstCorner);
    return table;
}

struct Regions {
    std::vector<u32> groupOfTriangle;
    std::vector<u32> faceOfGroup;
    u32 groupCount = 0;
    u32 faceCount = 0;
};

// Merges groups below `minTriangles` into the neighbour they share the longest border with.
void mergeSmallGroups(const std::vector<u32>& edgeOfCorner, u32 edgeCount, Regions& regions,
                      u32 minTriangles) {
    constexpr int kMaxPasses = 4;
    const usize triangleCount = regions.groupOfTriangle.size();
    DisjointSets faces(regions.groupCount);

    std::vector<u32> size(regions.groupCount, 0);
    for (u32 group : regions.groupOfTriangle) {
        ++size[group];
    }

    for (int pass = 0; pass < kMaxPasses; ++pass) {
        std::vector<u64> borders;
        std::vector<u32> lastFace(edgeCount, kNone);
        for (usize t = 0; t < triangleCount; ++t) {
            const u32 face = faces.find(regions.groupOfTriangle[t]);
            for (usize corner = 0; corner < 3; ++corner) {
                const u32 edge = edgeOfCorner[t * 3 + corner];
                const u32 other = lastFace[edge];
                lastFace[edge] = face;
                if (other != kNone && other != face) {
                    if (size[face] < minTriangles) {
                        borders.push_back(u64(face) << 32 | other);
                    }
                    if (size[other] < minTriangles) {
                        borders.push_back(u64(other) << 32 | face);
                    }
                }
            }
        }
        if (borders.empty()) {
            break;
        }
        std::sort(borders.begin(), borders.end());

        // Runs of equal pairs count shared edges, so each small region picks its longest border.
        usize i = 0;
        bool merged = false;
        while (i < borders.size()) {
            const u32 small = u32(borders[i] >> 32);
            u32 best = kNone;
            usize bestLength = 0;
            while (i < borders.size() && u32(borders[i] >> 32) == small) {
                const u64 pair = borders[i];
                usize length = 0;
                while (i < borders.size() && borders[i] == pair) {
                    ++length;
                    ++i;
                }
                if (length > bestLength) {
                    bestLength = length;
                    best = u32(pair);
                }
            }
            const u32 a = faces.find(small);
            const u32 b = faces.find(best);
            if (a != b) {
                const u32 combined = size[a] + size[b];
                faces.unite(a, b);
                size[faces.find(a)] = combined;
                merged = true;
            }
        }
        if (!merged) {
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
    DisjointSets groups(triangleCount);
    std::vector<u32> lastTriangle(edges.edgeCount, kNone);
    for (usize t = 0; t < triangleCount; ++t) {
        for (usize corner = 0; corner < 3; ++corner) {
            const u32 edge = edges.edgeOfCorner[t * 3 + corner];
            const u32 other = lastTriangle[edge];
            lastTriangle[edge] = u32(t);
            if (other == kNone) {
                continue;
            }
            // A zero-area triangle has no normal, and it joins whatever it touches.
            const bool degenerate = simd_length_squared(normals[t]) == 0.0f ||
                                    simd_length_squared(normals[other]) == 0.0f;
            if (degenerate || simd_dot(normals[t], normals[other]) >= cosCrease) {
                groups.unite(u32(t), other);
            }
        }
    }

    Regions regions;
    regions.groupCount = denseLabels(groups, triangleCount, regions.groupOfTriangle);
    const u32 minTriangles = std::max<u32>(1, u32(triangleCount / options.maxLargeFaces));
    mergeSmallGroups(edges.edgeOfCorner, edges.edgeCount, regions, minTriangles);
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
    const Welded welded = weldCorners(soup.corners);
    const EdgeTable edges = matchEdges(welded.indices);
    const Regions regions = growRegions(welded, edges, options);

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
    return mesh;
}

} // namespace geo::import
