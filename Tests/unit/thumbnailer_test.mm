// The gallery's thumbnailer: one renderer kept for many files draws each as a renderer of its own
// would, holds nothing once trimmed, and finds the part's box for layout.
#include "drive_fixture.h"
#include "engine.h"
#include "hidden_box_fixture.h"
#include "render/style.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <doctest/doctest.h>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(GEO_TEST_WORKER_PATH) && defined(GEO_TEST_SHADER_LIBRARY)

using namespace geo;

namespace {

GeoRenderOptions pictureOptions(GeoStyle style) {
    GeoRenderOptions options = geo_default_render_options();
    options.width = 256;
    options.height = 256;
    options.overlay = false;
    options.elevation_degrees = 30.0;
    options.look.style = style;
    options.look.cast_shadow = geo_style_casts_shadow(style);
    options.line_scale = 0.4f;
    options.transparent = true;
    return options;
}

std::vector<u8> pixelsOf(const GeoThumbnail& picture) {
    return {picture.bgra, picture.bgra + usize(picture.width) * picture.height * 4};
}

GeoThumbnail draw(GeoThumbnailer* thumbnailer, const std::string& blob,
                  const GeoRenderOptions& options) {
    GeoThumbnail picture{};
    char error[256] = {};
    const bool rendered =
        geo_thumbnailer_render(thumbnailer, blob.c_str(), &options, &picture, error, sizeof(error));
    REQUIRE_MESSAGE(rendered, error);
    return picture;
}

} // namespace

TEST_CASE("a thumbnailer kept for many files draws each as a fresh renderer would") {
    @autoreleasepool {
        const std::string drive = geo::test::importDrive(GEO_TEST_WORKER_PATH);
        const std::string box = geo::test::importHiddenBox(GEO_TEST_WORKER_PATH);
        GeoThumbnailer* kept = geo_thumbnailer_create(GEO_TEST_SHADER_LIBRARY);
        REQUIRE(kept != nullptr);
        for (u32 i = 0; i < 20; ++i) {
            CAPTURE(i);
            const std::string& blob = i % 3 == 2 ? box : drive;
            GeoRenderOptions options = pictureOptions(GeoStyle(i % geo::render::kStyleCount));
            options.first_color = int32_t(i % 4);
            GeoThumbnail reused = draw(kept, blob, options);
            GeoThumbnailer* fresh = geo_thumbnailer_create(GEO_TEST_SHADER_LIBRARY);
            REQUIRE(fresh != nullptr);
            GeoThumbnail alone = draw(fresh, blob, options);
            geo_thumbnailer_destroy(fresh);
            CHECK(pixelsOf(reused) == pixelsOf(alone));
            CHECK(reused.world_per_pixel == alone.world_per_pixel);
            CHECK(std::equal(reused.part_rect, reused.part_rect + 4, alone.part_rect));
            geo_thumbnail_free(&reused);
            geo_thumbnail_free(&alone);
        }

        // Trimmed, it holds no file, and draws the next as before.
        geo_thumbnailer_trim(kept);
        GeoThumbnail after = draw(kept, drive, pictureOptions(GeoStyleMetal));
        CHECK(after.bgra != nullptr);
        CHECK(after.part_rect[2] > 0);
        geo_thumbnail_free(&after);
        CHECK(after.bgra == nullptr);
        geo_thumbnailer_destroy(kept);
        unlink(drive.c_str());
        unlink(box.c_str());
    }
}

TEST_CASE("a picture knows the part's box, its shadow's, and the scale it was drawn at") {
    @autoreleasepool {
        const std::string drive = geo::test::importDrive(GEO_TEST_WORKER_PATH);
        GeoThumbnailer* thumbnailer = geo_thumbnailer_create(GEO_TEST_SHADER_LIBRARY);
        REQUIRE(thumbnailer != nullptr);

        // Metal's soft shadow spills below the part; the part's box leaves it out.
        GeoThumbnail metal = draw(thumbnailer, drive, pictureOptions(GeoStyleMetal));
        CHECK(metal.premultiplied);
        const uint32_t* part = metal.part_rect;
        const uint32_t* ink = metal.ink_rect;
        CHECK(part[2] > 100);
        CHECK(ink[0] <= part[0]);
        CHECK(ink[1] <= part[1]);
        CHECK(ink[1] + ink[3] > part[1] + part[3]); // the shadow below
        CHECK(ink[0] + ink[2] <= metal.width);
        // Nothing drawn outside the ink's box.
        const std::vector<u8> pixels = pixelsOf(metal);
        CHECK(pixels[(usize(ink[1]) * metal.width + ink[0] + ink[2]) * 4 - 1] == 0);

        // Fitted, the drive's 40 mm housing spans most of the picture; at a fixed scale of 0.5 mm a
        // pixel it is drawn smaller, as the same scale draws any other file.
        CHECK(metal.world_per_pixel > 0.0);
        GeoRenderOptions fixed = pictureOptions(GeoStyleMetal);
        fixed.world_per_pixel = metal.world_per_pixel * 2.0;
        GeoThumbnail half = draw(thumbnailer, drive, fixed);
        CHECK(half.world_per_pixel == doctest::Approx(fixed.world_per_pixel));
        CHECK(double(half.part_rect[2]) == doctest::Approx(part[2] * 0.5).epsilon(0.05));

        // On an opaque sheet every pixel is drawn.
        GeoRenderOptions opaque = pictureOptions(GeoStyleMetal);
        opaque.transparent = false;
        GeoThumbnail sheet = draw(thumbnailer, drive, opaque);
        CHECK_FALSE(sheet.premultiplied);
        CHECK(sheet.ink_rect[2] == 256);
        CHECK(sheet.ink_rect[3] == 256);

        geo_thumbnail_free(&metal);
        geo_thumbnail_free(&half);
        geo_thumbnail_free(&sheet);
        geo_thumbnailer_destroy(thumbnailer);
        unlink(drive.c_str());
    }
}

