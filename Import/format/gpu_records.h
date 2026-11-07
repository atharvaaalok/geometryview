// Records stored in cache files and read directly by shaders. This header compiles as both C++ and
// Metal Shading Language, so the two can never disagree about a layout.
#pragma once

#ifdef __METAL_VERSION__
#include <metal_stdlib>
#else
#include <cstdint>
#endif

// Enums rather than constexpr, because they compile unchanged in both languages.
enum MeshletLimits : unsigned {
    // See ADR 0001. CAD vertices are split per face, so vertices are about equal to triangles and a
    // 64-vertex limit would leave half of each meshlet's triangles unused.
    kMeshletMaxVertices = 124,
    kMeshletMaxTriangles = 124,
};

enum DefinitionFlags : uint32_t {
    // The definition's vertices have a parallel high-bits stream, giving a 24-bit grid. See
    // ADR 0004.
    kDefinitionWide = 1u << 0,
    // The surface is not closed, so both sides can show and back-face culling must stay off.
    kDefinitionOpen = 1u << 1,
};

// 12 bytes. Positions are integers on a per-definition grid, so vertices at the same place in
// different meshlets decode to identical floats and shared edges stay watertight.
struct PackedVertex {
    uint16_t x, y, z;
    uint16_t localFace; // added to the meshlet's faceBase
    int16_t octX, octY; // octahedral unit normal
};

// 32 bytes. Bounds use a 16-bit grid across the definition, rounded up to stay conservative.
struct Meshlet {
    uint16_t centerX, centerY, centerZ;
    uint16_t radius;
    uint32_t vertexOffset;
    uint32_t triangleOffset;
    uint32_t faceBase;
    uint8_t vertexCount;
    uint8_t triangleCount;
    int8_t coneX, coneY, coneZ, coneCutoff;
    uint16_t reserved0;
    uint32_t reserved1; // kept for a level-of-detail error
};

// What a feature edge is, which decides when it is drawn.
enum EdgeClass : uint32_t {
    kEdgeCrease = 0,   // faces meet at an angle: always inked
    kEdgeSmooth = 1,   // faces meet tangentially: inked only around a highlighted face
    kEdgeBoundary = 2, // a face on one side only: the rim of an open surface
};

enum EdgeLimits : uint32_t {
    kEdgeChunkSegments = 32, // one SIMD group of segments, culled together
    kEdgeNoFace = 0x3FFFFFFF,
    kEdgeClassShift = 30,
};

// 20 bytes. A straight piece of a feature edge. Its ends are vertices of the definition itself, so
// the line decodes to exactly the points of the surface it borders.
struct EdgeSegment {
    uint32_t a, b;  // vertices, in the order the edge's chain runs
    uint32_t faceA; // the face on one side
    uint32_t faceB; // the face on the other side, or kEdgeNoFace; the class is in the top two bits
    float along;    // arc length at `a` from the start of its chain, in definition units
};

// 16 bytes. Consecutive segments, culled together. Bounds use the definition's 16-bit bounds grid,
// as meshlets do.
struct EdgeChunk {
    uint16_t centerX, centerY, centerZ;
    uint16_t radius;
    uint32_t firstSegment;
    uint32_t segmentCount;
};

#ifndef __METAL_VERSION__
static_assert(sizeof(PackedVertex) == 12);
static_assert(sizeof(Meshlet) == 32);
static_assert(sizeof(EdgeSegment) == 20);
static_assert(sizeof(EdgeChunk) == 16);
#endif
