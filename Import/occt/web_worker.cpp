// The OCCT worker for a Web Worker: the same meshing as geometryview-occt-worker, called directly
// rather than over pipes. The page's script hands it one slice at a time and reads the reply.
//
//   occt_init()
//   occt_mesh(text, size, linear, angular, minSize) -> 1 if meshed, 0 if OCCT failed
//   occt_stats()    eleven doubles: status, faces, unmeshed faces, payload bytes, triangles, area,
//                   signed volume, seconds reading, transferring and meshing, and unmapped faces
//   occt_payload()  the reply's payload, occt_stats()[3] bytes, as the native worker sends it
//
// For testing recovery, occt_trap() traps as a crash in OCCT would, and occt_exhaust() allocates
// until the worker's memory runs out.
#include "base/types.h"
#include "occt/mesh_shape.h"
#include "occt/worker_protocol.h"

#include <emscripten/emscripten.h>

#include <cmath>
#include <cstring>
#include <new>
#include <string_view>
#include <vector>

using namespace geo;
using namespace geo::import::worker;

namespace {

std::vector<u8> payload;
double stats[11];

// The area and signed volume of the payload's triangles, to compare with the native build.
void measure(u32 faces, double& area, double& volume, double& triangles) {
    area = volume = triangles = 0.0;
    usize at = 0;
    for (u32 f = 0; f < faces && at + sizeof(FaceHeader) <= payload.size(); ++f) {
        FaceHeader header{};
        std::memcpy(&header, payload.data() + at, sizeof(header));
        at += sizeof(header);
        const auto* positions = reinterpret_cast<const float*>(payload.data() + at);
        at += usize(header.vertexCount) * 6 * sizeof(float);
        for (u32 t = 0; t < header.triangleCount; ++t) {
            u32 index[3];
            std::memcpy(index, payload.data() + at + usize(t) * 12, 12);
            double p[3][3];
            for (u32 k = 0; k < 3; ++k) {
                for (u32 axis = 0; axis < 3; ++axis) {
                    float value;
                    std::memcpy(&value, positions + usize(index[k]) * 3 + axis, sizeof(value));
                    p[k][axis] = value;
                }
            }
            const double u[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            const double v[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            const double c[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                                 u[0] * v[1] - u[1] * v[0]};
            area += 0.5 * std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            volume += (p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) -
                       p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
                       p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0])) /
                      6.0;
        }
        at += usize(header.triangleCount) * 12;
        triangles += header.triangleCount;
    }
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE void occt_init() { initializeOcct(); }

EMSCRIPTEN_KEEPALIVE int occt_mesh(const char* text, u32 size, double linear, double angular,
                                   double minSize) {
    Request request{};
    request.magic = kMagic;
    request.kind = u32(RequestKind::Slice);
    request.textBytes = size;
    request.linearDeflection = linear;
    request.angularDeflection = angular;
    request.minSize = minSize;
    Reply reply{};
    payload.clear();
    meshSlice(std::string_view(text, size), request, reply, payload);
    double area = 0, volume = 0, triangles = 0;
    measure(reply.faceCount, area, volume, triangles);
    const double values[11] = {double(reply.status), double(reply.faceCount),
                               double(reply.unmeshedFaces), double(payload.size()),
                               triangles, area, volume, reply.readSeconds,
                               reply.transferSeconds, reply.meshSeconds,
                               double(reply.unmappedFaces)};
    std::memcpy(stats, values, sizeof(stats));
    return reply.status == u32(ReplyStatus::Ok) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const double* occt_stats() { return stats; }

EMSCRIPTEN_KEEPALIVE const u8* occt_payload() { return payload.data(); }

EMSCRIPTEN_KEEPALIVE void occt_trap() { __builtin_trap(); }

// Returns how many megabytes it got before an allocation failed, keeping all of it.
EMSCRIPTEN_KEEPALIVE u32 occt_exhaust() {
    static std::vector<std::vector<u8>> held;
    u32 megabytes = 0;
    for (;;) {
        std::vector<u8> block;
        try {
            block.resize(64u << 20, 1); // touched, so the memory is really committed
        } catch (const std::bad_alloc&) {
            return megabytes;
        }
        held.push_back(std::move(block));
        megabytes += 64;
    }
}

} // extern "C"
