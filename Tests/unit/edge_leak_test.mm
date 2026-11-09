// Edges hidden behind a face stay hidden, in Metal: see hidden_box_fixture.h. Needs the OCCT worker,
// since the assembly is STEP. webgpu_test.mm checks the WebGPU renderer the same way.

#if defined(GEO_TEST_WORKER_PATH)

#include "engine.h"
#include "hidden_box_fixture.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <doctest/doctest.h>
#include <filesystem>
#include <unistd.h>
#include <vector>

using namespace geo;
namespace fs = std::filesystem;

namespace {

// Luminance of each pixel of a PNG, or nothing if it cannot be read.
std::vector<u8> readGray(const std::string& path, u32& width, u32& height) {
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
    std::vector<u8> gray(usize(width) * height);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceGray();
    CGContextRef context = CGBitmapContextCreate(gray.data(), width, height, 8, width, space,
                                                 u32(kCGImageAlphaNone));
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    return gray;
}

} // namespace

TEST_CASE("a part's edges behind another's face stay hidden, in an assembly with a coarse tolerance") {
    @autoreleasepool {
        const std::string blob = test::importHiddenBox(GEO_TEST_WORKER_PATH);
        const std::string png = (fs::temp_directory_path() / "edge-leak.png").string();
        const GeoRenderOptions options = test::hiddenBoxView();
        REQUIRE(geo_render_blob_to_png(GEO_TEST_SHADER_LIBRARY, blob.c_str(), png.c_str(), &options));
        u32 width = 0, height = 0;
        const std::vector<u8> gray = readGray(png, width, height);
        REQUIRE(!gray.empty());
        test::checkNothingShowsThrough(gray, width, height);
        fs::remove(png);
        unlink(blob.c_str());
    }
}

#endif
