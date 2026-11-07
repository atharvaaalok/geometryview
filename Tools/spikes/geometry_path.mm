// Spikes 1 and 3: classic pipeline vs wide mesh shaders on real meshlets, and Metal 3 vs Metal 4.
//
//   spike_geometry <metallib> <file.mesh|file.stl> [instances] [max meshlet vertices]
//
// Renders offscreen at 3456x2234 with memoryless 4x MSAA. Every mode's image is hashed, so a mode
// that silently draws something different is reported rather than appearing fast.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "geometry_path_shared.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <print>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 3456;
constexpr int kHeight = 2234;
constexpr int kWarmupFrames = 6;
constexpr int kMeasuredFrames = 24;
constexpr unsigned kMaxBatchesPerDraw = 4096;

struct Mesh {
    std::vector<float> positions; // xyz per vertex
    std::vector<float> normals;   // xyz per vertex
    std::vector<uint32_t> faces;  // one per vertex
    std::vector<uint32_t> indices;
};

template <typename T>
bool readAll(FILE* f, std::vector<T>& out, size_t count) {
    out.resize(count);
    return std::fread(out.data(), sizeof(T), count, f) == count;
}

bool loadRawMesh(const char* path, Mesh& mesh) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint32_t header[4];
    bool ok = std::fread(header, sizeof(header), 1, f) == 1 && header[0] == 0x48534d47;
    ok = ok && readAll(f, mesh.positions, size_t(header[1]) * 3);
    ok = ok && readAll(f, mesh.normals, size_t(header[1]) * 3);
    ok = ok && readAll(f, mesh.faces, header[1]);
    ok = ok && readAll(f, mesh.indices, size_t(header[2]) * 3);
    std::fclose(f);
    return ok;
}

// Welds a binary STL with meshoptimizer. M1 replaces this with the production parser.
bool loadBinaryStl(const char* path, Mesh& mesh) {
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    std::fseek(f, 80, SEEK_SET);
    uint32_t triangleCount = 0;
    if (std::fread(&triangleCount, 4, 1, f) != 1) {
        std::fclose(f);
        return false;
    }
    std::vector<uint8_t> records(size_t(triangleCount) * 50);
    bool ok = std::fread(records.data(), 1, records.size(), f) == records.size();
    std::fclose(f);
    if (!ok) {
        return false;
    }
    size_t cornerCount = size_t(triangleCount) * 3;
    std::vector<float> corners(cornerCount * 3);
    for (size_t t = 0; t < triangleCount; ++t) {
        std::memcpy(&corners[t * 9], &records[t * 50 + 12], 36);
    }
    std::vector<unsigned> remap(cornerCount);
    size_t vertexCount = meshopt_generateVertexRemap(remap.data(), nullptr, cornerCount,
                                                     corners.data(), cornerCount, 12);
    mesh.positions.resize(vertexCount * 3);
    meshopt_remapVertexBuffer(mesh.positions.data(), corners.data(), cornerCount, 12, remap.data());
    mesh.indices.assign(remap.begin(), remap.end());
    mesh.faces.assign(vertexCount, 0);
    mesh.normals.assign(vertexCount * 3, 0.0f);
    for (size_t t = 0; t < triangleCount; ++t) {
        const uint32_t* tri = &mesh.indices[t * 3];
        simd_float3 p[3];
        for (int i = 0; i < 3; ++i) {
            p[i] = simd_make_float3(mesh.positions[tri[i] * 3], mesh.positions[tri[i] * 3 + 1],
                                    mesh.positions[tri[i] * 3 + 2]);
        }
        simd_float3 n = simd_cross(p[1] - p[0], p[2] - p[0]);
        for (int i = 0; i < 3; ++i) {
            mesh.normals[tri[i] * 3] += n.x;
            mesh.normals[tri[i] * 3 + 1] += n.y;
            mesh.normals[tri[i] * 3 + 2] += n.z;
        }
    }
    return true;
}

