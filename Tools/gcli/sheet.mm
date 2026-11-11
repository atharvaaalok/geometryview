// gcli sheet: many cache files drawn on one sheet as the gallery draws them, each part standing on
// its cell's baseline over its name. For MCP's view_folder and for checking pictures by eye.
//
//   gcli sheet <metallib> <out.png> [--cell 192] [--columns N] [--style S] [--colour C]
//              [--line-scale X] [--scale 2] [--same-scale] [--opaque] [--font regular.ttf]
//              [--title text]
//
// Reads one JSON object a line from stdin: {"blob": path, "label": text, "sub": text,
// "first_color": n}, only blob required. Writes one JSON line a tile to stdout: its index, its
// cell in points, the scale it was drawn at in model units per point, and any error.
#include "sheet.h"

#include "base/types.h"
#include "engine.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace geo::gcli {
namespace {

// ---- One flat JSON object a line ----------------------------------------------------------

struct Tile {
    std::string blob, label, sub;
    int firstColor = -1;
};

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    bool object(Tile& tile, std::string& error) {
        if (!take('{')) {
            return fail(error, "expected an object");
        }
        if (take('}')) {
            return true;
        }
        do {
            std::string key, text;
            if (!string(key) || !take(':')) {
                return fail(error, "expected a member");
            }
            space();
            if (peek() == '"') {
                if (!string(text)) {
                    return fail(error, "bad string");
                }
                if (key == "blob") {
                    tile.blob = text;
                } else if (key == "label") {
                    tile.label = text;
                } else if (key == "sub") {
                    tile.sub = text;
                }
            } else {
                const usize start = at_;
                while (at_ < text_.size() && text_[at_] != ',' && text_[at_] != '}') {
                    ++at_;
                }
                const std::string value(text_.substr(start, at_ - start));
                if (key == "first_color") {
                    tile.firstColor = std::atoi(value.c_str());
                }
            }
        } while (take(','));
        return take('}') ? true : fail(error, "expected the object's end");
    }

private:
    static bool fail(std::string& error, const char* what) {
        error = what;
        return false;
    }
    void space() {
        while (at_ < text_.size() && std::isspace(u8(text_[at_]))) {
            ++at_;
        }
    }
    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }
    bool take(char c) {
        space();
        if (peek() != c) {
            return false;
        }
        ++at_;
        return true;
    }
    static void append(std::string& out, u32 code) {
        if (code < 0x80) {
            out += char(code);
        } else if (code < 0x800) {
            out += char(0xC0 | (code >> 6));
            out += char(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += char(0xE0 | (code >> 12));
            out += char(0x80 | ((code >> 6) & 0x3F));
            out += char(0x80 | (code & 0x3F));
        } else {
            out += char(0xF0 | (code >> 18));
            out += char(0x80 | ((code >> 12) & 0x3F));
            out += char(0x80 | ((code >> 6) & 0x3F));
            out += char(0x80 | (code & 0x3F));
        }
    }
    bool hex4(u32& code) {
        if (at_ + 4 > text_.size()) {
            return false;
        }
        code = u32(std::strtoul(std::string(text_.substr(at_, 4)).c_str(), nullptr, 16));
        at_ += 4;
        return true;
    }
    bool string(std::string& out) {
        if (!take('"')) {
            return false;
        }
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            const char e = peek();
            ++at_;
            switch (e) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'u': {
                u32 code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code < 0xDC00 && peek() == '\\') {
                    u32 low = 0;
                    at_ += 2; // \u
                    if (!hex4(low)) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append(out, code);
                break;
            }
            default:
                out += e;
                break; // " \ /
            }
        }
        return false;
    }

    std::string_view text_;
    usize at_ = 0;
};

void printJson(std::string_view text) {
    std::fputc('"', stdout);
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            std::fputc('\\', stdout);
            std::fputc(c, stdout);
        } else if (u8(c) < 0x20) {
            std::printf("\\u%04x", unsigned(c));
        } else {
            std::fputc(c, stdout);
        }
    }
    std::fputc('"', stdout);
}

// ---- Drawing ------------------------------------------------------------------------------

