#include "mesh/packer.h"

#include "base/assert.h"
#include "base/parallel.h"
#include "base/parallel_sort.h"
#include "base/trace.h"
#include "mesh/edges.h"
#include "mesh/grid.h"

#include <meshoptimizer.h>

#include "base/simd.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numeric>
#include <span>

namespace geo::import {
namespace {

constexpr float kConeWeight = 0.25f;
constexpr u32 kGrid16 = 65535;
constexpr u32 kGrid24 = (1u << 24) - 1;
constexpr usize kMaxUnitTriangles = 1 << 18;
constexpr usize kMaxUnitFaces = 1 << 16;
// Short edges below this many 16-bit grid steps would visibly lose shape, so the definition gets
// the wide grid. See ADR 0004.
constexpr float kMinEdgeGridSteps = 8.0f;
constexpr usize kEdgeSampleStride = 97;
// Meshes at least this large find their feature edges while meshlets are built. Smaller ones are
// quicker done in turn.
constexpr usize kConcurrentEdgeTriangles = 1 << 16;

simd_float3 readVector(const std::vector<float>& values, usize index) {
    const float* p = values.data() + index * 3;
    return simd_make_float3(p[0], p[1], p[2]);
}

simd_float3 roundToGrid(simd_float3 value) {
    return simd::floor(value + 0.5f);
}

i16 snorm16(float value) {
    return i16(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

void encodeOctahedral(simd_float3 n, i16& x, i16& y) {
    const float sum = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    if (sum == 0.0f) {
        x = 0;
        y = 0;
        return;
    }
    n /= sum;
    float u = n.x, v = n.y;
    if (n.z < 0.0f) {
        u = (1.0f - std::fabs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
        v = (1.0f - std::fabs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
    }
    x = snorm16(u);
    y = snorm16(v);
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

struct Bounds {
    simd_float3 lo, hi;
};

Bounds computeBounds(const IndexedMesh& mesh) {
    Bounds bounds{simd_make_float3(INFINITY, INFINITY, INFINITY),
                  simd_make_float3(-INFINITY, -INFINITY, -INFINITY)};
    for (usize v = 0; v < mesh.vertexCount(); ++v) {
        const simd_float3 p = readVector(mesh.positions, v);
        bounds.lo = simd_min(bounds.lo, p);
        bounds.hi = simd_max(bounds.hi, p);
    }
    return bounds;
}

u32 mortonOf(simd_float3 point, const Bounds& bounds) {
    const simd_float3 extent = simd_max(bounds.hi - bounds.lo, simd_float3(1e-30f));
    const simd_float3 unit = simd_clamp((point - bounds.lo) / extent, 0.0f, 1.0f) * 1023.0f;
    return morton3(u32(unit.x), u32(unit.y), u32(unit.z));
}

simd_float3 triangleCentroid(const IndexedMesh& mesh, usize triangle) {
    return (readVector(mesh.positions, mesh.indices[triangle * 3]) +
            readVector(mesh.positions, mesh.indices[triangle * 3 + 1]) +
            readVector(mesh.positions, mesh.indices[triangle * 3 + 2])) /
           3.0f;
}

// Decides whether 16-bit positions are fine enough, from a sample of edge lengths.
bool needsWideGrid(const IndexedMesh& mesh, const Bounds& bounds) {
    const simd_float3 extent = bounds.hi - bounds.lo;
    const float step = simd_reduce_max(extent) / float(kGrid16);
    if (mesh.maxGridStep > 0.0f && step > mesh.maxGridStep) {
        return true;
    }
    std::vector<float> lengths;
    for (usize t = 0; t < mesh.triangleCount(); t += kEdgeSampleStride) {
        const simd_float3 a = readVector(mesh.positions, mesh.indices[t * 3]);
        const simd_float3 b = readVector(mesh.positions, mesh.indices[t * 3 + 1]);
        const simd_float3 c = readVector(mesh.positions, mesh.indices[t * 3 + 2]);
        // The shortest edge, because slivers are what a coarse grid damages.
        lengths.push_back(std::min({simd_length(b - a), simd_length(c - b), simd_length(a - c)}));
    }
    if (lengths.empty()) {
        return false;
    }
    auto middle = lengths.begin() + std::ptrdiff_t(lengths.size() / 2);
    std::nth_element(lengths.begin(), middle, lengths.end());
    return *middle < step * kMinEdgeGridSteps;
}

// Face ids in spatial order, with the order of the caller's faces stored for reporting picks.
struct FaceOrder {
    std::vector<u32> packedOfFace;
    std::vector<u32> originalOfPacked;
};

FaceOrder orderFaces(const IndexedMesh& mesh, const Bounds& bounds) {
    if (mesh.faceOrderIsFixed) {
        FaceOrder order;
        order.packedOfFace.resize(mesh.faceCount);
        std::iota(order.packedOfFace.begin(), order.packedOfFace.end(), 0);
        order.originalOfPacked = order.packedOfFace;
        return order;
    }
    std::vector<simd_float3> sums(mesh.faceCount, simd_make_float3(0.0f, 0.0f, 0.0f));
    std::vector<u32> counts(mesh.faceCount, 0);
    for (usize t = 0; t < mesh.triangleCount(); ++t) {
        const u32 face = mesh.faceOfVertex[mesh.indices[t * 3]];
        sums[face] += triangleCentroid(mesh, t);
        ++counts[face];
    }
    std::vector<u64> keyed(mesh.faceCount);
    for (u32 face = 0; face < mesh.faceCount; ++face) {
        const simd_float3 centroid = sums[face] / float(std::max<u32>(counts[face], 1));
        keyed[face] = u64(mortonOf(centroid, bounds)) << 32 | face;
    }
    std::sort(keyed.begin(), keyed.end());

    FaceOrder order;
    order.packedOfFace.resize(mesh.faceCount);
    order.originalOfPacked.resize(mesh.faceCount);
    for (u32 packed = 0; packed < mesh.faceCount; ++packed) {
        const u32 face = u32(keyed[packed]);
        order.packedOfFace[face] = packed;
        order.originalOfPacked[packed] = face;
    }
    return order;
}

// A range of triangles whose packed faces span under 2^16 ids, so no meshlet built from it can
// overflow its 16-bit local face. Units are also the unit of parallel work.
struct Unit {
    usize first, last; // into the sorted triangle list
};

// A triangle ordered by packed face, then by position. Keeping the key beside the index avoids a
// pointer chase per comparison.
struct SortedTriangle {
    u64 key;
    u32 triangle;

    u32 face() const { return u32(key >> 32); }
};

std::vector<Unit> makeUnits(const std::vector<SortedTriangle>& sortedTriangles) {
    std::vector<Unit> units;
    usize first = 0;
    for (usize t = 1; t <= sortedTriangles.size(); ++t) {
        const bool atEnd = t == sortedTriangles.size();
        if (atEnd || t - first >= kMaxUnitTriangles ||
            sortedTriangles[t].face() - sortedTriangles[first].face() >= kMaxUnitFaces) {
            units.push_back({first, t});
            first = t;
        }
    }
    return units;
}

struct UnitOutput {
    std::vector<PackedVertex> vertices;
    std::vector<u32> highBits;
    std::vector<u32> triangles;
    std::vector<Meshlet> meshlets;
    std::vector<u32> sources; // the mesh vertex of each packed vertex
};

struct PackContext {
    const IndexedMesh& mesh;
    const FaceOrder& faces;
    VertexGrid grid;
    simd_float3 toBoundsGrid; // definition space to 16-bit bounds grid
    float boundsStep;         // largest bounds grid step, in definition units
};

void packVertex(const PackContext& context, u32 vertex, u32 faceBase, UnitOutput& out) {
    const auto [x, y, z] = context.grid.of(readVector(context.mesh.positions, vertex));

    PackedVertex packed{};
    packed.x = u16(x & 0xFFFF);
    packed.y = u16(y & 0xFFFF);
    packed.z = u16(z & 0xFFFF);
    const u32 face = context.faces.packedOfFace[context.mesh.faceOfVertex[vertex]];
    GEO_ASSERT(face >= faceBase && face - faceBase <= 0xFFFF);
    packed.localFace = u16(face - faceBase);
    encodeOctahedral(readVector(context.mesh.normals, vertex), packed.octX, packed.octY);
    out.vertices.push_back(packed);
    out.sources.push_back(vertex);
    if (context.grid.wide) {
        out.highBits.push_back((x >> 16) | (y >> 16) << 8 | (z >> 16) << 16);
    }
}

void packUnit(const PackContext& context, std::span<const SortedTriangle> sortedTriangles,
              UnitOutput& out) {
    const IndexedMesh& mesh = context.mesh;

    // meshoptimizer's cost grows with the vertex range, so each unit gets its own compact ids.
    std::vector<u32> indices;
    indices.reserve(sortedTriangles.size() * 3);
    for (const SortedTriangle& sorted : sortedTriangles) {
        const usize t = sorted.triangle;
        indices.insert(indices.end(),
                       {mesh.indices[t * 3], mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]});
    }
    // Compact ids in order of mesh id, from a bitset over the unit's span of mesh ids: a vertex's
    // id is the number of set bits below it. Linear, and small, at a bit per id in the span.
    const auto [lowest, highest] = std::minmax_element(indices.begin(), indices.end());
    const u32 base = *lowest;
    std::vector<u64> used((usize(*highest - base) >> 6) + 1, 0);
    for (const u32 index : indices) {
        used[(index - base) >> 6] |= u64(1) << ((index - base) & 63);
    }
    std::vector<u32> below(used.size());
    u32 total = 0;
    for (usize w = 0; w < used.size(); ++w) {
        below[w] = total;
        total += u32(std::popcount(used[w]));
    }
    std::vector<u32> globals(total);
    for (usize w = 0; w < used.size(); ++w) {
        u32 n = below[w];
        for (u64 bits = used[w]; bits != 0; bits &= bits - 1) {
            globals[n++] = base + u32(w << 6) + u32(std::countr_zero(bits));
        }
    }
    for (u32& index : indices) {
        const u32 offset = index - base;
        const u64 lower = used[offset >> 6] & ((u64(1) << (offset & 63)) - 1);
        index = below[offset >> 6] + u32(std::popcount(lower));
    }
    std::vector<float> positions(globals.size() * 3);
    for (usize v = 0; v < globals.size(); ++v) {
        std::copy_n(mesh.positions.data() + usize(globals[v]) * 3, 3, positions.data() + v * 3);
    }

    const usize bound =
        meshopt_buildMeshletsBound(indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);
    std::vector<meshopt_Meshlet> built(bound);
    std::vector<unsigned> builtVertices(bound * kMeshletMaxVertices);
    std::vector<unsigned char> builtTriangles(bound * kMeshletMaxTriangles * 3);
    const usize count = meshopt_buildMeshlets(
        built.data(), builtVertices.data(), builtTriangles.data(), indices.data(), indices.size(),
        positions.data(), globals.size(), sizeof(float) * 3, kMeshletMaxVertices,
        kMeshletMaxTriangles, kConeWeight);

    out.meshlets.reserve(count);
    for (usize i = 0; i < count; ++i) {
        const meshopt_Meshlet& m = built[i];
        unsigned* localVertices = &builtVertices[m.vertex_offset];
        unsigned char* localTriangles = &builtTriangles[m.triangle_offset];
        meshopt_optimizeMeshlet(localVertices, localTriangles, m.triangle_count, m.vertex_count);
        const meshopt_Bounds bounds =
            meshopt_computeMeshletBounds(localVertices, localTriangles, m.triangle_count,
                                         positions.data(), globals.size(), sizeof(float) * 3);

        u32 faceBase = 0xFFFFFFFF;
        for (u32 v = 0; v < m.vertex_count; ++v) {
            const u32 face =
                context.faces.packedOfFace[mesh.faceOfVertex[globals[localVertices[v]]]];
            faceBase = std::min(faceBase, face);
        }

        // Both rounding directions are covered by growing the radius, so bounds stay conservative.
        const simd_float3 center =
            simd_make_float3(bounds.center[0], bounds.center[1], bounds.center[2]);
        const simd_float3 gridCenter =
            roundToGrid((center - context.grid.lo) * context.toBoundsGrid);
        const float gridRadius = std::ceil(bounds.radius / context.boundsStep) + 1.0f;

        Meshlet meshlet{};
        meshlet.centerX = u16(gridCenter.x);
        meshlet.centerY = u16(gridCenter.y);
        meshlet.centerZ = u16(gridCenter.z);
        meshlet.radius = u16(std::min(gridRadius, float(kGrid16)));
        meshlet.vertexOffset = u32(out.vertices.size());
        meshlet.triangleOffset = u32(out.triangles.size());
        meshlet.faceBase = faceBase;
        meshlet.vertexCount = u8(m.vertex_count);
        meshlet.triangleCount = u8(m.triangle_count);
        meshlet.coneX = bounds.cone_axis_s8[0];
        meshlet.coneY = bounds.cone_axis_s8[1];
        meshlet.coneZ = bounds.cone_axis_s8[2];
        meshlet.coneCutoff = bounds.cone_cutoff_s8;
        out.meshlets.push_back(meshlet);

        for (u32 v = 0; v < m.vertex_count; ++v) {
            packVertex(context, globals[localVertices[v]], faceBase, out);
        }
        for (u32 t = 0; t < m.triangle_count; ++t) {
            const unsigned char* tri = localTriangles + usize(t) * 3;
            out.triangles.push_back(u32(tri[0]) | u32(tri[1]) << 8 | u32(tri[2]) << 16);
        }
    }
}

} // namespace

PackedDefinition packMesh(const IndexedMesh& mesh) {
    PackedDefinition packed;
    if (mesh.triangleCount() == 0) {
        return packed;
    }

    StageTrace trace("pack");
    const Bounds bounds = computeBounds(mesh);
    const bool wide = needsWideGrid(mesh, bounds);
    const simd_float3 extent = simd_max(bounds.hi - bounds.lo, simd_float3(1e-30f));
    const simd_float3 gridSteps = simd_float3(float(wide ? kGrid24 : kGrid16));
    const simd_float3 boundsSteps = simd_float3(float(kGrid16));
    const simd_float3 scale = extent / gridSteps;
    const simd_float3 boundsScale = extent / boundsSteps;

    const FaceOrder faces = orderFaces(mesh, bounds);
    trace.lap("bounds and face order");

    // Sorted by packed face, then by position, so a unit is spatially tight and a huge face can
    // still be divided among threads.
    std::vector<SortedTriangle> sortedTriangles(mesh.triangleCount());
    parallelRanges(mesh.triangleCount(), 1 << 15, [&](usize first, usize last, usize) {
        for (usize t = first; t < last; ++t) {
            const u32 face = faces.packedOfFace[mesh.faceOfVertex[mesh.indices[t * 3]]];
            const u32 position = mortonOf(triangleCentroid(mesh, t), bounds);
            sortedTriangles[t] = {u64(face) << 32 | position, u32(t)};
        }
    });
    // Triangles in one cell of a face tie on their key, so their index orders them.
    parallelSort(
        sortedTriangles, [](const SortedTriangle& t) { return t.key; },
        [](const SortedTriangle& t) { return t.triangle; });
    trace.lap("sort triangles");
    const std::vector<Unit> units = makeUnits(sortedTriangles);
    const PackContext context{mesh,
                              faces,
                              {bounds.lo, 1.0f / scale, scale, gridSteps, wide},
                              1.0f / boundsScale,
                              simd_reduce_max(boundsScale)};

    // Edges depend only on the mesh and its grid, so a large mesh finds them meanwhile.
    struct EdgeWork {
        const IndexedMesh* mesh;
        EdgeLayout layout;
        FeatureEdges edges;
    } edgeWork{&mesh, {context.grid, boundsScale, faces.packedOfFace}, {}};
    const auto findEdges = [](void* work) {
        auto* w = static_cast<EdgeWork*>(work);
        w->edges = findFeatureEdges(*w->mesh, w->layout);
    };
    const bool concurrent = mesh.triangleCount() >= kConcurrentEdgeTriangles;
    Concurrent edgeTask;
    if (concurrent) {
        edgeTask.start(&edgeWork, findEdges);
    }

    std::vector<UnitOutput> outputs(units.size());
    parallelFor(units.size(), [&](usize u) {
        const std::span<const SortedTriangle> range(sortedTriangles.data() + units[u].first,
                                                    units[u].last - units[u].first);
        packUnit(context, range, outputs[u]);
    });
    // Freed before the outputs are gathered, which holds two copies of them for a moment.
    std::vector<SortedTriangle>().swap(sortedTriangles);

    trace.lap("build meshlets");
    usize vertexCount = 0, triangleCount = 0, meshletCount = 0;
    for (const UnitOutput& output : outputs) {
        vertexCount += output.vertices.size();
        triangleCount += output.triangles.size();
        meshletCount += output.meshlets.size();
    }
    packed.vertices.reserve(vertexCount);
    packed.highBits.reserve(wide ? vertexCount : 0);
    packed.triangles.reserve(triangleCount);
    packed.meshlets.reserve(meshletCount);
    // A packed vertex for each mesh vertex. Meshlets copy shared vertices, and any copy will do,
    // because all of them decode to the same point.
    std::vector<u32> packedVertex(mesh.vertexCount(), 0);
    // Each unit's output is freed as soon as it is copied, so the two copies never both hold all.
    for (UnitOutput& output : outputs) {
        const u32 vertexBase = u32(packed.vertices.size());
        for (usize v = 0; v < output.sources.size(); ++v) {
            packedVertex[output.sources[v]] = vertexBase + u32(v);
        }
        const u32 triangleBase = u32(packed.triangles.size());
        packed.vertices.insert(packed.vertices.end(), output.vertices.begin(),
                               output.vertices.end());
        packed.highBits.insert(packed.highBits.end(), output.highBits.begin(),
                               output.highBits.end());
        packed.triangles.insert(packed.triangles.end(), output.triangles.begin(),
                                output.triangles.end());
        for (Meshlet meshlet : output.meshlets) {
            meshlet.vertexOffset += vertexBase;
            meshlet.triangleOffset += triangleBase;
            packed.meshlets.push_back(meshlet);
        }
        output = {};
    }

    std::copy_n(reinterpret_cast<const float*>(&bounds.lo), 3, packed.boundsMin.begin());
    std::copy_n(reinterpret_cast<const float*>(&bounds.hi), 3, packed.boundsMax.begin());
    std::copy_n(reinterpret_cast<const float*>(&scale), 3, packed.decodeScale.begin());
    std::copy_n(reinterpret_cast<const float*>(&boundsScale), 3, packed.boundsScale.begin());
    packed.flags = (wide ? u32(kDefinitionWide) : 0) | (mesh.closed ? 0 : u32(kDefinitionOpen));
    packed.faceCount = mesh.faceCount;
    packed.originalFace = faces.originalOfPacked;
    trace.lap("concatenate");

    if (concurrent) {
        edgeTask.wait();
    } else {
        findEdges(&edgeWork);
    }
    usePackedVertices(edgeWork.edges, packedVertex);
    packed.edgeSegments = std::move(edgeWork.edges.segments);
    packed.edgeChunks = std::move(edgeWork.edges.chunks);
    trace.lap("edges");
    return packed;
}

std::array<float, 3> decodePosition(const PackedDefinition& definition, usize vertex) {
    const PackedVertex& v = definition.vertices[vertex];
    std::array<u32, 3> grid = {v.x, v.y, v.z};
    if (definition.flags & kDefinitionWide) {
        const u32 high = definition.highBits[vertex];
        grid[0] |= (high & 0xFF) << 16;
        grid[1] |= ((high >> 8) & 0xFF) << 16;
        grid[2] |= ((high >> 16) & 0xFF) << 16;
    }
    return {float(grid[0]) * definition.decodeScale[0] + definition.boundsMin[0],
            float(grid[1]) * definition.decodeScale[1] + definition.boundsMin[1],
            float(grid[2]) * definition.decodeScale[2] + definition.boundsMin[2]};
}

} // namespace geo::import
