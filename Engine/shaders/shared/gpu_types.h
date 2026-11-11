// Runtime records shared by the renderer and its shaders. This header compiles as both C++ and
// Metal Shading Language, so the two can never disagree about a layout.
#pragma once

#include "format/gpu_records.h"
#ifdef __METAL_VERSION__
#include <simd/simd.h>
#else
#include "base/simd.h"
#endif

#ifdef __METAL_VERSION__
#define GEO_DEVICE_POINTER(Type) const device Type*
#else
#define GEO_DEVICE_POINTER(Type) uint64_t
#endif

enum SurfaceLimits : unsigned {
    kBatchMeshlets = 32,
    // One dispatch of 120k threadgroups was seen to draw nothing, so draws stay well below that.
    kMaxBatchesPerDraw = 4096,
    kTileSize = 16, // 4x MSAA does not support 32x32 tiles
};

// Section planes cut the model; caps are drawn where the cut opens a closed part.
enum SectionLimits : unsigned {
    kMaxSectionPlanes = 6,
};

// The body tag's top bit, set on section caps, so outlines can tell a cut face from its part.
enum TagBits : unsigned {
    kTagCap = 0x80,   // in red: a section cap
    kTagShell = 0x80, // in green: the see-through shell, rather than a ghost
    kTagValues = 127, // tags run from 1 to this
};

// An occurrence's look index has its top bit set when the occurrence is part of the shell that
// see-through fades.
enum LookBits : uint32_t {
    kLookShell = 0x8000,
    kLookIndex = 0x7FFF,
};

// The see-through layers, drawn in turn by the surface functions.
enum ShellLayer : uint32_t {
    kShellNone = 0,  // opaque surfaces, leaving out the shell while it is faded
    kShellBack = 1,  // the shell's far walls, held behind everything
    kShellDepth = 2, // the shell's nearest front faces, into depth only
    kShellFront = 3, // those faces again, blended once
};

enum BufferIndex : unsigned {
    kBufferFrame = 0,
    kBufferDefinitions = 1,
    kBufferOccurrences = 2,
    kBufferBatches = 3,
    kBufferDrawBase = 4,
    kBufferOccurrenceState = 5,
    kBufferFaceState = 6,
    kBufferMeshletRefs = 7,
    kBufferStyle = 8,
    kBufferPalette = 9, // three RGBA8 texels per look
    kBufferLooks = 10,  // a 16-bit look per occurrence
    kBufferCount = 11,
};

enum TextureIndex : unsigned {
    kTexturePickIds = 0,
    kTexturePickDepths = 1,
    kTextureTags = 2,
    kTextureAo = 3,     // half resolution
    kTextureShadow = 4, // half resolution
};

enum AttachmentIndex : unsigned {
    kAttachmentColor = 0,
    kAttachmentId = 1,
    kAttachmentDepth = 2,
    // Ink coverage of feature edges, kept apart from colour so overlapping segments take the
    // larger coverage rather than inking twice: r creases, g ghost lines, b highlights.
    kAttachmentInk = 3,
    // Which body a pixel shows, as a small tag that differs between neighbours: r for solid
    // surfaces and g for ghosts in front of them. Outlines are drawn where tags change.
    kAttachmentTags = 4,
    // With a section open, which closed part the cut plane lies inside at each sample: every
    // surface behind the cut toggles its body's tag into the low seven bits, and the top bit
    // counts the surfaces. Where the top bit ends set, the low bits are the tag of the part.
    kAttachmentParity = 5,
};

// One per definition. Geometry is addressed directly, so a scene can span several mapped files.
struct GpuDefinition {
    GEO_DEVICE_POINTER(Meshlet) meshlets;
    GEO_DEVICE_POINTER(PackedVertex) vertices;
    GEO_DEVICE_POINTER(uint32_t) highBits;
    GEO_DEVICE_POINTER(uint32_t) triangles;
    GEO_DEVICE_POINTER(EdgeSegment) edgeSegments;
    GEO_DEVICE_POINTER(EdgeChunk) edgeChunks;
    simd_float3 boundsMin;
    simd_float3 decodeScale;
    simd_float3 boundsScale;
    float boundsStep; // the largest component of boundsScale
    uint32_t flags;
    uint32_t vertexCount; // for clamping what a cache file says, which is untrusted
    uint32_t faceCount;
    // How far its edges move toward the eye, in its own units: twice its tessellation's chord
    // error, so a line on a curved face is not hidden by the facets beside it. Zero when flat.
    float edgeLift;
};

