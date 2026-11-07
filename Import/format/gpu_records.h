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

#ifndef __METAL_VERSION__
static_assert(sizeof(PackedVertex) == 12);
static_assert(sizeof(Meshlet) == 32);
#endif