int16_t snorm16(float v) {
    return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

void encodeOctahedral(const float* normal, int16_t& x, int16_t& y) {
    simd_float3 n = simd_make_float3(normal[0], normal[1], normal[2]);
    float length = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    if (length == 0.0f) {
        x = y = 0;
        return;
    }
    n /= length;
    float u = n.x, v = n.y;
    if (n.z < 0.0f) {
        u = (1.0f - std::fabs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
        v = (1.0f - std::fabs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
    }
    x = snorm16(u);
    y = snorm16(v);
}

struct Scene {
    simd_float3 boundsMin, boundsMax;
    simd_float3 decodeScale, decodeOffset;
    std::vector<PackedVertex> classicVertices;
    std::vector<PackedVertex> meshletVertices;
    std::vector<uint32_t> meshletTriangles;
    std::vector<SpikeMeshlet> meshlets;
    std::vector<Batch> batches;
    std::vector<simd_float4> instances;
    size_t triangleCount = 0;
};

PackedVertex packVertex(const Mesh& mesh, const Scene& scene, uint32_t v, uint32_t faceBase) {
    PackedVertex out{};
    const float* p = &mesh.positions[size_t(v) * 3];
    simd_float3 q = (simd_make_float3(p[0], p[1], p[2]) - scene.decodeOffset) / scene.decodeScale;
    out.x = uint16_t(std::lround(q.x));
    out.y = uint16_t(std::lround(q.y));
    out.z = uint16_t(std::lround(q.z));
    out.face = uint16_t(mesh.faces[v] - faceBase);
    encodeOctahedral(&mesh.normals[size_t(v) * 3], out.octX, out.octY);
    return out;
}

Scene buildScene(const Mesh& mesh, int instanceCount, unsigned maxVertices) {
    Scene scene{};
    size_t vertexCount = mesh.positions.size() / 3;
    scene.triangleCount = mesh.indices.size() / 3;
    simd_float3 lo = simd_make_float3(INFINITY, INFINITY, INFINITY);
    simd_float3 hi = -lo;
    for (size_t v = 0; v < vertexCount; ++v) {
        simd_float3 p = simd_make_float3(mesh.positions[v * 3], mesh.positions[v * 3 + 1],
                                         mesh.positions[v * 3 + 2]);
        lo = simd_min(lo, p);
        hi = simd_max(hi, p);
    }
    scene.boundsMin = lo;
    scene.boundsMax = hi;
    scene.decodeOffset = lo;
    scene.decodeScale = simd_max(hi - lo, simd_make_float3(1e-6f, 1e-6f, 1e-6f)) / 65535.0f;

    scene.classicVertices.resize(vertexCount);
    for (size_t v = 0; v < vertexCount; ++v) {
        scene.classicVertices[v] = packVertex(mesh, scene, uint32_t(v), 0);
    }

    size_t bound =
        meshopt_buildMeshletsBound(mesh.indices.size(), maxVertices, kMeshletMaxTriangles);
    std::vector<meshopt_Meshlet> built(bound);
    std::vector<unsigned> builtVertices(bound * maxVertices);
    std::vector<unsigned char> builtTriangles(bound * kMeshletMaxTriangles * 3);
    size_t meshletCount = meshopt_buildMeshlets(
        built.data(), builtVertices.data(), builtTriangles.data(), mesh.indices.data(),
        mesh.indices.size(), mesh.positions.data(), vertexCount, 12, maxVertices,
        kMeshletMaxTriangles, 0.25f);

    scene.meshlets.reserve(meshletCount);
    for (size_t i = 0; i < meshletCount; ++i) {
        meshopt_Meshlet& m = built[i];
        meshopt_optimizeMeshlet(&builtVertices[m.vertex_offset], &builtTriangles[m.triangle_offset],
                                m.triangle_count, m.vertex_count);
        meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            &builtVertices[m.vertex_offset], &builtTriangles[m.triangle_offset], m.triangle_count,
            mesh.positions.data(), vertexCount, 12);

        uint32_t faceBase = UINT32_MAX;
        for (unsigned v = 0; v < m.vertex_count; ++v) {
            faceBase = std::min(faceBase, mesh.faces[builtVertices[m.vertex_offset + v]]);
        }
        SpikeMeshlet out{};
        out.centerX = bounds.center[0];
        out.centerY = bounds.center[1];
        out.centerZ = bounds.center[2];
        out.radius = bounds.radius;
        out.vertexOffset = uint32_t(scene.meshletVertices.size());
        out.triangleOffset = uint32_t(scene.meshletTriangles.size());
        out.vertexCount = uint8_t(m.vertex_count);
        out.triangleCount = uint8_t(m.triangle_count);
        out.coneX = bounds.cone_axis_s8[0];
        out.coneY = bounds.cone_axis_s8[1];
        out.coneZ = bounds.cone_axis_s8[2];
        out.coneCutoff = bounds.cone_cutoff_s8;
        out.faceBase = faceBase;
        scene.meshlets.push_back(out);
        for (unsigned v = 0; v < m.vertex_count; ++v) {
            scene.meshletVertices.push_back(
                packVertex(mesh, scene, builtVertices[m.vertex_offset + v], faceBase));
        }
        for (unsigned t = 0; t < m.triangle_count; ++t) {
            const unsigned char* tri = &builtTriangles[m.triangle_offset + t * 3];
            scene.meshletTriangles.push_back(uint32_t(tri[0]) | uint32_t(tri[1]) << 8 |
                                             uint32_t(tri[2]) << 16);
        }
    }

    int columns = int(std::ceil(std::sqrt(double(instanceCount))));
    simd_float3 size = hi - lo;
    for (int i = 0; i < instanceCount; ++i) {
        float x = float(i % columns) * size.x * 1.05f;
        float y = float(i / columns) * size.y * 1.05f;
        scene.instances.push_back(simd_make_float4(x, y, 0.0f, 0.0f));
    }
    for (int i = 0; i < instanceCount; ++i) {
        for (size_t first = 0; first < meshletCount; first += kBatchMeshlets) {
            uint32_t count = uint32_t(std::min<size_t>(kBatchMeshlets, meshletCount - first));
            scene.batches.push_back({uint32_t(first), count, uint32_t(i), 0});
        }
    }
    return scene;
}

FrameData makeFrame(const Scene& scene, float zoom) {
    int columns = int(std::ceil(std::sqrt(double(scene.instances.size()))));
    int rows = int(std::ceil(double(scene.instances.size()) / columns));
    simd_float3 size = scene.boundsMax - scene.boundsMin;
    simd_float3 spread = simd_make_float3(size.x * 1.05f * float(columns - 1),
                                          size.y * 1.05f * float(rows - 1), 0.0f);
    simd_float3 center = (scene.boundsMin + scene.boundsMax) * 0.5f + spread * 0.5f;
    float radius = simd_length(size + spread) * 0.5f;

    simd_float3 forward = simd_normalize(simd_make_float3(-0.55f, 0.65f, -0.52f));
    simd_float3 right = simd_normalize(simd_cross(forward, simd_make_float3(0.0f, 0.0f, 1.0f)));
    simd_float3 up = simd_cross(right, forward);

    float aspect = float(kWidth) / float(kHeight);
    float halfHeight = radius * 1.05f / zoom;
    float halfWidth = halfHeight * aspect;
    simd_float3 eye = center - forward * radius * 2.0f;
    float nearPlane = 0.01f * radius;
    float farPlane = 4.0f * radius;

    simd_float4x4 view = matrix_identity_float4x4;
    view.columns[0] = simd_make_float4(right.x, up.x, -forward.x, 0.0f);
    view.columns[1] = simd_make_float4(right.y, up.y, -forward.y, 0.0f);
    view.columns[2] = simd_make_float4(right.z, up.z, -forward.z, 0.0f);
    view.columns[3] =
        simd_make_float4(-simd_dot(right, eye), -simd_dot(up, eye), simd_dot(forward, eye), 1.0f);

    simd_float4x4 projection = matrix_identity_float4x4;
    projection.columns[0].x = 1.0f / halfWidth;
    projection.columns[1].y = 1.0f / halfHeight;
    projection.columns[2].z = 1.0f / (farPlane - nearPlane); // reversed Z
    projection.columns[3].z = farPlane / (farPlane - nearPlane);

    FrameData frame{};
    frame.viewProjection = simd_mul(projection, view);
    frame.decodeScale = scene.decodeScale;
    frame.decodeOffset = scene.decodeOffset;
    frame.cameraRight = right;
    frame.cameraUp = up;
    frame.cameraForward = forward;
    frame.cameraCenter = center;
    frame.halfWidth = halfWidth;
    frame.halfHeight = halfHeight;
    return frame;
}

struct Result {
    double medianMs;
    size_t covered;
    size_t differingPixels; // against the most recent baseline
    unsigned maxChannelDelta;
};

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

class Bench {
public:
    Bench(id<MTLDevice> device, id<MTLLibrary> library, const Scene& scene)
        : device_(device), library_(library), scene_(scene) {
        queue_ = [device newCommandQueue];
        auto shared = MTLResourceStorageModeShared;
        vertices_ = [device newBufferWithBytes:scene.classicVertices.data()
                                        length:scene.classicVertices.size() * sizeof(PackedVertex)
                                       options:shared];
        meshletVertices_ =
            [device newBufferWithBytes:scene.meshletVertices.data()
                                length:scene.meshletVertices.size() * sizeof(PackedVertex)
                               options:shared];
        triangles_ = [device newBufferWithBytes:scene.meshletTriangles.data()
                                         length:scene.meshletTriangles.size() * sizeof(uint32_t)
                                        options:shared];
        meshlets_ = [device newBufferWithBytes:scene.meshlets.data()
                                        length:scene.meshlets.size() * sizeof(SpikeMeshlet)
                                       options:shared];
        batches_ = [device newBufferWithBytes:scene.batches.data()
                                       length:scene.batches.size() * sizeof(Batch)
                                      options:shared];
        instances_ = [device newBufferWithBytes:scene.instances.data()
                                         length:scene.instances.size() * sizeof(simd_float4)
                                        options:shared];
        frame_ = [device newBufferWithLength:sizeof(FrameData) options:shared];
        size_t drawCount = scene.batches.size() / kMaxBatchesPerDraw + 1;
        bases_ = [device newBufferWithLength:drawCount * sizeof(uint32_t) options:shared];
        auto* bases = static_cast<uint32_t*>(bases_.contents);
        for (size_t i = 0; i < drawCount; ++i) {
            bases[i] = uint32_t(i * kMaxBatchesPerDraw);
        }
        readback_ = [device newBufferWithLength:size_t(kWidth) * kHeight * 4 options:shared];
        createTargets();
        createDepthState();
    }

    void setFrame(const FrameData& frame) { std::memcpy(frame_.contents, &frame, sizeof(frame)); }

    id<MTLRenderPipelineState> classicPipeline() {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [library_ newFunctionWithName:@"classicVertex"];
        d.fragmentFunction = [library_ newFunctionWithName:@"surfaceFragment"];
        configureAttachments(d.colorAttachments);
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        d.rasterSampleCount = 4;
        NSError* error = nil;
        id<MTLRenderPipelineState> pso = [device_ newRenderPipelineStateWithDescriptor:d
                                                                                 error:&error];
        reportIfNil(pso, error);
        return pso;
    }

    id<MTLRenderPipelineState> meshPipeline(NSString* objectName) {
        MTLMeshRenderPipelineDescriptor* d = [MTLMeshRenderPipelineDescriptor new];
        d.objectFunction = [library_ newFunctionWithName:objectName];
        d.meshFunction = [library_ newFunctionWithName:@"meshWide"];
        d.fragmentFunction = [library_ newFunctionWithName:@"surfaceFragment"];
        d.payloadMemoryLength = sizeof(uint32_t) * (kBatchMeshlets + 1);
        d.maxTotalThreadgroupsPerMeshGrid = kBatchMeshlets;
        d.rasterSampleCount = 4;
        configureAttachments(d.colorAttachments);
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError* error = nil;
        id<MTLRenderPipelineState> pso =
            [device_ newRenderPipelineStateWithMeshDescriptor:d
                                                      options:MTLPipelineOptionNone
                                                   reflection:nil
                                                        error:&error];
        reportIfNil(pso, error);
        return pso;
    }

    Result runClassic(id<MTLRenderPipelineState> pso) {
        return measure(^(id<MTLCommandBuffer> commands) {
            id<MTLRenderCommandEncoder> e =
                [commands renderCommandEncoderWithDescriptor:passDescriptor()];
            [e setRenderPipelineState:pso];
            configureRaster(e);
            [e setVertexBuffer:frame_ offset:0 atIndex:kBufferFrame];
            [e setVertexBuffer:vertices_ offset:0 atIndex:kBufferVertices];
            [e setVertexBuffer:instances_ offset:0 atIndex:kBufferInstances];
            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:scene_.triangleCount * 3
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:indices_
                   indexBufferOffset:0
                       instanceCount:scene_.instances.size()];
            [e endEncoding];
        });
    }

    Result runMesh(id<MTLRenderPipelineState> pso) {
        return measure(^(id<MTLCommandBuffer> commands) {
            id<MTLRenderCommandEncoder> e =
                [commands renderCommandEncoderWithDescriptor:passDescriptor()];
            [e setRenderPipelineState:pso];
            configureRaster(e);
            [e setObjectBuffer:frame_ offset:0 atIndex:kBufferFrame];
            [e setObjectBuffer:batches_ offset:0 atIndex:kBufferBatches];
            [e setObjectBuffer:meshlets_ offset:0 atIndex:kBufferMeshlets];
            [e setObjectBuffer:instances_ offset:0 atIndex:kBufferInstances];
            [e setMeshBuffer:frame_ offset:0 atIndex:kBufferFrame];
            [e setMeshBuffer:meshlets_ offset:0 atIndex:kBufferMeshlets];
            [e setMeshBuffer:instances_ offset:0 atIndex:kBufferInstances];
            [e setMeshBuffer:meshletVertices_ offset:0 atIndex:kBufferVertices];
            [e setMeshBuffer:triangles_ offset:0 atIndex:kBufferTriangles];
            for (size_t first = 0, draw = 0; first < scene_.batches.size();
                 first += kMaxBatchesPerDraw, ++draw) {
                [e setObjectBuffer:bases_ offset:draw * sizeof(uint32_t) atIndex:kBufferBase];
                size_t groups = std::min<size_t>(kMaxBatchesPerDraw, scene_.batches.size() - first);
                [e drawMeshThreadgroups:MTLSizeMake(groups, 1, 1)
                    threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                      threadsPerMeshThreadgroup:MTLSizeMake(kMeshletMaxTriangles, 1, 1)];
            }
            [e endEncoding];
        });
    }

    // The same draw through Metal 4: allocator, argument table, residency set, commit feedback.
    Result runMeshMetal4(NSString* objectName) {
        id<MTL4CommandQueue> queue = [device_ newMTL4CommandQueue];
        id<MTL4CommandAllocator> allocator = [device_ newCommandAllocator];
        id<MTL4CommandBuffer> commands = [device_ newCommandBuffer];

        MTLResidencySetDescriptor* residencyDescriptor = [MTLResidencySetDescriptor new];
        NSError* error = nil;
        id<MTLResidencySet> residency = [device_ newResidencySetWithDescriptor:residencyDescriptor
                                                                         error:&error];
        for (id<MTLAllocation> a in @[
                 frame_, batches_, meshlets_, instances_, bases_, meshletVertices_, triangles_,
                 colorResolve_
             ]) {
            [residency addAllocation:a];
        }
        [residency commit];
        [residency requestResidency];
        [queue addResidencySet:residency];

        MTL4ArgumentTableDescriptor* tableDescriptor = [MTL4ArgumentTableDescriptor new];
        tableDescriptor.maxBufferBindCount = 8;
        id<MTL4ArgumentTable> table = [device_ newArgumentTableWithDescriptor:tableDescriptor
                                                                        error:&error];
        reportIfNil(table, error);
        [table setAddress:frame_.gpuAddress atIndex:kBufferFrame];
        [table setAddress:batches_.gpuAddress atIndex:kBufferBatches];
        [table setAddress:meshlets_.gpuAddress atIndex:kBufferMeshlets];
        [table setAddress:instances_.gpuAddress atIndex:kBufferInstances];
        [table setAddress:meshletVertices_.gpuAddress atIndex:kBufferVertices];
        [table setAddress:triangles_.gpuAddress atIndex:kBufferTriangles];

        id<MTLRenderPipelineState> pso = meshPipeline4(objectName);

        MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
        pass.colorAttachments[0].texture = colorMsaa_;
        pass.colorAttachments[0].resolveTexture = colorResolve_;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.949, 0.937, 0.906, 1.0);
        pass.colorAttachments[1].texture = idMsaa_;
        pass.colorAttachments[1].loadAction = MTLLoadActionClear;
        pass.colorAttachments[1].storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.texture = depthMsaa_;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 0.0;

        std::vector<double> samples;
        for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
            [allocator reset];
            [commands beginCommandBufferWithAllocator:allocator];
            id<MTL4RenderCommandEncoder> e = [commands renderCommandEncoderWithDescriptor:pass];
            [e setRenderPipelineState:pso];
            [e setDepthStencilState:depthState_];
            [e setCullMode:MTLCullModeBack];
            [e setFrontFacingWinding:MTLWindingCounterClockwise];
            for (size_t first = 0, draw = 0; first < scene_.batches.size();
                 first += kMaxBatchesPerDraw, ++draw) {
                [table setAddress:bases_.gpuAddress + draw * sizeof(uint32_t) atIndex:kBufferBase];
                [e setArgumentTable:table
                           atStages:MTLRenderStageObject | MTLRenderStageMesh];
                size_t groups = std::min<size_t>(kMaxBatchesPerDraw, scene_.batches.size() - first);
                [e drawMeshThreadgroups:MTLSizeMake(groups, 1, 1)
                    threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                      threadsPerMeshThreadgroup:MTLSizeMake(kMeshletMaxTriangles, 1, 1)];
            }
            [e endEncoding];
            [commands endCommandBuffer];

            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block double elapsed = 0.0;
            MTL4CommitOptions* options = [MTL4CommitOptions new];
            [options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
                if (feedback.error) {
                    std::println(stderr, "MTL4 error: {}",
                                 feedback.error.localizedDescription.UTF8String);
                }
                elapsed = (feedback.GPUEndTime - feedback.GPUStartTime) * 1e3;
                dispatch_semaphore_signal(done);
            }];
            id<MTL4CommandBuffer> list[] = {commands};
            [queue commit:list count:1 options:options];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            if (frame >= kWarmupFrames) {
                samples.push_back(elapsed);
            }
        }
        [residency endResidency];
        return compareToBaseline(median(samples));
    }

    // Makes the most recent image the reference that later runs are compared against.
    void captureBaseline() {
        baseline_.assign(static_cast<const uint8_t*>(readback_.contents),
                         static_cast<const uint8_t*>(readback_.contents) + size_t(kWidth) * kHeight * 4);
    }

    void clearBaseline() { baseline_.clear(); }

    void uploadIndices(const std::vector<uint32_t>& indices) {
        indices_ = [device_ newBufferWithBytes:indices.data()
                                        length:indices.size() * sizeof(uint32_t)
                                       options:MTLResourceStorageModeShared];
    }