// One placed instance of a definition.
struct GpuOccurrence {
    simd_float4 rows[3]; // a 3x4 transform by rows
    uint32_t definition;
    uint32_t idBase;     // pick ids of this occurrence start here
    uint32_t shellGroup; // which separate assembly see-through classifies it with, 0 for none
    uint32_t pad;
};

// Flags shared by occurrences and faces. A face shows its own flags combined with its occurrence's.
enum StateFlags : uint32_t {
    kStateHidden = 1u << 0,
    kStateGhost = 1u << 1,
    kStateSelected = 1u << 2,
    // Occurrence only: some face has its own flag, so the per-face page must be read.
    kStateFaceHidden = 1u << 4,
    kStateFaceGhost = 1u << 5,
    kStateFaceSelected = 1u << 6,
};

// Per occurrence: its own flags, and where its per-face bytes start. Occurrences with no face-level
// change point at a shared page of zeros.
struct OccurrenceState {
    uint32_t flags;
    uint32_t facePage;
};

// Up to kBatchMeshlets meshlets, culled together by one object thread. A large definition's
// batches cover consecutive meshlets of one occurrence. Small definitions' meshlets are packed from
// many occurrences into shared batches through MeshletRefs, so a million one-meshlet parts need
// 31k object threadgroups rather than a million, and the driver memory that scales with them.
enum BatchKind : uint32_t {
    kBatchRun = 0,    // meshlets [first, first + count) of `occurrence`
    kBatchPacked = 1, // MeshletRefs [first, first + count)
};

struct Batch {
    uint32_t kind;
    uint32_t occurrence;
    uint32_t first;
    uint32_t count;
};

struct MeshletRef {
    uint32_t occurrence;
    uint32_t meshlet;
};

// The view cube and scale bar, drawn over the model at the end of the main pass.
enum OverlayLimits : unsigned {
    kOverlayMaxQuads = 48,
    kAtlasWidth = 512,
    kAtlasLabelWidth = 144, // the label region's shape on a cube face, 3 to 1
    kAtlasLabelHeight = 48, // one row per cube face
    kAtlasGlyphWidth = 28,
    kAtlasGlyphHeight = 48, // one strip of cells below the labels
    kAtlasHeight = 6 * 48 + 48,
};

// Pick ids at and above this are the view cube's faces, far above any model's.
enum CubeIds : uint32_t {
    kCubeIdBase = 0xFFFFFF00u,
};

enum OverlayBufferIndex : unsigned {
    kOverlayUniforms = 0,
    kOverlayQuads = 1,
};

// A screen-space rectangle in pixels, from the top left. Solid if uv.z < 0, else coverage from the
// atlas region uv.xy to uv.zw tints it.
struct OverlayQuad {
    simd_float2 origin;
    simd_float2 size;
    simd_float4 uv;
    simd_float4 color;
};

struct OverlayUniforms {
    simd_float3x3 viewRotation; // world to view, so the cube turns with the camera
    simd_float2 viewport;       // pixels
    simd_float2 cubeCenter;     // pixels
    float cubeHalf;             // half the cube's edge, in pixels
    float lineWidth;            // the ink edge, in pixels
    uint32_t cubeHover;         // face index plus one, or zero
    uint32_t quadCount;
};

// How ghosts draw: bit 0 a faint skin, bit 1 dashed edges.
enum GhostStyle : uint32_t {
    kGhostShell = 1,
    kGhostLines = 2,
    kGhostBoth = 3,
};

