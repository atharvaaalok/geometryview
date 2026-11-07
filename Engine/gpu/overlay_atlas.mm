#include "gpu/overlay_atlas.h"

#include "base/types.h"
#include "render/overlay.h"
#include "shaders/shared/gpu_types.h"

#import <CoreText/CoreText.h>
#include <vector>

namespace geo::gpu {

bool makeOverlayAtlas(id<MTLDevice> device, OverlayAtlas& atlas) {
    std::vector<u8> pixels(usize(kAtlasWidth) * kAtlasHeight, 0);
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef context = CGBitmapContextCreate(pixels.data(), kAtlasWidth, kAtlasHeight, 8,
                                                 kAtlasWidth, gray, u32(kCGImageAlphaNone));
    CGColorSpaceRelease(gray);
    if (context == nullptr) {
        return false;
    }
    CGContextSetGrayFillColor(context, 1.0, 1.0);
    CGContextSetShouldSmoothFonts(context, false);

    const auto makeFont = [](CGFloat size) {
        CTFontRef font = CTFontCreateWithName(CFSTR("IBMPlexMono-SmBld"), size, nullptr);
        NSString* name = (__bridge_transfer NSString*)CTFontCopyPostScriptName(font);
        if (![name isEqualToString:@"IBMPlexMono-SmBld"]) {
            CFRelease(font);
            font = CTFontCreateWithName(CFSTR("Menlo-Bold"), size, nullptr);
        }
        return font;
    };
    // Draws `text` centred in a cell whose top is `top` pixels below the atlas's top.
    const auto draw = [&](NSString* text, CTFontRef font, CGFloat left, CGFloat width, CGFloat top,
                          CGFloat height) {
        NSDictionary* attributes = @{
            (__bridge id)kCTFontAttributeName : (__bridge id)font,
            (__bridge id)kCTForegroundColorFromContextAttributeName : @YES,
        };
        NSAttributedString* string = [[NSAttributedString alloc] initWithString:text
                                                                     attributes:attributes];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)string);
        const CGFloat ascent = CTFontGetCapHeight(font);
        const CGFloat x =
            left + (width - CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr)) / 2.0;
        // Core Graphics counts up from the bottom, and the cell's middle is cap-centred.
        const CGFloat y = CGFloat(kAtlasHeight) - top - (height + ascent) / 2.0;
        CGContextSetTextPosition(context, x, y);
        CTLineDraw(line, context);
        CFRelease(line);
    };

    CTFontRef labelFont = makeFont(23);
    NSArray<NSString*>* labels = @[ @"FRONT", @"BACK", @"LEFT", @"RIGHT", @"TOP", @"BOTTOM" ];
    for (NSUInteger face = 0; face < labels.count; ++face) {
        draw(labels[face], labelFont, 0, CGFloat(kAtlasLabelWidth),
             CGFloat(face) * CGFloat(kAtlasLabelHeight), CGFloat(kAtlasLabelHeight));
    }
    CFRelease(labelFont);

    // The glyph strip, drawn for a 2x display: 11 points is 22 pixels.
    CTFontRef glyphFont = makeFont(22);
    const UniChar zero = '0';
    CGGlyph glyph = 0;
    CTFontGetGlyphsForCharacters(glyphFont, &zero, &glyph, 1);
    CGSize advance{};
    CTFontGetAdvancesForGlyphs(glyphFont, kCTFontOrientationHorizontal, &glyph, &advance, 1);
    atlas.glyphAdvance = float(advance.width);
    for (usize cell = 0; cell < render::kAtlasGlyphs.size(); ++cell) {
        const char c = render::kAtlasGlyphs[cell];
        NSString* text = c == 'u' ? @"\u00B5" : [NSString stringWithFormat:@"%c", c];
        draw(text, glyphFont, CGFloat(cell * kAtlasGlyphWidth), CGFloat(kAtlasGlyphWidth),
             6.0 * CGFloat(kAtlasLabelHeight), CGFloat(kAtlasGlyphHeight));
    }
    CFRelease(glyphFont);
    CGContextRelease(context);

    MTLTextureDescriptor* d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                                           width:kAtlasWidth
                                                          height:kAtlasHeight
                                                       mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    atlas.texture = [device newTextureWithDescriptor:d];
    [atlas.texture replaceRegion:MTLRegionMake2D(0, 0, kAtlasWidth, kAtlasHeight)
                     mipmapLevel:0
                       withBytes:pixels.data()
                     bytesPerRow:kAtlasWidth];
    return atlas.texture != nil;
}

} // namespace geo::gpu