private:
    id<MTLRenderPipelineState> meshPipeline4(NSString* objectName) {
        MTL4CompilerDescriptor* compilerDescriptor = [MTL4CompilerDescriptor new];
        NSError* error = nil;
        id<MTL4Compiler> compiler = [device_ newCompilerWithDescriptor:compilerDescriptor
                                                                 error:&error];
        reportIfNil(compiler, error);
        auto function = ^(NSString* name) {
            MTL4LibraryFunctionDescriptor* f = [MTL4LibraryFunctionDescriptor new];
            f.library = library_;
            f.name = name;
            return f;
        };
        MTL4MeshRenderPipelineDescriptor* d = [MTL4MeshRenderPipelineDescriptor new];
        d.objectFunctionDescriptor = function(objectName);
        d.meshFunctionDescriptor = function(@"meshWide");
        d.fragmentFunctionDescriptor = function(@"surfaceFragment");
        d.payloadMemoryLength = sizeof(uint32_t) * (kBatchMeshlets + 1);
        d.maxTotalThreadgroupsPerMeshGrid = kBatchMeshlets;
        d.rasterSampleCount = 4;
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        d.colorAttachments[1].pixelFormat = MTLPixelFormatR32Uint;
        id<MTLRenderPipelineState> pso = [compiler newRenderPipelineStateWithDescriptor:d
                                                                    compilerTaskOptions:nil
                                                                                  error:&error];
        reportIfNil(pso, error);
        return pso;
    }

    void configureAttachments(MTLRenderPipelineColorAttachmentDescriptorArray* attachments) {
        attachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        attachments[1].pixelFormat = MTLPixelFormatR32Uint;
    }

    void reportIfNil(id object, NSError* error) {
        if (!object) {
            std::println(stderr, "Metal object creation failed: {}",
                         error ? error.localizedDescription.UTF8String : "unknown");
            std::exit(1);
        }
    }

    void createTargets() {
        auto make = ^(MTLPixelFormat format, NSUInteger samples, MTLStorageMode storage,
                      MTLTextureUsage usage) {
            MTLTextureDescriptor* d =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                   width:kWidth
                                                                  height:kHeight
                                                               mipmapped:NO];
            d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
            d.sampleCount = samples;
            d.storageMode = storage;
            d.usage = usage;
            return [device_ newTextureWithDescriptor:d];
        };
        colorMsaa_ = make(MTLPixelFormatBGRA8Unorm, 4, MTLStorageModeMemoryless,
                          MTLTextureUsageRenderTarget);
        idMsaa_ = make(MTLPixelFormatR32Uint, 4, MTLStorageModeMemoryless,
                       MTLTextureUsageRenderTarget);
        depthMsaa_ = make(MTLPixelFormatDepth32Float, 4, MTLStorageModeMemoryless,
                          MTLTextureUsageRenderTarget);
        colorResolve_ = make(MTLPixelFormatBGRA8Unorm, 1, MTLStorageModePrivate,
                             MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    }

    void createDepthState() {
        MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
        d.depthCompareFunction = MTLCompareFunctionGreater;
        d.depthWriteEnabled = YES;
        depthState_ = [device_ newDepthStencilStateWithDescriptor:d];
    }

    void configureRaster(id<MTLRenderCommandEncoder> e) {
        [e setDepthStencilState:depthState_];
        [e setCullMode:MTLCullModeBack];
        [e setFrontFacingWinding:MTLWindingCounterClockwise];
    }

    MTLRenderPassDescriptor* passDescriptor() {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = colorMsaa_;
        pass.colorAttachments[0].resolveTexture = colorResolve_;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.949, 0.937, 0.906, 1.0);
        pass.colorAttachments[1].texture = idMsaa_;
        pass.colorAttachments[1].loadAction = MTLLoadActionClear;
        pass.colorAttachments[1].storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.texture = depthMsaa_;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 0.0;
        return pass;
    }

    Result measure(void (^encode)(id<MTLCommandBuffer>)) {
        std::vector<double> samples;
        for (int frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
            id<MTLCommandBuffer> commands = [queue_ commandBuffer];
            encode(commands);
            [commands commit];
            [commands waitUntilCompleted];
            if (commands.error) {
                std::println(stderr, "GPU error: {}",
                             commands.error.localizedDescription.UTF8String);
            }
            if (frame >= kWarmupFrames) {
                samples.push_back((commands.GPUEndTime - commands.GPUStartTime) * 1e3);
            }
        }
        return compareToBaseline(median(samples));
    }

    Result compareToBaseline(double medianMs) {
        readBackResolved();
        const auto* pixels = static_cast<const uint8_t*>(readback_.contents);
        Result result{medianMs, covered(), 0, 0};
        if (baseline_.empty()) {
            return result;
        }
        size_t pixelCount = size_t(kWidth) * kHeight;
        for (size_t i = 0; i < pixelCount; ++i) {
            unsigned delta = 0;
            for (int c = 0; c < 3; ++c) {
                int d = int(pixels[i * 4 + c]) - int(baseline_[i * 4 + c]);
                delta = std::max(delta, unsigned(std::abs(d)));
            }
            result.differingPixels += delta != 0;
            result.maxChannelDelta = std::max(result.maxChannelDelta, delta);
        }
        return result;
    }

    void readBackResolved() {
        id<MTLCommandBuffer> commands = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:colorResolve_
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(kWidth, kHeight, 1)
                            toBuffer:readback_
                   destinationOffset:0
              destinationBytesPerRow:size_t(kWidth) * 4
            destinationBytesPerImage:size_t(kWidth) * kHeight * 4];
        [blit endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
    }

    size_t covered() {
        const auto* pixels = static_cast<const uint32_t*>(readback_.contents);
        size_t count = size_t(kWidth) * kHeight;
        uint32_t background = pixels[0];
        size_t n = 0;
        for (size_t i = 0; i < count; ++i) {
            n += pixels[i] != background;
        }
        return n;
    }

    id<MTLDevice> device_;
    id<MTLLibrary> library_;
    const Scene& scene_;
    id<MTLCommandQueue> queue_;
    id<MTLBuffer> vertices_, indices_, meshletVertices_, triangles_, meshlets_, batches_;
    id<MTLBuffer> instances_, frame_, bases_, readback_;
    id<MTLTexture> colorMsaa_, idMsaa_, depthMsaa_, colorResolve_;
    id<MTLDepthStencilState> depthState_;
    std::vector<uint8_t> baseline_;
};

