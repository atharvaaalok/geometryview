#include "stl/stl_reader.h"

#include "base/parallel.h"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <xlocale.h>

namespace geo::import {
namespace {

constexpr usize kBinaryHeaderSize = 84;
constexpr usize kBinaryRecordSize = 50;
constexpr usize kNormalSize = 12;
constexpr usize kCornersSize = 36;
constexpr usize kFloatsPerTriangle = 9;
constexpr usize kParallelGrainBytes = 1 << 22;

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

const char* skipSpaces(const char* p, const char* end) {
    while (p < end && isSpace(*p)) {
        ++p;
    }
    return p;
}

// Slow path for numbers the fast path cannot round exactly. The text is not null-terminated, so it
// is copied first. Very long numbers are cut off, which only drops digits beyond float precision.
float parseSlow(const char* first, const char* last) {
    static const locale_t cLocale = newlocale(LC_ALL_MASK, "C", nullptr);
    std::array<char, 64> buffer{};
    const usize length = std::min<usize>(usize(last - first), buffer.size() - 1);
    std::memcpy(buffer.data(), first, length);
    return strtof_l(buffer.data(), nullptr, cLocale);
}

// Parses a decimal float. When the mantissa fits in 2^53 and the exponent is within 10^22, one
// double operation gives the correctly rounded result, which covers nearly all STL files.
const char* parseFloat(const char* p, const char* end, float& value) {
    static constexpr std::array<double, 23> kPowersOfTen = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    constexpr u64 kMaxExactMantissa = u64(1) << 53;

    const char* start = p;
    bool negative = false;
    if (p < end && (*p == '-' || *p == '+')) {
        negative = *p == '-';
        ++p;
    }

    u64 mantissa = 0;
    int digits = 0;
    int exponent = 0;
    const char* firstDigit = p;
    while (p < end && isDigit(*p)) {
        if (digits < 18) {
            mantissa = mantissa * 10 + u64(*p - '0');
            digits += mantissa != 0;
        } else {
            ++exponent;
        }
        ++p;
    }
    bool sawDigit = p != firstDigit;
    if (p < end && *p == '.') {
        ++p;
        const char* fraction = p;
        while (p < end && isDigit(*p)) {
            if (digits < 18) {
                mantissa = mantissa * 10 + u64(*p - '0');
                digits += mantissa != 0;
                --exponent;
            }
            ++p;
        }
        sawDigit = sawDigit || p != fraction;
    }
    if (!sawDigit) {
        // Covers "nan" and "inf", which the caller drops as non-finite.
        while (p < end && !isSpace(*p) && *p != '\n') {
            ++p;
        }
        value = p == start ? 0.0f : parseSlow(start, p);
        return p == start ? nullptr : p;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        const char* marker = p + 1;
        bool negativeExponent = false;
        if (marker < end && (*marker == '-' || *marker == '+')) {
            negativeExponent = *marker == '-';
            ++marker;
        }
        if (marker < end && isDigit(*marker)) {
            int written = 0;
            while (marker < end && isDigit(*marker)) {
                written = std::min(written * 10 + (*marker - '0'), 100000);
                ++marker;
            }
            exponent += negativeExponent ? -written : written;
            p = marker;
        }
    }

    if (mantissa <= kMaxExactMantissa && exponent >= -22 && exponent <= 22) {
        double result = double(mantissa);
        result = exponent < 0 ? result / kPowersOfTen[usize(-exponent)]
                              : result * kPowersOfTen[usize(exponent)];
        value = float(negative ? -result : result);
    } else {
        value = parseSlow(start, p);
    }
    return p;
}

// Appends the vertices of whole lines in [first, last). Anything that is not a vertex line is
// skipped, which tolerates the non-standard `color` lines some exporters write.
void parseAsciiRange(const char* first, const char* last, std::vector<float>& out) {
    static constexpr std::string_view kVertex = "vertex";
    const char* p = first;
    while (p < last) {
        p = skipSpaces(p, last);
        const char* lineEnd = static_cast<const char*>(std::memchr(p, '\n', usize(last - p)));
        lineEnd = lineEnd ? lineEnd : last;
        const usize length = usize(lineEnd - p);
        if (length > kVertex.size() && std::memcmp(p, kVertex.data(), kVertex.size()) == 0 &&
            isSpace(p[kVertex.size()])) {
            std::array<float, 3> xyz{};
            const char* cursor = p + kVertex.size();
            bool valid = true;
            for (float& coordinate : xyz) {
                cursor = skipSpaces(cursor, lineEnd);
                cursor = parseFloat(cursor, lineEnd, coordinate);
                if (cursor == nullptr) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                out.insert(out.end(), xyz.begin(), xyz.end());
            }
        }
        p = lineEnd + 1;
    }
}

// Moves `p` forward to just past the next newline, so a parallel range starts on a whole line.
const char* nextLine(const char* p, const char* end) {
    const char* newline = static_cast<const char*>(std::memchr(p, '\n', usize(end - p)));
    return newline ? newline + 1 : end;
}

void parseAscii(std::span<const u8> file, std::vector<float>& corners) {
    const char* begin = reinterpret_cast<const char*>(file.data());
    const char* end = begin + file.size();
    const usize cores = std::max<usize>(1, std::thread::hardware_concurrency());
    const usize ranges = std::clamp<usize>(file.size() / kParallelGrainBytes, 1, cores);

    std::vector<std::vector<float>> parts(ranges);
    parallelFor(ranges, [&](usize range) {
        const char* first = begin + file.size() * range / ranges;
        const char* last = begin + file.size() * (range + 1) / ranges;
        if (range != 0) {
            first = nextLine(first, end);
        }
        if (range + 1 != ranges) {
            last = nextLine(last, end);
        }
        if (first < last) {
            parts[range].reserve(usize(last - first) / 16);
            parseAsciiRange(first, last, parts[range]);
        }
    });

    usize total = 0;
    for (const std::vector<float>& part : parts) {
        total += part.size();
    }
    corners.resize(total);
    usize offset = 0;
    for (const std::vector<float>& part : parts) {
        if (!part.empty()) {
            std::memcpy(corners.data() + offset, part.data(), part.size() * sizeof(float));
        }
        offset += part.size();
    }
}

void parseBinary(std::span<const u8> file, usize triangles, std::vector<float>& corners) {
    corners.resize(triangles * kFloatsPerTriangle);
    parallelRanges(triangles, 1 << 16, [&](usize first, usize last, usize) {
        const u8* record = file.data() + kBinaryHeaderSize + first * kBinaryRecordSize;
        float* out = corners.data() + first * kFloatsPerTriangle;
        for (usize t = first; t < last; ++t) {
            std::memcpy(out, record + kNormalSize, kCornersSize);
            record += kBinaryRecordSize;
            out += kFloatsPerTriangle;
        }
    });
}

u32 readCount(std::span<const u8> file) {
    u32 count = 0;
    std::memcpy(&count, file.data() + kBinaryHeaderSize - sizeof(u32), sizeof(u32));
    return count;
}

// True if the start of the file reads like ASCII STL. Binary files often begin with "solid" too, so
// the word alone proves nothing, and a `facet` or `vertex` keyword must also appear early on.
bool looksAscii(std::span<const u8> file) {
    const std::string_view head(reinterpret_cast<const char*>(file.data()),
                                std::min<usize>(file.size(), 4096));
    return head.find("solid") != std::string_view::npos &&
           (head.find("facet") != std::string_view::npos ||
            head.find("vertex") != std::string_view::npos);
}

// Removes triangles containing NaN or infinity, which would poison bounds and quantisation.
usize dropNonFinite(std::vector<float>& corners) {
    const usize triangles = corners.size() / kFloatsPerTriangle;
    usize kept = 0;
    for (usize t = 0; t < triangles; ++t) {
        const float* source = corners.data() + t * kFloatsPerTriangle;
        bool finite = true;
        for (usize i = 0; i < kFloatsPerTriangle; ++i) {
            finite = finite && std::isfinite(source[i]);
        }
        if (finite) {
            if (kept != t) {
                std::memmove(corners.data() + kept * kFloatsPerTriangle, source,
                             kFloatsPerTriangle * sizeof(float));
            }
            ++kept;
        }
    }
    corners.resize(kept * kFloatsPerTriangle);
    return triangles - kept;
}

} // namespace

bool readStl(std::span<const u8> file, TriangleSoup& soup, std::string& error) {
    static_assert(std::endian::native == std::endian::little);

    if (file.empty()) {
        error = "This file is empty.";
        return false;
    }

    const usize declared = file.size() >= kBinaryHeaderSize ? readCount(file) : 0;
    const bool exactBinary = file.size() >= kBinaryHeaderSize &&
                             (file.size() - kBinaryHeaderSize) / kBinaryRecordSize == declared &&
                             (file.size() - kBinaryHeaderSize) % kBinaryRecordSize == 0;

    if (exactBinary) {
        parseBinary(file, declared, soup.corners);
    } else if (looksAscii(file)) {
        parseAscii(file, soup.corners);
        const usize stray = soup.corners.size() % kFloatsPerTriangle;
        if (stray != 0) {
            soup.corners.resize(soup.corners.size() - stray);
            soup.warnings.push_back("The file ended partway through a triangle.");
        }
    } else if (file.size() >= kBinaryHeaderSize &&
               declared <= (file.size() - kBinaryHeaderSize) / kBinaryRecordSize && declared > 0) {
        parseBinary(file, declared, soup.corners);
        soup.warnings.push_back("The file has extra data after its last triangle.");
    } else {
        error = "This does not look like an STL file.";
        return false;
    }

    if (const usize dropped = dropNonFinite(soup.corners); dropped != 0) {
        soup.warnings.push_back(
            std::format("Dropped {} triangles with invalid coordinates.", dropped));
    }
    if (soup.corners.empty()) {
        error = "This STL file has no triangles.";
        return false;
    }
    return true;
}

} // namespace geo::import
