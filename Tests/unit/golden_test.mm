// Renders a small assembly in every style and colour mode and compares each image with a golden one
// in Tests/golden. Set GEOMETRYVIEW_UPDATE_GOLDENS=1 to write the goldens instead, after checking
// the new images by eye. Needs the OCCT worker, since the assembly is STEP.
#include "engine.h"
#include "render/style.h"
#include "step/step_import.h"
#include "drive_fixture.h"
#include "step_fixture.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <cstdlib>
#include <doctest/doctest.h>
#include <filesystem>
#include <format>
#include <unistd.h>
#include <vector>

using namespace geo;
namespace fs = std::filesystem;

#if defined(GEO_TEST_WORKER_PATH) && defined(GEO_TEST_GOLDEN_DIR)

namespace {

constexpr u32 kWidth = 480;
constexpr u32 kHeight = 300;

// BGRA pixels of a PNG, or nothing if it cannot be read.
std::vector<u8> readPng(const std::string& path, u32& width, u32& height) {
    NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (source == nullptr) {
        return {};
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (image == nullptr) {
        return {};
    }
    width = u32(CGImageGetWidth(image));
    height = u32(CGImageGetHeight(image));
    std::vector<u8> pixels(usize(width) * height * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context =
        CGBitmapContextCreate(pixels.data(), width, height, 8, usize(width) * 4, space,
                              kCGBitmapByteOrder32Little | u32(kCGImageAlphaNoneSkipFirst));
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    return pixels;
}

struct Difference {
    double mean = 0.0;    // mean absolute channel difference
    double changed = 0.0; // share of pixels with a channel more than 12 levels off
};

Difference compare(const std::vector<u8>& a, const std::vector<u8>& b) {
    Difference d;
    const usize pixels = a.size() / 4;
    u64 total = 0, changed = 0;
    for (usize p = 0; p < pixels; ++p) {
        int worst = 0;
        for (usize c = 0; c < 3; ++c) {
            const int delta = std::abs(int(a[p * 4 + c]) - int(b[p * 4 + c]));
            total += u64(delta);
            worst = std::max(worst, delta);
        }
        changed += worst > 12 ? 1 : 0;
    }
    d.mean = double(total) / double(pixels * 3);
    d.changed = double(changed) / double(pixels);
    return d;
}

// Renders `options` and compares it with the golden image `name`, or replaces the golden.
void checkGolden(const std::string& blob, const std::string& name,
                 const GeoRenderOptions& options) {
    CAPTURE(name);
    const fs::path goldens = GEO_TEST_GOLDEN_DIR;
    const std::string rendered = (fs::temp_directory_path() / name).string();
    REQUIRE(
        geo_render_blob_to_png(GEO_TEST_SHADER_LIBRARY, blob.c_str(), rendered.c_str(), &options));
    if (std::getenv("GEOMETRYVIEW_UPDATE_GOLDENS") != nullptr) {
        fs::copy_file(rendered, goldens / name, fs::copy_options::overwrite_existing);
        return;
    }
    u32 w = 0, h = 0, gw = 0, gh = 0;
    const std::vector<u8> image = readPng(rendered, w, h);
    const std::vector<u8> golden = readPng((goldens / name).string(), gw, gh);
    REQUIRE_MESSAGE(!golden.empty(), "no golden image; run with GEOMETRYVIEW_UPDATE_GOLDENS");
    REQUIRE(w == gw);
    REQUIRE(h == gh);
    const Difference d = compare(image, golden);
    CAPTURE(d.mean);
    CAPTURE(d.changed);
    CHECK(d.mean < 1.0);
    CHECK(d.changed < 0.005);
    fs::remove(rendered);
}

} // namespace

TEST_CASE("every style and colour mode matches its golden image") {
    @autoreleasepool {
        const std::string blob = test::importDrive(GEO_TEST_WORKER_PATH);
        const char* styles[] = {"illustration", "metal",     "clay",
                                "blueprint",    "technical", "studio"};
        static_assert(std::size(styles) == render::kStyleCount);
        const char* modes[] = {"material", "parts", "mono", "file"};
        for (u32 style = 0; style < render::kStyleCount; ++style) {
            // Blueprint and Technical draw every part alike, so one mode covers them.
            const u32 modeCount = render::styleNamed(render::StyleName(style)).colorModes ? 4 : 1;
            for (u32 mode = 0; mode < modeCount; ++mode) {
                const std::string name = std::format("{}-{}.png", styles[style], modes[mode]);
                GeoRenderOptions options = geo_default_render_options();
                options.width = kWidth;
                options.height = kHeight;
                options.overlay = false;
                options.look.style = GeoStyle(style);
                options.look.cast_shadow = geo_style_casts_shadow(options.look.style);
                options.look.color_mode = GeoColorMode(mode);
                checkGolden(blob, name, options);
            }
        }
        unlink(blob.c_str());
    }
}

// Cut across x and through its parts, where they stand on the housing and touch each other: the
// cut faces, their hatching and the outlines between them.
TEST_CASE("a section through the assembly matches its golden image") {
    @autoreleasepool {
        const std::string blob = test::importDrive(GEO_TEST_WORKER_PATH);
        for (const GeoStyle style : {GeoStyleIllustration, GeoStyleMetal}) {
            GeoRenderOptions options = geo_default_render_options();
            options.width = kWidth;
            options.height = kHeight;
            options.overlay = false;
            options.azimuth_degrees = 62.0;
            options.elevation_degrees = 30.0;
            options.look.style = style;
            options.look.cast_shadow = geo_style_casts_shadow(style);
            options.section.enabled[0] = true;
            options.section.position[0] = 0.45f;
            checkGolden(blob,
                        std::format("section-{}.png",
                                    style == GeoStyleIllustration ? "illustration" : "metal"),
                        options);
        }
        unlink(blob.c_str());
    }
}

#endif