void printRow(const char* name, const Result& r, const Result& baseline) {
    double share = 100.0 * double(r.differingPixels) / double(std::max<size_t>(r.covered, 1));
    std::println("  {:<32} {:>7.2f} ms {:>5.2f}x  {:>8} px  diff {:>7} px ({:.3f}%) max {:>3}/255",
                 name, r.medianMs, r.medianMs / baseline.medianMs, r.covered, r.differingPixels,
                 share, r.maxChannelDelta);
}

} // namespace

int main(int argc, const char** argv) {
    @autoreleasepool {
        if (argc < 3) {
            std::println(stderr, "usage: spike_geometry <metallib> <file.mesh|file.stl> [instances]");
            return 2;
        }
        std::string path = argv[2];
        int instances = argc > 3 ? std::atoi(argv[3]) : 1;
        unsigned maxVertices = argc > 4 ? unsigned(std::atoi(argv[4])) : 64;

        Mesh mesh;
        bool loaded = path.ends_with(".stl") ? loadBinaryStl(path.c_str(), mesh)
                                             : loadRawMesh(path.c_str(), mesh);
        if (!loaded) {
            std::println(stderr, "could not load {}", path);
            return 1;
        }

        NSDate* start = [NSDate date];
        Scene scene = buildScene(mesh, instances, maxVertices);
        double buildSeconds = -start.timeIntervalSinceNow;

        size_t vertexSum = 0, triangleSum = 0;
        for (const SpikeMeshlet& m : scene.meshlets) {
            vertexSum += m.vertexCount;
            triangleSum += m.triangleCount;
        }
        size_t meshletCount = scene.meshlets.size();
        double triangleFill = double(triangleSum) / double(meshletCount * kMeshletMaxTriangles);
        double vertexFill = double(vertexSum) / double(meshletCount * maxVertices);
        double duplication =
            double(scene.meshletVertices.size()) / double(mesh.positions.size() / 3);
        std::println("{} triangles x{} instances = {:.1f} Mtri, {} meshlets, built in {:.2f} s",
                     scene.triangleCount, instances,
                     double(scene.triangleCount) * instances / 1e6, meshletCount, buildSeconds);
        std::println("meshlet fill: triangles {:.1f}%, vertices {:.1f}%, vertex duplication {:.2f}x",
                     triangleFill * 100, vertexFill * 100, duplication);

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError* error = nil;
        id<MTLLibrary> library =
            [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        if (!library) {
            std::println(stderr, "library: {}", error.localizedDescription.UTF8String);
            return 1;
        }

        Bench bench(device, library, scene);
        bench.uploadIndices(mesh.indices);
        id<MTLRenderPipelineState> classic = bench.classicPipeline();
        id<MTLRenderPipelineState> meshAll = bench.meshPipeline(@"objectAll");
        id<MTLRenderPipelineState> meshCull = bench.meshPipeline(@"objectCull");

        for (float zoom : {1.0f, 2.5f, 5.0f}) {
            bench.setFrame(makeFrame(scene, zoom));
            std::println("zoom {:.1f}x:", zoom);
            bench.clearBaseline();
            Result baseline = bench.runClassic(classic);
            bench.captureBaseline();
            printRow("classic indexed (Metal 3)", baseline, baseline);
            printRow("mesh wide, no culling (Metal 3)", bench.runMesh(meshAll), baseline);
            printRow("mesh wide, culling (Metal 3)", bench.runMesh(meshCull), baseline);
            printRow("mesh wide, no culling (Metal 4)", bench.runMeshMetal4(@"objectAll"), baseline);
            printRow("mesh wide, culling (Metal 4)", bench.runMeshMetal4(@"objectCull"), baseline);
        }
    }
    return 0;
}
