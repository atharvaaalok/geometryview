// Headless rendering, for tests and `gcli render`.
#include "engine.h"

#include "base/types.h"
#include "gpu/renderer.h"
#include "nav/camera.h"
#include "render/view.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

using namespace geo;

constexpr double kFitMargin = 1.12;

bool writePng(const char* path, const std::vector<u8>& bgra, u32 width, u32 height) {
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        const_cast<u8*>(bgra.data()), width, height, 8, usize(width) * 4, colorSpace,
        kCGBitmapByteOrder32Little | u32(kCGImageAlphaNoneSkipFirst));
    CGImageRef image = CGBitmapContextCreateImage(context);
    NSURL* url = [NSURL fileURLWithPath:@(path)];
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    bool ok = false;
    if (destination != nullptr) {
        CGImageDestinationAddImage(destination, image, nullptr);
        ok = CGImageDestinationFinalize(destination);
        CFRelease(destination);
    }
    CGImageRelease(image);
    CGContextRelease(context);
    CGColorSpaceRelease(colorSpace);
    return ok;
}

} // namespace

bool geo_render_blob_to_png(const char* shader_library_path, const char* blob_path,
                            const char* png_path, uint32_t width, uint32_t height,
                            double azimuth_degrees, double elevation_degrees, bool perspective) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        std::unique_ptr<gpu::Renderer> renderer =
            device ? gpu::Renderer::create(device, shader_library_path) : nullptr;
        if (!renderer) {
            return false;
        }

        const int fd = open(blob_path, O_RDONLY);
        if (fd < 0) {
            std::fprintf(stderr, "Could not open %s.\n", blob_path);
            return false;
        }
        scene::MappedBlob blob;
        std::string error;
        const bool opened = blob.open(fd, error);
        close(fd);
        if (!opened) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return false;
        }
        renderer->setScene(blob);
        renderer->resize(width, height);

        const nav::Viewport viewport{double(width), double(height)};
        const Aabb bounds = renderer->sceneBounds();
        const double azimuth = azimuth_degrees * M_PI / 180.0;
        const double elevation = elevation_degrees * M_PI / 180.0;
        const Vec3 direction =
            simd_make_double3(std::cos(elevation) * std::sin(azimuth),
                              -std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
        nav::Camera camera;
        camera.perspective = perspective;
        camera.lookAt(bounds.center() + direction, bounds.center(),
                      simd_make_double3(0.0, 0.0, 1.0));
        nav::fit(camera, bounds, viewport, kFitMargin);

        const std::vector<u8> pixels =
            renderer->renderOffscreen(render::makeView(camera, viewport, bounds));
        if (!writePng(png_path, pixels, width, height)) {
            std::fprintf(stderr, "Could not write %s.\n", png_path);
            return false;
        }
    }
    return true;
}
