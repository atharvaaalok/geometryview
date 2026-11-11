#include "engine/png.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace geo::engine {

bool writePng(const char* path, const std::vector<u8>& bgra, u32 width, u32 height, Alpha alpha) {
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGImageAlphaInfo info = alpha == Alpha::Premultiplied ? kCGImageAlphaPremultipliedFirst
                                                                : kCGImageAlphaNoneSkipFirst;
    CGContextRef context =
        CGBitmapContextCreate(const_cast<u8*>(bgra.data()), width, height, 8, usize(width) * 4,
                              colorSpace, kCGBitmapByteOrder32Little | u32(info));
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