// What a render style sets for every shader, in encoded colour and in pixels. Built when the style
// or the display scale changes, not per frame.
struct StyleUniforms {
    simd_float3 sheetTop;
    simd_float3 sheetBottom;
    simd_float3 gridColor;
    simd_float3 grainTint;
    simd_float3 ink;
    simd_float3 hover;
    simd_float3 hideTint;
    simd_float3 shadowColor;
    simd_float3 lightExterior; // view space, for flat bands
    simd_float3 lightInterior; // view space, for the inside of an opened part
    simd_float3 sky;           // the studio, in linear radiance
    simd_float3 floor;
    simd_float3 key;
    simd_float3 band;
    simd_float3 fill;
    simd_uint4 selection; // three palette texels, as for a body
    simd_float2 strips;
    float vignette;
    float gridAlpha;
    float gridPitch; // pixels, or 0 for none
    float grain;     // 0 to 1
    float inkAlpha;
    float silhouette; // ink widths, pixels
    float partLine;
    float crease;
    float highlight;
    float ghostInk; // a ghost's outline, as a share of the ink
    float ghostAlpha;
    float ghostLineAlpha;
    float hoverAmount;
    float ao; // strength, 0 for none
    float shadowAlpha;
    float shadowSoft;   // blur radius, pixels
    float wobble;       // amplitude, pixels
    float wobblePeriod; // pixels
    float pixelsPerPoint;
    float sharpness;
    float exposure;
    float contrast;
    uint32_t ghostStyle;
    uint32_t shadowOn;
    float capInk;       // 0 hatches caps in the part's own dark tone, 1 in ink
    float metalDiffuse; // the share of diffuse light a metal keeps
    float fold;         // the ink of a body folding in front of itself, 0 for none
    float ghostLine;    // the width of a ghost's dashed lines, pixels
    float sheetAlpha;   // 1 for an opaque sheet; 0 leaves the background clear, premultiplied
};

struct FrameUniforms {
    simd_float4x4 viewProjection;
    simd_float3x3 viewRotation; // world to view, for lighting
    simd_float3 eye;
    simd_float3 right;
    simd_float3 up;
    simd_float3 forward;
    float halfWidth; // orthographic half extents
    float halfHeight;
    float tanHalfX; // perspective half-angle tangents
    float tanHalfY;
    float secHalfX; // reciprocal cosines, for sphere tests
    float secHalfY;
    uint32_t perspective;
    // Hover covers a range of ids, so a whole body or part lights up in those selection modes.
    uint32_t hoverFirst;
    uint32_t hoverEnd;
    uint32_t ghostPickable; // ghosts write ids, while the ghost tool is held, so a click restores
    uint32_t hoverHides;    // the hide tool is held, so hover shows what a click would hide
    simd_float2 viewport;   // pixels
    // How far every edge moves toward the eye at least, in points of depth at its distance: enough
    // for a line's own width across a concave crease, and for depth precision. Each definition
    // adds its chord error (GpuDefinition.edgeLift).
    float edgeFloor;
    float worldPerPixel; // orthographic, or at unit depth in perspective
    float depthNear;     // for turning stored depth back into distance
    float depthFar;
    float aoRadius;     // world units
    uint32_t aoSamples; // fewer while the view moves
    // The cast shadow: a high sun whose bearing follows the camera, so the shadow always falls
    // toward the viewer's lower left, onto a ground just below the model.
    simd_float3 lightTravel; // unit direction the light travels
    float ground;            // height of the ground along z
    float shadowVisibility;  // full from above, gone from below the ground
    // World planes: where dot(xyz, p) + w > 0 is cut away.
    simd_float4 sectionPlanes[kMaxSectionPlanes];
    // Each plane's stored depth across the screen, for drawing caps on the plane itself:
    // depth = x * ndc.x + y * ndc.y + z, where w is 1; w is 0 for a plane seen edge on.
    simd_float4 sectionDepths[kMaxSectionPlanes];
    uint32_t sectionPlaneCount;
    float seeThrough;       // k: 0 solid, 1 fully see-through
    uint32_t excludeShells; // classifying: leave out parts already found to be shell
    uint32_t classifyGroup; // classifying: draw only the occurrences of this group, or all for 0
    // Where this frame samples each pixel, from its centre, in pixels with y down. Zero but in
    // the WebGPU renderer's jittered samples, whose shaders move what Metal evaluates once per
    // pixel back to the centre.
    simd_float2 sampleOffset;
};