// The gallery's colours on each style's sheet: text, muted text.
struct Inks {
    CGFloat text[3], muted[3];
};

Inks inksOf(GeoStyle style) {
    switch (style) {
    case GeoStyleBlueprint:
        return {{0xEE / 255.0, 0xF4 / 255.0, 1.0}, {0xA9 / 255.0, 0xBE / 255.0, 0xDF / 255.0}};
    case GeoStyleStudio:
        return {{0xEC / 255.0, 0xEA / 255.0, 0xE4 / 255.0},
                {0xA8 / 255.0, 0xA5 / 255.0, 0x9C / 255.0}};
    default:
        return {{0x23 / 255.0, 0x1E / 255.0, 0x16 / 255.0},
                {0x6F / 255.0, 0x68 / 255.0, 0x5C / 255.0}};
    }
}

// The style's sheet: its wash, vignette and drafting grid, as the engine draws them.
void drawSheet(CGContextRef context, GeoStyle style, CGFloat width, CGFloat height, CGFloat scale) {
    const GeoSheet sheet = geo_style_sheet(style);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat stops[] = {sheet.top[0],    sheet.top[1],    sheet.top[2],    1.0,
                             sheet.bottom[0], sheet.bottom[1], sheet.bottom[2], 1.0};
    CGGradientRef wash = CGGradientCreateWithColorComponents(space, stops, nullptr, 2);
    // CoreGraphics' y runs up: the top colour at the top.
    CGContextDrawLinearGradient(context, wash, CGPointMake(0, height), CGPointZero, 0);
    CGGradientRelease(wash);
    if (sheet.vignette > 0.0f) {
        const CGFloat dark[] = {0, 0, 0, 0, 0, 0, 0, sheet.vignette};
        const CGFloat at[] = {0.3, 1.0};
        CGGradientRef vignette = CGGradientCreateWithColorComponents(space, dark, at, 2);
        const CGPoint middle = CGPointMake(width / 2, height / 2);
        CGContextDrawRadialGradient(context, vignette, middle, 0, middle, height,
                                    kCGGradientDrawsAfterEndLocation);
        CGGradientRelease(vignette);
    }
    if (sheet.grid_alpha > 0.0f && sheet.grid_pitch > 0.0f) {
        const CGFloat pitch = sheet.grid_pitch * scale;
        for (int major = 0; major < 2; ++major) {
            CGContextSetRGBStrokeColor(context, sheet.grid[0], sheet.grid[1], sheet.grid[2],
                                       sheet.grid_alpha * (major ? 1.6 : 1.0));
            CGContextSetLineWidth(context, 1.0);
            const CGFloat step = major ? pitch * 5 : pitch;
            for (CGFloat x = step; x < width; x += step) {
                const CGPoint line[] = {{std::round(x) + 0.5, 0}, {std::round(x) + 0.5, height}};
                CGContextStrokeLineSegments(context, line, 2);
            }
            for (CGFloat y = step; y < height; y += step) {
                const CGPoint line[] = {{0, height - std::round(y) - 0.5},
                                        {width, height - std::round(y) - 0.5}};
                CGContextStrokeLineSegments(context, line, 2);
            }
        }
    }
    CGColorSpaceRelease(space);
}

// One line of text, its baseline at `baseline` from the top, centred on `middle` and cut to
// `width`, all in pixels.
void drawText(CGContextRef context, CTFontRef font, const CGFloat rgb[3], const std::string& text,
              CGFloat middle, CGFloat baseline, CGFloat width, CGFloat canvasHeight) {
    if (text.empty()) {
        return;
    }
    CGColorRef color = CGColorCreateSRGB(rgb[0], rgb[1], rgb[2], 1.0);
    NSString* string = [NSString stringWithUTF8String:text.c_str()];
    if (string == nil) {
        string = @"";
    }
    NSDictionary* attributes = @{
        (__bridge id)kCTFontAttributeName : (__bridge id)font,
        (__bridge id)kCTForegroundColorAttributeName : (__bridge id)color,
    };
    NSAttributedString* attributed = [[NSAttributedString alloc] initWithString:string
                                                                     attributes:attributes];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
    if (CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) > width) {
        NSAttributedString* dots = [[NSAttributedString alloc] initWithString:@"…"
                                                                   attributes:attributes];
        CTLineRef ellipsis = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)dots);
        CTLineRef cut = CTLineCreateTruncatedLine(line, width, kCTLineTruncationEnd, ellipsis);
        CFRelease(ellipsis);
        if (cut != nullptr) {
            CFRelease(line);
            line = cut;
        }
    }
    const CGFloat drawn = CGFloat(CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr));
    CGContextSetTextPosition(context, middle - drawn / 2, canvasHeight - baseline);
    CTLineDraw(line, context);
    CFRelease(line);
    CGColorRelease(color);
}

