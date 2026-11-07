// The portable vectors, matrices and quaternions must match Apple's simd library, which they stand
// in for off Apple platforms. Both are compiled here, side by side, and compared on random input.
#if defined(__APPLE__) && !defined(GEO_PORTABLE_SIMD)

#include <simd/simd.h>

#define GEO_SIMD_NAMESPACE portable
#include "base/simd_portable.h"

#include <cmath>
#include <doctest/doctest.h>
#include <random>

namespace {

constexpr int kCases = 1'000'000;
constexpr double kTolerance = 1e-12;

struct Random {
    std::mt19937_64 engine{20261107};
    std::uniform_real_distribution<double> uniform{-10.0, 10.0};

    double scalar() { return uniform(engine); }
    simd_double3 vector() { return simd_make_double3(scalar(), scalar(), scalar()); }
    simd_double3 unit() { return simd_normalize(vector()); }
    simd_quatd rotation() { return simd_quaternion(scalar(), unit()); }
};

// The largest difference, relative to the larger magnitude where that exceeds one.
double difference(double a, double b) {
    return std::abs(a - b) / std::max({1.0, std::abs(a), std::abs(b)});
}
double difference(simd_double3 a, simd_double3 b) {
    return std::max({difference(a.x, b.x), difference(a.y, b.y), difference(a.z, b.z)});
}
double difference(simd_double4 a, simd_double4 b) {
    return std::max(difference(a.xyz, b.xyz), difference(a.w, b.w));
}
// Quaternions q and -q are the same rotation.
double difference(simd_quatd a, portable::simd_quatd b) {
    return std::min(difference(a.vector, b.vector), difference(a.vector, -b.vector));
}

simd_double3x3 toApple(portable::simd_double3x3 m) {
    return simd_matrix(m.columns[0], m.columns[1], m.columns[2]);
}

} // namespace

TEST_CASE("portable vector functions match Apple's") {
    Random random;
    double worst = 0;
    for (int i = 0; i < kCases; ++i) {
        const simd_double3 a = random.vector(), b = random.vector();
        worst = std::max({worst, difference(simd_normalize(a), portable::simd_normalize(a)),
                          difference(simd_cross(a, b), portable::simd_cross(a, b)),
                          difference(simd_dot(a, b), portable::simd_dot(a, b)),
                          difference(simd_length(a), portable::simd_length(a)),
                          difference(simd_distance(a, b), portable::simd_distance(a, b)),
                          difference(simd_min(a, b), portable::simd_min(a, b)),
                          difference(simd_max(a, b), portable::simd_max(a, b)),
                          difference(simd_clamp(a, -2.0, 3.0), portable::simd_clamp(a, -2.0, 3.0)),
                          difference(simd_reduce_max(a), portable::simd_reduce_max(a)),
                          difference(simd::floor(a), portable::simd::floor(a)),
                          difference(simd_mix(a, b, simd_double3(0.3)),
                                     portable::simd_mix(a, b, simd_double3(0.3)))});
    }
    CHECK(worst <= kTolerance);
}

TEST_CASE("portable matrices match Apple's") {
    Random random;
    double worst = 0;
    for (int i = 0; i < kCases; ++i) {
        const simd_double3 c0 = random.vector(), c1 = random.vector(), c2 = random.vector();
        const simd_double3x3 apple = simd_matrix(c0, c1, c2);
        const portable::simd_double3x3 ours = portable::simd_matrix(c0, c1, c2);
        if (std::abs(simd_determinant(apple)) < 1e-3) {
            continue; // nearly singular: both inverses are meaningless
        }
        const simd_double3 v = random.vector();
        const simd_double3x3 inverse = toApple(portable::simd_inverse(ours));
        const simd_double3x3 product = toApple(portable::simd_mul(ours, ours));
        for (int c = 0; c < 3; ++c) {
            worst = std::max({worst,
                              difference(simd_inverse(apple).columns[c], inverse.columns[c]) /
                                  std::max(1.0, simd_length(simd_inverse(apple).columns[c])),
                              difference(simd_mul(apple, apple).columns[c], product.columns[c])});
        }
        worst = std::max(worst, difference(simd_mul(apple, v), portable::simd_mul(ours, v)));
    }
    CHECK(worst <= 1e-9); // an inverse loses accuracy with the condition number
}

TEST_CASE("portable quaternions match Apple's") {
    Random random;
    double worst = 0;
    for (int i = 0; i < kCases; ++i) {
        const double angle = random.scalar();
        const simd_double3 axis = random.unit(), v = random.vector();
        const simd_quatd p = simd_quaternion(angle, axis), q = random.rotation();
        const portable::simd_quatd ourP = portable::simd_quaternion(angle, axis);
        const portable::simd_quatd ourQ{q.vector};
        worst = std::max(
            {worst, difference(p, ourP), difference(simd_mul(p, q), portable::simd_mul(ourP, ourQ)),
             difference(simd_act(p, v), portable::simd_act(ourP, v)),
             difference(simd_normalize(p), portable::simd_normalize(ourP)),
             difference(simd_slerp(p, q, 0.37), portable::simd_slerp(ourP, ourQ, 0.37))});
        // From a rotation matrix, as Camera::lookAt makes them.
        const simd_double3 x = simd_act(q, simd_make_double3(1, 0, 0));
        const simd_double3 y = simd_act(q, simd_make_double3(0, 1, 0));
        const simd_double3 z = simd_act(q, simd_make_double3(0, 0, 1));
        worst =
            std::max(worst, difference(simd_quaternion(simd_matrix(x, y, z)),
                                       portable::simd_quaternion(portable::simd_matrix(x, y, z))));
    }
    // Interpolating nearly equal orientations stays exact.
    const simd_quatd q = random.rotation();
    const simd_quatd near = simd_normalize(simd_quaternion(q.vector + 1e-9));
    worst =
        std::max(worst, difference(simd_slerp(q, near, 0.5),
                                   portable::simd_slerp(portable::simd_quatd{q.vector},
                                                        portable::simd_quatd{near.vector}, 0.5)));
    CHECK(worst <= kTolerance);
}

TEST_CASE("portable float vectors match Apple's") {
    Random random;
    float worst = 0;
    for (int i = 0; i < kCases; ++i) {
        const simd_float3 a = simd_make_float3(float(random.scalar()), float(random.scalar()),
                                               float(random.scalar()));
        const simd_float3 b = simd_make_float3(float(random.scalar()), float(random.scalar()),
                                               float(random.scalar()));
        const simd_float3 n = simd_normalize(a) - portable::simd_normalize(a);
        const simd_float3 c = simd_cross(a, b) - portable::simd_cross(a, b);
        const simd_float3 k = simd_clamp(a, 0.0f, 1.0f) - portable::simd_clamp(a, 0.0f, 1.0f);
        worst = std::max({worst, simd_reduce_max(simd_abs(n)), simd_reduce_max(simd_abs(c)) / 100,
                          simd_reduce_max(simd_abs(k))});
    }
    CHECK(worst <= 1e-6f);
}

#endif
