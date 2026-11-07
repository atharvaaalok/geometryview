#include "engine/png.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace geo::engine {

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

} // namespace geo::engine