std::optional<GeoStyle> styleNamed(std::string_view name) {
    const char* names[] = {"illustration", "metal", "clay", "blueprint", "technical", "studio"};
    for (int i = 0; i < 6; ++i) {
        if (name == names[i] || name == std::to_string(i)) {
            return GeoStyle(i);
        }
    }
    return std::nullopt;
}

std::optional<GeoColorMode> colourNamed(std::string_view name) {
    const char* names[] = {"material", "parts", "mono", "file"};
    for (int i = 0; i < 4; ++i) {
        if (name == names[i] || name == std::to_string(i)) {
            return GeoColorMode(i);
        }
    }
    return std::nullopt;
}

int usage() {
    std::println(stderr, "usage: gcli sheet <metallib> <out.png> [--cell 192] [--columns N] "
                         "[--style S] [--colour C] [--line-scale X] [--scale 2] [--same-scale] "
                         "[--opaque] [--font regular.ttf] [--title text] < tiles.jsonl");
    return 2;
}

} // namespace

int sheetCommand(int argc, const char** argv) {
    if (argc < 4) {
        return usage();
    }
    const char* library = argv[2];
    const char* output = argv[3];
    double cell = 192, scale = 2;
    double lineScale = -1;
    u32 columns = 0;
    GeoStyle style = GeoStyleIllustration;
    GeoColorMode colour = GeoColorParts;
    bool sameScale = false, opaque = false;
    std::string font, title;
    for (int i = 4; i < argc; ++i) {
        const std::string_view flag = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (flag == "--same-scale") {
            sameScale = true;
        } else if (flag == "--opaque") {
            opaque = true;
        } else if (value == nullptr) {
            return usage();
        } else if (flag == "--cell") {
            cell = std::clamp(std::atof(value), 24.0, 1024.0);
            ++i;
        } else if (flag == "--columns") {
            columns = u32(std::max(1, std::atoi(value)));
            ++i;
        } else if (flag == "--scale") {
            scale = std::clamp(std::atof(value), 1.0, 4.0);
            ++i;
        } else if (flag == "--line-scale") {
            lineScale = std::clamp(std::atof(value), 0.1, 2.0);
            ++i;
        } else if (flag == "--style") {
            const std::optional<GeoStyle> s = styleNamed(value);
            if (!s) {
                return usage();
            }
            style = *s;
            ++i;
        } else if (flag == "--colour" || flag == "--color") {
            const std::optional<GeoColorMode> c = colourNamed(value);
            if (!c) {
                return usage();
            }
            colour = *c;
            ++i;
        } else if (flag == "--font") {
            font = value;
            ++i;
        } else if (flag == "--title") {
            title = value;
            ++i;
        } else {
            return usage();
        }
    }

    std::vector<Tile> tiles;
    for (std::string line; std::getline(std::cin, line);) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        Tile tile;
        std::string error;
        if (!Reader(line).object(tile, error) || tile.blob.empty()) {
            std::println(stderr, "line {}: {}", tiles.size() + 1,
                         error.empty() ? "no blob" : error);
            return 1;
        }
        tiles.push_back(std::move(tile));
    }
    if (tiles.empty()) {
        std::println(stderr, "no tiles on stdin");
        return 1;
    }

    @autoreleasepool {
        GeoThumbnailer* thumbnailer = geo_thumbnailer_create(library);
        if (thumbnailer == nullptr) {
            return 1;
        }
        // The gallery's picture: 2x the cell, from the front right and above, lines weighted for
        // the cell's size.
        const u32 pixels = u32(std::lround(cell * scale));
        GeoRenderOptions options = geo_default_render_options();
        options.width = options.height = pixels;
        options.overlay = false;
        options.azimuth_degrees = 35.0;
        options.elevation_degrees = 30.0;
        options.look.style = style;
        options.look.color_mode = colour;
        options.look.cast_shadow = geo_style_casts_shadow(style);
        options.pixels_per_point = float(scale);
        options.line_scale = float(lineScale > 0 ? lineScale : std::clamp(cell / 320.0, 0.5, 1.0));
        options.transparent = !opaque;

        // At one scale, the largest file fits its cell and the rest are drawn at its scale.
        std::vector<GeoThumbnail> pictures(tiles.size());
        std::vector<std::string> errors(tiles.size());
        double shared = 0.0;
        if (sameScale) {
            for (usize i = 0; i < tiles.size(); ++i) {
                GeoRenderOptions fit = options;
                fit.width = fit.height = 32;
                fit.first_color = tiles[i].firstColor;
                GeoThumbnail probe{};
                char error[512] = {};
                if (geo_thumbnailer_render(thumbnailer, tiles[i].blob.c_str(), &fit, &probe, error,
                                           sizeof(error))) {
                    shared = std::max(shared, probe.world_per_pixel * 32.0 / double(pixels));
                }
                geo_thumbnail_free(&probe);
            }
        }
        for (usize i = 0; i < tiles.size(); ++i) {
            GeoRenderOptions tile = options;
            tile.first_color = tiles[i].firstColor;
            tile.world_per_pixel = shared;
            char error[512] = {};
            if (!geo_thumbnailer_render(thumbnailer, tiles[i].blob.c_str(), &tile, &pictures[i],
                                        error, sizeof(error))) {
                errors[i] = error;
            }
        }
        geo_thumbnailer_destroy(thumbnailer);

        // The sheet, in points: a margin, an optional title, and cells of a picture over two
        // lines of label.
        const double margin = 28, gapX = 16, gapY = 14, titleHeight = title.empty() ? 0 : 34;
        const double labelHeight = 34;
        if (columns == 0) {
            columns = u32(std::ceil(std::sqrt(double(tiles.size()) * 1.4)));
        }
        columns = std::min<u32>(columns, u32(tiles.size()));
        const u32 rows = u32((tiles.size() + columns - 1) / columns);
        const double widthPt = 2 * margin + columns * cell + (columns - 1) * gapX;
        const double heightPt =
            2 * margin + titleHeight + rows * (cell + labelHeight) + (rows - 1) * gapY;
        const usize width = usize(std::lround(widthPt * scale));
        const usize height = usize(std::lround(heightPt * scale));
        if (width > 16384 || height > 16384) {
            std::println(stderr, "the sheet would be {} by {} pixels; use fewer or smaller cells",
                         width, height);
            return 1;
        }

        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, 0, space,
                                                     kCGBitmapByteOrder32Little |
                                                         u32(kCGImageAlphaPremultipliedFirst));
        drawSheet(context, style, CGFloat(width), CGFloat(height), scale);

        if (!font.empty()) {
            CTFontManagerRegisterFontsForURL(
                (__bridge CFURLRef)[NSURL fileURLWithPath:@(font.c_str())],
                kCTFontManagerScopeProcess, nullptr);
        }
        const auto makeFont = [&](double size) {
            CTFontRef made = CTFontCreateWithName(CFSTR("IBMPlexMono"), size * scale, nullptr);
            NSString* family = CFBridgingRelease(CTFontCopyFamilyName(made));
            if (![family hasPrefix:@"IBM Plex Mono"]) {
                CFRelease(made);
                made = CTFontCreateWithName(CFSTR("Menlo"), size * scale, nullptr);
            }
            return made;
        };
        CTFontRef labelFont = makeFont(11.0);
        CTFontRef subFont = makeFont(9.5);
        CTFontRef titleFont = makeFont(15.0);
        const Inks inks = inksOf(style);
        if (!title.empty()) {
            drawText(context, titleFont, inks.text, title, CGFloat(width) / 2,
                     (margin + 18) * scale, CGFloat(double(width) - 2 * margin * scale),
                     CGFloat(height));
        }

        // Every part stands on one baseline, raised by the deepest shadow below any part, so the
        // labels hang under the shadows and every row's labels sit at one height.
        double spill = 0.0;
        for (const GeoThumbnail& picture : pictures) {
            if (picture.bgra != nullptr && picture.part_rect[3] > 0) {
                const double partBottom = picture.part_rect[1] + picture.part_rect[3];
                const double inkBottom = picture.ink_rect[1] + picture.ink_rect[3];
                spill = std::max(spill, (inkBottom - partBottom) / scale);
            }
        }
        const double foot = std::clamp(spill + 3.0, 6.0, cell * 0.25);

        for (usize i = 0; i < tiles.size(); ++i) {
            const u32 column = u32(i % columns), row = u32(i / columns);
            const double left = margin + column * (cell + gapX);
            const double top = margin + titleHeight + row * (cell + labelHeight + gapY);
            // The part stands on the baseline, centred on its own box rather than its shadow's.
            const double baseline = top + cell - foot;
            const GeoThumbnail& picture = pictures[i];
            std::print("{{\"index\":{},\"rect\":[{},{},{},{}]", i, left, top, cell, cell);
            if (picture.bgra != nullptr) {
                const double partBottom = (picture.part_rect[1] + picture.part_rect[3]) / scale;
                const double partMiddle =
                    (picture.part_rect[0] + picture.part_rect[2] / 2.0) / scale;
                const double x =
                    left + cell / 2 - (picture.part_rect[2] > 0 ? partMiddle : cell / 2);
                const double y = baseline - (picture.part_rect[3] > 0 ? partBottom : cell - foot);
                CGDataProviderRef provider = CGDataProviderCreateWithData(
                    nullptr, picture.bgra, usize(picture.width) * picture.height * 4, nullptr);
                CGImageRef image = CGImageCreate(
                    picture.width, picture.height, 8, 32, usize(picture.width) * 4, space,
                    kCGBitmapByteOrder32Little |
                        u32(picture.premultiplied ? kCGImageAlphaPremultipliedFirst
                                                  : kCGImageAlphaNoneSkipFirst),
                    provider, nullptr, false, kCGRenderingIntentDefault);
                const CGRect at = CGRectMake(x * scale, CGFloat(height) - (y + cell) * scale,
                                             picture.width, picture.height);
                CGContextDrawImage(context, at, image);
                CGImageRelease(image);
                CGDataProviderRelease(provider);
                std::print(",\"world_per_point\":{}", picture.world_per_pixel * scale);
            } else {
                std::print(",\"error\":");
                printJson(errors[i]);
            }
            std::println("}}");
            const double middle = left + cell / 2;
            drawText(context, labelFont, inks.text,
                     tiles[i].label.empty() ? tiles[i].blob : tiles[i].label,
                     CGFloat(middle * scale), CGFloat((top + cell + 13) * scale),
                     CGFloat((cell + gapX - 4) * scale), CGFloat(height));
            drawText(context, subFont, inks.muted,
                     picture.bgra != nullptr ? tiles[i].sub : "could not draw",
                     CGFloat(middle * scale), CGFloat((top + cell + 27) * scale),
                     CGFloat((cell + gapX - 4) * scale), CGFloat(height));
        }
        CFRelease(labelFont);
        CFRelease(subFont);
        CFRelease(titleFont);
        for (GeoThumbnail& picture : pictures) {
            geo_thumbnail_free(&picture);
        }

        CGImageRef sheet = CGBitmapContextCreateImage(context);
        NSURL* url = [NSURL fileURLWithPath:@(output)];
        CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
            (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
        bool written = false;
        if (destination != nullptr) {
            CGImageDestinationAddImage(destination, sheet, nullptr);
            written = CGImageDestinationFinalize(destination);
            CFRelease(destination);
        }
        CGImageRelease(sheet);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
        if (!written) {
            std::println(stderr, "could not write {}", output);
            return 1;
        }
    }
    return 0;
}

} // namespace geo::gcli
