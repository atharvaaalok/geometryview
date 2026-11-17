// A saved picture says what made it: PNG text after the header, which ImageIO reads back, with
// the picture itself untouched.
#include "base/about.h"
#include "base/png_text.h"
#include "engine.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <array>
#include <cstring>
#include <doctest/doctest.h>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace geo;

namespace {

// A picture GeometryView drew, as ImageIO wrote it: header, sRGB, EXIF and data.
std::vector<u8> goldenPng() {
    std::ifstream file(std::string(GEO_TEST_GOLDEN_DIR) + "/clay-parts.png", std::ios::binary);
    REQUIRE(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Chunk {
    std::string type;
    std::string data;
    bool crcValid;
};

std::vector<Chunk> chunks(const std::vector<u8>& png) {
    std::vector<Chunk> out;
    for (usize at = 8; at + 12 <= png.size();) {
        const u32 length =
            u32(png[at]) << 24 | u32(png[at + 1]) << 16 | u32(png[at + 2]) << 8 | u32(png[at + 3]);
        REQUIRE(at + 12 + length <= png.size());
        const u8* end = png.data() + at + 8 + length;
        const u32 crc = u32(end[0]) << 24 | u32(end[1]) << 16 | u32(end[2]) << 8 | u32(end[3]);
        out.push_back({std::string(reinterpret_cast<const char*>(png.data() + at + 4), 4),
                       std::string(reinterpret_cast<const char*>(png.data() + at + 8), length),
                       crc == pngCrc(std::span(png).subspan(at + 4, 4 + length))});
        at += 12 + length;
    }
    return out;
}

// The picture's pixels as ImageIO decodes them, and its PNG Software text.
struct Decoded {
    std::vector<u8> rgba;
    std::string software;
};

Decoded decode(const std::vector<u8>& png) {
    Decoded decoded;
    NSData* data = [NSData dataWithBytes:png.data() length:png.size()];
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr);
    REQUIRE(source != nullptr);
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    REQUIRE(image != nullptr);
    NSDictionary* properties =
        CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr));
    NSString* software = properties[(__bridge NSString*)kCGImagePropertyPNGDictionary]
                                   [(__bridge NSString*)kCGImagePropertyPNGSoftware];
    decoded.software = software != nil ? software.UTF8String : "";
    const usize width = CGImageGetWidth(image), height = CGImageGetHeight(image);
    decoded.rgba.resize(width * height * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        decoded.rgba.data(), width, height, 8, width * 4, space,
        uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
    CGContextDrawImage(context, CGRectMake(0, 0, double(width), double(height)), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    CFRelease(source);
    return decoded;
}

} // namespace

TEST_CASE("a tagged picture says what made it, right after its header, and is otherwise the same") {
    const std::vector<u8> png = goldenPng();
    GeoPng tagged{};
    REQUIRE(geo_tag_picture(png.data(), png.size(), &tagged));
    const std::vector<u8> bytes(tagged.bytes, tagged.bytes + tagged.size);
    geo_png_free(&tagged);
    CHECK(tagged.bytes == nullptr);

    const std::vector<Chunk> before = chunks(png), after = chunks(bytes);
    REQUIRE(after.size() == before.size() + 2);
    CHECK(after[0].type == "IHDR");
    CHECK(after[1].type == "tEXt");
    CHECK(after[1].data == std::string("Software") + '\0' + about::pictureSoftware());
    CHECK(after[2].type == "tEXt");
    CHECK(after[2].data == std::string("Comment") + '\0' + std::string(about::pictureComment()));
    CHECK(about::pictureSoftware() == std::string("GeometryView ") + geo_engine_version());
    for (usize i = 0; i < after.size(); ++i) {
        CAPTURE(i);
        CHECK(after[i].crcValid);
        if (i >= 3) {
            CHECK(after[i].type == before[i - 2].type);
            CHECK(after[i].data == before[i - 2].data);
        }
    }

    const Decoded original = decode(png), read = decode(bytes);
    CHECK(read.software == about::pictureSoftware());
    CHECK(read.rgba == original.rgba);
}

TEST_CASE("tagging a picture again changes nothing, and anything but a whole PNG is refused") {
    const std::vector<u8> png = goldenPng();
    const std::vector<u8> once = about::taggedPicture(png);
    REQUIRE(!once.empty());
    CHECK(about::taggedPicture(once) == once);

    // Another program's text stays; ours replaces only our own keywords.
    const std::array<PngText, 1> theirs = {PngText{"Author", "someone"}};
    const std::vector<u8> authored = withPngText(once, theirs);
    const std::vector<u8> again = about::taggedPicture(authored);
    usize software = 0, author = 0;
    for (const Chunk& chunk : chunks(again)) {
        software += chunk.data.starts_with(std::string("Software") + '\0') ? 1 : 0;
        author += chunk.data.starts_with(std::string("Author") + '\0') ? 1 : 0;
    }
    CHECK(software == 1);
    CHECK(author == 1);

    const std::vector<u8> text = {'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g'};
    CHECK(about::taggedPicture(text).empty());
    CHECK(about::taggedPicture(std::span(png).first(png.size() - 5)).empty());
    GeoPng out{};
    CHECK_FALSE(geo_tag_picture(text.data(), text.size(), &out));
    CHECK(out.bytes == nullptr);
}
