// Integer hash and value noise for paper grain and hand-drawn wobble. The engine mirrors this
// exactly on the CPU, so the pointer can follow the wobble.
#pragma once

#include <metal_stdlib>

static inline uint noiseHash(int2 v) {
    const uint x = uint(v.x + 32768);
    const uint y = uint(v.y + 32768);
    uint h = (x * 0x8DA6B343u) ^ (y * 0xD8163841u);
    h ^= h >> 13u;
    h *= 0x5BD1E995u;
    h ^= h >> 15u;
    return h;
}

static inline float noiseAt(int2 v) {
    return float(noiseHash(v) & 0xFFFFu) / 65535.0;
}

// Smooth noise in [0, 1] with one feature per unit.
static inline float valueNoise(float2 p) {
    const float2 i = metal::floor(p);
    const float2 f = p - i;
    const float2 u = f * f * (3.0 - 2.0 * f);
    const int2 c = int2(i);
    return metal::mix(metal::mix(noiseAt(c), noiseAt(c + int2(1, 0)), u.x),
                      metal::mix(noiseAt(c + int2(0, 1)), noiseAt(c + int2(1, 1)), u.x), u.y);
}