TEST_CASE("supersampled, a picture is the same picture with smoother edges") {
    @autoreleasepool {
        const std::string drive = geo::test::importDrive(GEO_TEST_WORKER_PATH);
        GeoThumbnailer* thumbnailer = geo_thumbnailer_create(GEO_TEST_SHADER_LIBRARY);
        REQUIRE(thumbnailer != nullptr);
        GeoRenderOptions options = pictureOptions(GeoStyleIllustration);
        GeoThumbnail plain = draw(thumbnailer, drive, options);
        options.supersample = 2;
        GeoThumbnail smooth = draw(thumbnailer, drive, options);
        CHECK(smooth.width == plain.width);
        CHECK(smooth.height == plain.height);
        CHECK(smooth.world_per_pixel == doctest::Approx(plain.world_per_pixel).epsilon(0.02));
        for (int i = 0; i < 4; ++i) {
            CAPTURE(i);
            CHECK(std::abs(int(smooth.part_rect[i]) - int(plain.part_rect[i])) <= 2);
        }
        // Nearer the picture as it would be with no stairs at all, drawn four times as large and
        // averaged: outlines and the shadow's edge, where the plain picture steps, are what differ.
        options.supersample = 4;
        GeoThumbnail truth = draw(thumbnailer, drive, options);
        const auto off = [&](const GeoThumbnail& picture) {
            double sum = 0;
            const usize n = usize(picture.width) * picture.height * 4;
            for (usize p = 0; p < n; ++p) {
                sum += std::abs(int(picture.bgra[p]) - int(truth.bgra[p]));
            }
            return sum / double(n);
        };
        const double plainOff = off(plain), smoothOff = off(smooth);
        CAPTURE(plainOff);
        CAPTURE(smoothOff);
        CHECK(smoothOff < plainOff * 0.7);
        geo_thumbnail_free(&plain);
        geo_thumbnail_free(&smooth);
        geo_thumbnail_free(&truth);
        geo_thumbnailer_destroy(thumbnailer);
        unlink(drive.c_str());
    }
}

TEST_CASE("a file that cannot be drawn says why and leaves the picture empty") {
    @autoreleasepool {
        GeoThumbnailer* thumbnailer = geo_thumbnailer_create(GEO_TEST_SHADER_LIBRARY);
        REQUIRE(thumbnailer != nullptr);
        GeoThumbnail picture{};
        char error[256] = {};
        const GeoRenderOptions options = pictureOptions(GeoStyleIllustration);
        CHECK_FALSE(geo_thumbnailer_render(thumbnailer, "/nonexistent.geoblob", &options, &picture,
                                           error, sizeof(error)));
        CHECK(std::strstr(error, "Could not open") != nullptr);
        CHECK(picture.bgra == nullptr);
        geo_thumbnailer_destroy(thumbnailer);
    }
}

// Not a check: how long a picture takes, kept and fresh, to compare against the gallery's budget
// of 6 ms a picture, and supersampled as the gallery draws them. Printed with -s.
TEST_CASE("timing: pictures from a kept thumbnailer and from fresh ones") {
    @autoreleasepool {
        const std::string drive = geo::test::importDrive(GEO_TEST_WORKER_PATH);
        const auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        using Clock = std::chrono::steady_clock;
        const auto ms = [](Clock::duration d) {
            return std::chrono::duration<double, std::milli>(d).count();
        };
        for (const auto [size, supersample] : {std::pair{256u, 1u}, {512u, 1u}, {512u, 2u}}) {
            GeoThumbnailer* kept = geo_thumbnailer_create(GEO_TEST_SHADER_LIBRARY);
            REQUIRE(kept != nullptr);
            std::vector<double> warm, fresh;
            for (u32 i = 0; i < 24; ++i) {
                GeoRenderOptions options = pictureOptions(GeoStyle(i % geo::render::kStyleCount));
                options.width = options.height = size;
                options.supersample = supersample;
                const Clock::time_point start = Clock::now();
                GeoThumbnail picture = draw(kept, drive, options);
                warm.push_back(ms(Clock::now() - start));
                geo_thumbnail_free(&picture);
            }
            for (u32 i = 0; i < 6; ++i) {
                GeoRenderOptions options = pictureOptions(GeoStyle(i % geo::render::kStyleCount));
                options.width = options.height = size;
                options.supersample = supersample;
                const Clock::time_point start = Clock::now();
                GeoThumbnailer* once = geo_thumbnailer_create(GEO_TEST_SHADER_LIBRARY);
                GeoThumbnail picture = draw(once, drive, options);
                geo_thumbnailer_destroy(once);
                fresh.push_back(ms(Clock::now() - start));
                geo_thumbnail_free(&picture);
            }
            geo_thumbnailer_destroy(kept);
            warm.erase(warm.begin(), warm.begin() + 6); // each style's first picture compiles
            MESSAGE(size << " px, supersample " << supersample << ": kept " << median(warm)
                         << " ms, fresh " << median(fresh) << " ms (median)");
        }
        unlink(drive.c_str());
    }
}

#endif
