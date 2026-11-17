#include "base/png_text.h"

#include <array>
#include <cstring>

namespace geo {

namespace {

constexpr std::array<u8, 8> kSignature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

constexpr std::array<u32, 256> makeCrcTable() {
    std::array<u32, 256> table{};
    for (u32 n = 0; n < 256; ++n) {
        u32 c = n;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}

constexpr std::array<u32, 256> kCrcTable = makeCrcTable();

u32 readBigEndian(const u8* p) {
    return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | u32(p[3]);
}

void appendBigEndian(std::vector<u8>& out, u32 value) {
    out.push_back(u8(value >> 24));
    out.push_back(u8(value >> 16));
    out.push_back(u8(value >> 8));
    out.push_back(u8(value));
}

void appendText(std::vector<u8>& out, const PngText& text) {
    const usize start = out.size();
    appendBigEndian(out, u32(text.keyword.size() + 1 + text.text.size()));
    out.insert(out.end(), {'t', 'E', 'X', 't'});
    out.insert(out.end(), text.keyword.begin(), text.keyword.end());
    out.push_back(0);
    out.insert(out.end(), text.text.begin(), text.text.end());
    appendBigEndian(out, pngCrc(std::span(out).subspan(start + 4)));
}

} // namespace

u32 pngCrc(std::span<const u8> bytes) {
    u32 c = 0xFFFFFFFFu;
    for (const u8 b : bytes) {
        c = kCrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

std::vector<u8> withPngText(std::span<const u8> png, std::span<const PngText> texts) {
    if (png.size() < kSignature.size() ||
        std::memcmp(png.data(), kSignature.data(), kSignature.size()) != 0) {
        return {};
    }
    std::vector<u8> out(png.begin(), png.begin() + kSignature.size());
    out.reserve(png.size() + 256);
    usize at = kSignature.size();
    bool sawHeader = false, sawEnd = false;
    while (!sawEnd) {
        // Each chunk: its data's length, its type, its data and its CRC.
        if (png.size() - at < 12) {
            return {};
        }
        const u32 length = readBigEndian(png.data() + at);
        if (length > png.size() - at - 12) {
            return {};
        }
        const u8* type = png.data() + at + 4;
        const usize size = 12 + usize(length);
        if (!sawHeader && std::memcmp(type, "IHDR", 4) != 0) {
            return {}; // a PNG starts with its header
        }
        bool replaced = false;
        if (std::memcmp(type, "tEXt", 4) == 0) {
            const std::string_view data(reinterpret_cast<const char*>(type + 4), length);
            const std::string_view keyword = data.substr(0, data.find('\0'));
            for (const PngText& text : texts) {
                replaced = replaced || keyword == text.keyword;
            }
        }
        if (!replaced) {
            out.insert(out.end(), png.begin() + at, png.begin() + at + size);
        }
        if (!sawHeader) {
            sawHeader = true;
            for (const PngText& text : texts) {
                appendText(out, text);
            }
        }
        sawEnd = std::memcmp(type, "IEND", 4) == 0;
        at += size;
    }
    return out;
}

} // namespace geo
