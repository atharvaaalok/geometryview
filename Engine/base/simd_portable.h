// The part of Apple's <simd/simd.h> this project uses, for platforms without it.
//
// The types are clang extended vectors, laid out exactly as Apple's: a three-element vector takes
// the space of four. Results match Apple's to rounding; Tests/unit/simd_portable_test.cpp
// compares them on Apple platforms. Defining GEO_SIMD_NAMESPACE puts everything in that namespace,
// which is how that test includes both side by side.
#pragma once

#include <cmath>

#ifdef GEO_SIMD_NAMESPACE
namespace GEO_SIMD_NAMESPACE {
#endif

typedef float simd_float2 __attribute__((ext_vector_type(2)));
typedef float simd_float3 __attribute__((ext_vector_type(3)));
typedef float simd_float4 __attribute__((ext_vector_type(4)));
typedef double simd_double2 __attribute__((ext_vector_type(2)));
typedef double simd_double3 __attribute__((ext_vector_type(3)));
typedef double simd_double4 __attribute__((ext_vector_type(4)));
typedef unsigned simd_uint4 __attribute__((ext_vector_type(4)));
typedef int simd_int3 __attribute__((ext_vector_type(3)));
typedef long long simd_long3 __attribute__((ext_vector_type(3)));

struct simd_float3x3 {
    simd_float3 columns[3];
};
struct simd_float4x4 {
    simd_float4 columns[4];
};
struct simd_double3x3 {
    simd_double3 columns[3];
};
struct simd_quatd {
    simd_double4 vector; // the imaginary part in x, y and z, the real part in w
};

inline constexpr simd_float3x3 matrix_identity_float3x3 = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
inline constexpr simd_float4x4 matrix_identity_float4x4 = {
    {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
inline constexpr simd_double3x3 matrix_identity_double3x3 = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

// ---- Construction ------------------------------------------------------------------------------

inline simd_float2 simd_make_float2(float x, float y) {
    return simd_float2{x, y};
}
inline simd_float3 simd_make_float3(float x, float y, float z) {
    return simd_float3{x, y, z};
}
inline simd_float4 simd_make_float4(float x, float y, float z, float w) {
    return simd_float4{x, y, z, w};
}
inline simd_double3 simd_make_double3(double x, double y, double z) {
    return simd_double3{x, y, z};
}
inline simd_uint4 simd_make_uint4(unsigned x, unsigned y, unsigned z, unsigned w) {
    return simd_uint4{x, y, z, w};
}

inline simd_float3x3 simd_matrix(simd_float3 c0, simd_float3 c1, simd_float3 c2) {
    return {{c0, c1, c2}};
}
inline simd_double3x3 simd_matrix(simd_double3 c0, simd_double3 c1, simd_double3 c2) {
    return {{c0, c1, c2}};
}

// ---- Vector functions --------------------------------------------------------------------------

inline float simd_dot(simd_float3 a, simd_float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float simd_dot(simd_float4 a, simd_float4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
inline double simd_dot(simd_double3 a, simd_double3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline double simd_dot(simd_double4 a, simd_double4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

inline float simd_length_squared(simd_float3 v) {
    return simd_dot(v, v);
}
inline double simd_length_squared(simd_double3 v) {
    return simd_dot(v, v);
}
inline float simd_length(simd_float3 v) {
    return std::sqrt(simd_dot(v, v));
}
inline double simd_length(simd_double3 v) {
    return std::sqrt(simd_dot(v, v));
}
inline double simd_length(simd_double4 v) {
    return std::sqrt(simd_dot(v, v));
}
inline float simd_distance(simd_float3 a, simd_float3 b) {
    return simd_length(a - b);
}
inline double simd_distance(simd_double3 a, simd_double3 b) {
    return simd_length(a - b);
}
inline simd_float3 simd_normalize(simd_float3 v) {
    return v * (1.0f / simd_length(v));
}
inline simd_double3 simd_normalize(simd_double3 v) {
    return v * (1.0 / simd_length(v));
}

inline simd_float3 simd_cross(simd_float3 a, simd_float3 b) {
    return a.yzx * b.zxy - a.zxy * b.yzx;
}
inline simd_double3 simd_cross(simd_double3 a, simd_double3 b) {
    return a.yzx * b.zxy - a.zxy * b.yzx;
}

// Lane-wise minimum and maximum. As in Apple's, a NaN in one lane yields the other operand there.
// Newer clang names these builtins after IEEE minNum and maxNum; older clang has only the originals.
template <typename V> inline V simdLaneMin(V a, V b) {
#if __has_builtin(__builtin_elementwise_minnum)
    return __builtin_elementwise_minnum(a, b);
#else
    return __builtin_elementwise_min(a, b);
#endif
}
template <typename V> inline V simdLaneMax(V a, V b) {
#if __has_builtin(__builtin_elementwise_maxnum)
    return __builtin_elementwise_maxnum(a, b);
#else
    return __builtin_elementwise_max(a, b);
#endif
}

inline simd_float3 simd_min(simd_float3 a, simd_float3 b) {
    return simdLaneMin(a, b);
}
inline simd_double3 simd_min(simd_double3 a, simd_double3 b) {
    return simdLaneMin(a, b);
}
inline simd_float3 simd_max(simd_float3 a, simd_float3 b) {
    return simdLaneMax(a, b);
}
inline simd_double3 simd_max(simd_double3 a, simd_double3 b) {
    return simdLaneMax(a, b);
}

inline simd_float3 simd_clamp(simd_float3 x, simd_float3 lo, simd_float3 hi) {
    return simd_min(simd_max(x, lo), hi);
}
inline simd_float3 simd_clamp(simd_float3 x, float lo, float hi) {
    return simd_clamp(x, simd_float3(lo), simd_float3(hi));
}
inline simd_float3 simd_clamp(simd_float3 x, float lo, simd_float3 hi) {
    return simd_clamp(x, simd_float3(lo), hi);
}
inline simd_double3 simd_clamp(simd_double3 x, simd_double3 lo, simd_double3 hi) {
    return simd_min(simd_max(x, lo), hi);
}
inline simd_double3 simd_clamp(simd_double3 x, double lo, double hi) {
    return simd_clamp(x, simd_double3(lo), simd_double3(hi));
}

inline float simd_reduce_max(simd_float3 v) {
    return __builtin_reduce_max(v);
}
inline double simd_reduce_max(simd_double3 v) {
    return __builtin_reduce_max(v);
}

inline simd_double3 simd_mix(simd_double3 a, simd_double3 b, simd_double3 t) {
    return a + t * (b - a);
}

// True if every lane of a comparison holds.
inline bool simd_all(simd_long3 mask) {
    return mask.x != 0 && mask.y != 0 && mask.z != 0;
}
inline bool simd_all(simd_int3 mask) {
    return mask.x != 0 && mask.y != 0 && mask.z != 0;
}

namespace simd {
inline simd_float3 floor(simd_float3 v) {
    return __builtin_elementwise_floor(v);
}
inline simd_double3 floor(simd_double3 v) {
    return __builtin_elementwise_floor(v);
}
} // namespace simd

// ---- Matrices ----------------------------------------------------------------------------------

inline simd_float4 simd_mul(simd_float4x4 m, simd_float4 v) {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z + m.columns[3] * v.w;
}
inline simd_float4x4 simd_mul(simd_float4x4 a, simd_float4x4 b) {
    return {{simd_mul(a, b.columns[0]), simd_mul(a, b.columns[1]), simd_mul(a, b.columns[2]),
             simd_mul(a, b.columns[3])}};
}
inline simd_double3 simd_mul(simd_double3x3 m, simd_double3 v) {
    return m.columns[0] * v.x + m.columns[1] * v.y + m.columns[2] * v.z;
}
inline simd_double3x3 simd_mul(simd_double3x3 a, simd_double3x3 b) {
    return {{simd_mul(a, b.columns[0]), simd_mul(a, b.columns[1]), simd_mul(a, b.columns[2])}};
}

// The inverse by cofactors: the rows of the inverse are cross products of the columns.
inline simd_double3x3 simd_inverse(simd_double3x3 m) {
    const simd_double3 a = m.columns[0], b = m.columns[1], c = m.columns[2];
    const simd_double3 r0 = simd_cross(b, c), r1 = simd_cross(c, a), r2 = simd_cross(a, b);
    const double inverseDeterminant = 1.0 / simd_dot(a, r0);
    const simd_double3 x = r0 * inverseDeterminant, y = r1 * inverseDeterminant,
                       z = r2 * inverseDeterminant;
    // The transpose of the matrix whose rows are x, y and z.
    return {
        {simd_double3{x.x, y.x, z.x}, simd_double3{x.y, y.y, z.y}, simd_double3{x.z, y.z, z.z}}};
}

// ---- Quaternions -------------------------------------------------------------------------------

inline simd_quatd simd_quaternion(double ix, double iy, double iz, double r) {
    return simd_quatd{simd_double4{ix, iy, iz, r}};
}

inline simd_quatd simd_quaternion(double angle, simd_double3 axis) {
    const simd_double3 imaginary = std::sin(angle / 2) * axis;
    return simd_quaternion(imaginary.x, imaginary.y, imaginary.z, std::cos(angle / 2));
}

// From a rotation matrix, branching on the largest diagonal term for accuracy.
inline simd_quatd simd_quaternion(simd_double3x3 matrix) {
    const simd_double3* m = matrix.columns;
    const double trace = m[0][0] + m[1][1] + m[2][2];
    if (trace >= 0.0) {
        const double r = 2 * std::sqrt(1 + trace), k = 1 / r;
        return simd_quaternion(k * (m[1][2] - m[2][1]), k * (m[2][0] - m[0][2]),
                               k * (m[0][1] - m[1][0]), r / 4);
    }
    if (m[0][0] >= m[1][1] && m[0][0] >= m[2][2]) {
        const double r = 2 * std::sqrt(1 - m[1][1] - m[2][2] + m[0][0]), k = 1 / r;
        return simd_quaternion(r / 4, k * (m[0][1] + m[1][0]), k * (m[0][2] + m[2][0]),
                               k * (m[1][2] - m[2][1]));
    }
    if (m[1][1] >= m[2][2]) {
        const double r = 2 * std::sqrt(1 - m[0][0] - m[2][2] + m[1][1]), k = 1 / r;
        return simd_quaternion(k * (m[0][1] + m[1][0]), r / 4, k * (m[1][2] + m[2][1]),
                               k * (m[2][0] - m[0][2]));
    }
    const double r = 2 * std::sqrt(1 - m[0][0] - m[1][1] + m[2][2]), k = 1 / r;
    return simd_quaternion(k * (m[0][2] + m[2][0]), k * (m[1][2] + m[2][1]), r / 4,
                           k * (m[0][1] - m[1][0]));
}

inline simd_quatd simd_mul(simd_quatd p, simd_quatd q) {
    const simd_double3 pv = p.vector.xyz, qv = q.vector.xyz;
    const double pw = p.vector.w, qw = q.vector.w;
    const simd_double3 v = pw * qv + qw * pv + simd_cross(pv, qv);
    return simd_quaternion(v.x, v.y, v.z, pw * qw - simd_dot(pv, qv));
}

inline double simd_dot(simd_quatd p, simd_quatd q) {
    return simd_dot(p.vector, q.vector);
}
inline double simd_length(simd_quatd q) {
    return simd_length(q.vector);
}

inline simd_quatd simd_normalize(simd_quatd q) {
    return simd_quatd{q.vector * (1.0 / simd_length(q.vector))};
}

// Rotates `v` by `q`.
inline simd_double3 simd_act(simd_quatd q, simd_double3 v) {
    const simd_double3 imaginary = q.vector.xyz;
    const simd_double3 t = 2 * simd_cross(imaginary, v);
    return v + q.vector.w * t + simd_cross(imaginary, t);
}

namespace detail {
inline double sinc(double x) {
    return x == 0 ? 1 : std::sin(x) / x;
}
} // namespace detail

// Spherical interpolation along the shorter arc. Written with sinc, so it stays exact as the two
// orientations approach each other.
inline simd_quatd simd_slerp(simd_quatd q0, simd_quatd q1, double t) {
    if (simd_dot(q0, q1) < 0) {
        q1.vector = -q1.vector;
    }
    const double s = 1 - t;
    const double angle =
        2 * std::atan2(simd_length(q0.vector - q1.vector), simd_length(q0.vector + q1.vector));
    const double r = 1 / detail::sinc(angle);
    const simd_double4 blended =
        detail::sinc(s * angle) * r * s * q0.vector + detail::sinc(t * angle) * r * t * q1.vector;
    return simd_normalize(simd_quatd{blended});
}

#ifdef GEO_SIMD_NAMESPACE
} // namespace GEO_SIMD_NAMESPACE
#endif
