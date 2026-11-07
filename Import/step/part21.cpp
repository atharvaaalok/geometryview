#include "step/part21.h"

#include "base/parallel.h"
#include "base/parallel_sort.h"

#include <algorithm>
#include <arm_neon.h>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <format>

namespace geo::import::step {
namespace {

constexpr usize kChunkBytes = 8 << 20;
constexpr usize kBlockBytes = 64;
// A sparse id range would make a direct table wasteful, so such files use a sorted one.
constexpr u64 kMaxDenseIdsPerRecord = 16;

bool isSpace(u8 c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

bool isKeywordCharacter(u8 c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

constexpr u64 hashKeyword(std::string_view name) {
    u64 hash = 0xCBF29CE484222325;
    for (const char c : name) {
        hash = (hash ^ u8(c)) * 0x100000001B3;
    }
    return hash;
}

class KeywordTable {
public:
    KeywordTable() {
#define X(name) insert(hashKeyword(#name), Kind::name);
        GEO_STEP_KINDS(X)
#undef X
    }

    Kind find(u64 hash) const {
        for (usize slot = hash & kMask;; slot = (slot + 1) & kMask) {
            if (slots_[slot].hash == hash) {
                return slots_[slot].kind;
            }
            if (slots_[slot].hash == 0) {
                return Kind::Other;
            }
        }
    }

private:
    static constexpr usize kMask = 1023;
    struct Slot {
        u64 hash = 0;
        Kind kind = Kind::Other;
    };

    void insert(u64 hash, Kind kind) {
        usize slot = hash & kMask;
        while (slots_[slot].hash != 0) {
            slot = (slot + 1) & kMask;
        }
        slots_[slot] = {hash, kind};
    }

    std::array<Slot, kMask + 1> slots_;
};

const KeywordTable& keywordTable() {
    static const KeywordTable table;
    return table;
}

// For entities written as several types, the type that decides how the record is used.
int complexPriority(Kind kind) {
    switch (kind) {
    case Kind::REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION:
    case Kind::SHAPE_REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION:
        return 4;
    case Kind::REPRESENTATION_RELATIONSHIP:
    case Kind::SHAPE_REPRESENTATION_RELATIONSHIP:
    case Kind::BREP_WITH_VOIDS:
    case Kind::CONVERSION_BASED_UNIT:
    case Kind::SI_UNIT:
        return 3;
    case Kind::GEOMETRIC_REPRESENTATION_CONTEXT:
    case Kind::MANIFOLD_SOLID_BREP:
    case Kind::CARTESIAN_TRANSFORMATION_OPERATOR_3D:
    case Kind::FACETED_BREP:
        return 2;
    case Kind::Other:
    case Kind::Complex:
        return 0;
    default:
        return 1;
    }
}

// Sets one bit per byte of the four registers, in memory order.
u64 toBitMask(uint8x16_t a, uint8x16_t b, uint8x16_t c, uint8x16_t d) {
    const uint8x16_t bits = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t sum = vpaddq_u8(vandq_u8(a, bits), vandq_u8(b, bits));
    const uint8x16_t high = vpaddq_u8(vandq_u8(c, bits), vandq_u8(d, bits));
    sum = vpaddq_u8(sum, high);
    sum = vpaddq_u8(sum, sum);
    return vgetq_lane_u64(vreinterpretq_u64_u8(sum), 0);
}

// Every bit is the parity of the bits at or below it, so a quote bit opens a run that the next
// closes. A doubled quote inside a string toggles twice, which needs no special case.
u64 prefixXor(u64 mask) {
    return vgetq_lane_u64(vreinterpretq_u64_p128(vmull_p64(poly64_t(mask), poly64_t(~u64(0)))), 0);
}

struct ByteCounts {
    u64 quotes = 0;
    bool hasComment = false;
};

// Counts quotes and looks for the start of a comment. A comment can only be misjudged as present,
// which is safe: it triggers the slower path that handles comments properly.
ByteCounts countBytes(const u8* data, usize size, u8 previous) {
    ByteCounts result;
    const uint8x16_t quote = vdupq_n_u8('\'');
    const uint8x16_t slash = vdupq_n_u8('/');
    const uint8x16_t star = vdupq_n_u8('*');
    uint8x16_t last = vdupq_n_u8(previous);
    uint8x16_t comments = vdupq_n_u8(0);
    usize offset = 0;
    while (size - offset >= 16) {
        uint8x16_t counts = vdupq_n_u8(0);
        const usize steps = std::min<usize>((size - offset) / 16, 255);
        for (usize i = 0; i < steps; ++i, offset += 16) {
            const uint8x16_t v = vld1q_u8(data + offset);
            counts = vsubq_u8(counts, vceqq_u8(v, quote));
            const uint8x16_t before = vextq_u8(last, v, 15);
            comments = vorrq_u8(comments, vandq_u8(vceqq_u8(before, slash), vceqq_u8(v, star)));
            last = v;
        }
        result.quotes += vaddlvq_u8(counts);
    }
    result.hasComment = vmaxvq_u8(comments) != 0;
    u8 prior = vgetq_lane_u8(last, 15);
    for (; offset < size; ++offset) {
        const u8 c = data[offset];
        result.quotes += c == '\'';
        result.hasComment |= prior == '/' && c == '*';
        prior = c;
    }
    return result;
}

// Replaces comments with spaces, keeping every byte offset. This is the rare path.
void blankComments(u8* data, usize size) {
    bool inString = false;
    for (usize i = 0; i + 1 < size; ++i) {
        if (data[i] == '\'') {
            inString = !inString;
        } else if (!inString && data[i] == '/' && data[i + 1] == '*') {
            usize j = i;
            while (j < size && !(data[j] == '*' && j + 1 < size && data[j + 1] == '/')) {
                data[j++] = ' ';
            }
            for (usize k = 0; k < 2 && j < size; ++k) {
                data[j++] = ' ';
            }
            i = j - 1;
        }
    }
}

// Finds the DATA statement. Scanned statement by statement, so text in the header's strings cannot
// be mistaken for it. Returns the offset just past it, or nothing.
bool findData(std::span<const u8> file, usize& dataStart, bool& unsupportedSection) {
    usize statement = 0;
    bool inString = false;
    for (usize i = 0; i < file.size(); ++i) {
        const u8 c = file[i];
        if (c == '\'') {
            inString = !inString;
        } else if (c == ';' && !inString) {
            usize first = statement;
            while (first < i && isSpace(file[first])) {
                ++first;
            }
            const std::string_view text(reinterpret_cast<const char*>(file.data() + first),
                                        i - first);
            if (text.starts_with("DATA")) {
                dataStart = i + 1;
                return true;
            }
            unsupportedSection |= text.starts_with("ANCHOR") || text.starts_with("REFERENCE");
            statement = i + 1;
        }
    }
    return false;
}

struct ChunkOutput {
    std::vector<u32> ids;
    std::vector<Kind> kinds;
    std::vector<u64> offsets;
    std::vector<u32> lengths;
    std::vector<u32> refCounts;
    std::vector<u32> refs; // raw ids until they are resolved
    bool needsWholeFile = false;
    bool idOverflow = false;
};

// Reads a run of digits as an id. Ids beyond 32 bits are reported, not truncated.
const u8* parseId(const u8* p, const u8* end, u32& id, bool& overflow) {
    u64 value = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        value = value * 10 + u64(*p - '0');
        overflow |= value > 0xFFFFFFFE;
        ++p;
    }
    id = u32(value);
    return p;
}

// Records every reference in [p, end), skipping strings.
void scanReferences(const u8* p, const u8* end, ChunkOutput& out, u32& count) {
    const uint8x16_t hash = vdupq_n_u8('#');
    const uint8x16_t quote = vdupq_n_u8('\'');
    while (p < end) {
        if (end - p >= 16) {
            const uint8x16_t v = vld1q_u8(p);
            const uint8x16_t hit = vorrq_u8(vceqq_u8(v, hash), vceqq_u8(v, quote));
            // Narrowing gives four bits per byte in 64 bits, the usual NEON movemask.
            const u64 nibbles =
                vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(hit), 4)), 0);
            if (nibbles == 0) {
                p += 16;
                continue;
            }
            p += std::countr_zero(nibbles) / 4;
        }
        const u8 c = *p;
        if (c == '\'') {
            ++p;
            while (p < end && *p != '\'') {
                ++p;
            }
            ++p;
        } else if (c == '#') {
            u32 id = 0;
            p = parseId(p + 1, end, id, out.idOverflow);
            out.refs.push_back(id);
            ++count;
        } else {
            ++p;
        }
    }
}

// Reads a keyword and returns its type. `p` ends up just past it.
Kind parseKeyword(const u8*& p, const u8* end) {
    u64 hash = 0xCBF29CE484222325;
    while (p < end && isKeywordCharacter(*p)) {
        hash = (hash ^ *p) * 0x100000001B3;
        ++p;
    }
    return keywordTable().find(hash);
}

// Skips one balanced bracket group, starting at its opening bracket.
const u8* skipGroup(const u8* p, const u8* end) {
    int depth = 0;
    for (; p < end; ++p) {
        if (*p == '\'') {
            ++p;
            while (p < end && *p != '\'') {
                ++p;
            }
        } else if (*p == '(') {
            ++depth;
        } else if (*p == ')' && --depth == 0) {
            return p + 1;
        }
    }
    return end;
}

Kind parseComplexKeywords(const u8* p, const u8* end) {
    Kind best = Kind::Complex;
    while (true) {
        while (p < end && isSpace(*p)) {
            ++p;
        }
        if (p >= end || !isKeywordCharacter(*p)) {
            return best;
        }
        const Kind kind = parseKeyword(p, end);
        if (complexPriority(kind) > complexPriority(best)) {
            best = kind;
        }
        while (p < end && isSpace(*p)) {
            ++p;
        }
        if (p < end && *p == '(') {
            p = skipGroup(p, end);
        }
    }
}

// Parses one statement ending at the semicolon at `end`.
void parseStatement(const u8* base, const u8* p, const u8* end, ChunkOutput& out) {
    while (p < end && isSpace(*p)) {
        ++p;
    }
    if (p >= end) {
        return;
    }
    if (*p != '#') {
        // ENDSEC and further DATA statements are skipped. Anything else is not understood.
        const std::string_view text(reinterpret_cast<const char*>(p), usize(end - p));
        out.needsWholeFile |= !(text.starts_with("ENDSEC") || text.starts_with("DATA") ||
                                text.starts_with("END-ISO"));
        return;
    }
    const u8* start = p;
    u32 id = 0;
    p = parseId(p + 1, end, id, out.idOverflow);
    while (p < end && isSpace(*p)) {
        ++p;
    }
    if (p >= end || *p != '=') {
        out.needsWholeFile = true;
        return;
    }
    ++p;
    while (p < end && isSpace(*p)) {
        ++p;
    }
    if (p < end && *p == '&') {
        out.needsWholeFile = true; // external scope
        return;
    }
    Kind kind = Kind::Complex;
    if (p < end && *p == '(') {
        kind = parseComplexKeywords(p + 1, end);
    } else {
        kind = parseKeyword(p, end);
    }
    out.ids.push_back(id);
    out.kinds.push_back(kind);
    out.offsets.push_back(u64(start - base));
    out.lengths.push_back(u32(end + 1 - start));
    u32 refCount = 0;
    scanReferences(p, end, out, refCount);
    out.refCounts.push_back(refCount);
}

// Finds statement ends, one 64-byte block at a time, starting in a known string state. Handles the
// statements that begin in [first, last), reading on past `last` to finish the final one.
void scanChunk(std::span<const u8> file, usize first, usize last, usize dataStart,
               bool startsInString, ChunkOutput& out) {
    const u8* base = file.data();
    const uint8x16_t quote = vdupq_n_u8('\'');
    const uint8x16_t semicolon = vdupq_n_u8(';');

    bool inString = startsInString;
    // A statement that starts exactly here belongs to this chunk. Any other starts in the previous
    // chunk, which reads on to finish it, so scanning begins at the first statement end.
    const bool atStatementStart = first == dataStart || (base[first - 1] == ';' && !inString);
    usize statement = atStatementStart ? first : ~usize(0);
    usize offset = first;
    std::array<u8, kBlockBytes> tail{};
    while (offset < file.size()) {
        const usize available = file.size() - offset;
        const u8* block = base + offset;
        if (available < kBlockBytes) {
            tail.fill(' ');
            std::memcpy(tail.data(), block, available);
            block = tail.data();
        }
        const uint8x16_t a = vld1q_u8(block), b = vld1q_u8(block + 16), c = vld1q_u8(block + 32),
                         d = vld1q_u8(block + 48);
        const u64 quotes = toBitMask(vceqq_u8(a, quote), vceqq_u8(b, quote), vceqq_u8(c, quote),
                                     vceqq_u8(d, quote));
        u64 ends = toBitMask(vceqq_u8(a, semicolon), vceqq_u8(b, semicolon), vceqq_u8(c, semicolon),
                             vceqq_u8(d, semicolon));
        const u64 strings = prefixXor(quotes) ^ (inString ? ~u64(0) : 0);
        inString = (strings >> 63) != 0;
        ends &= ~strings;
        while (ends != 0) {
            const usize position = offset + usize(std::countr_zero(ends));
            ends &= ends - 1;
            if (statement != ~usize(0)) {
                parseStatement(base, base + statement, base + position, out);
            }
            statement = position + 1;
            if (statement >= last) {
                return;
            }
        }
        offset += kBlockBytes;
    }
}

template <typename T>
void concatenate(std::vector<ChunkOutput>& chunks, std::vector<T> ChunkOutput::* member,
                 std::vector<T>& out) {
    usize total = 0;
    std::vector<usize> bases(chunks.size());
    for (usize i = 0; i < chunks.size(); ++i) {
        bases[i] = total;
        total += (chunks[i].*member).size();
    }
    out.resize(total);
    parallelFor(chunks.size(), [&](usize i) {
        std::vector<T>& source = chunks[i].*member;
        std::copy(source.begin(), source.end(), out.begin() + std::ptrdiff_t(bases[i]));
        std::vector<T>().swap(source);
    });
}

// Replaces raw ids in `refs` with record indices.
void resolveReferences(Part21& index) {
    const u32 records = index.recordCount();
    index.maxId = records == 0 ? 0 : *std::max_element(index.ids.begin(), index.ids.end());

    if (u64(index.maxId) <= kMaxDenseIdsPerRecord * records + 1024) {
        std::vector<u32> table(usize(index.maxId) + 1, kNoRecord);
        parallelRanges(records, 1 << 16, [&](usize first, usize last, usize) {
            for (usize r = first; r < last; ++r) {
                table[index.ids[r]] = u32(r);
            }
        });
        parallelRanges(index.refs.size(), 1 << 16, [&](usize first, usize last, usize) {
            for (usize i = first; i < last; ++i) {
                const u32 id = index.refs[i];
                index.refs[i] = id <= index.maxId ? table[id] : kNoRecord;
            }
        });
        return;
    }

    struct IdEntry {
        u64 key;
        u32 record;
    };
    std::vector<IdEntry> sorted(records);
    for (u32 r = 0; r < records; ++r) {
        sorted[r] = {u64(index.ids[r]), r};
    }
    parallelSort(sorted, [](const IdEntry& e) { return e.key; });
    parallelRanges(index.refs.size(), 1 << 14, [&](usize first, usize last, usize) {
        for (usize i = first; i < last; ++i) {
            const u64 id = index.refs[i];
            const auto it =
                std::lower_bound(sorted.begin(), sorted.end(), id,
                                 [](const IdEntry& entry, u64 value) { return entry.key < value; });
            index.refs[i] = it != sorted.end() && it->key == id ? it->record : kNoRecord;
        }
    });
}

} // namespace

bool indexPart21(std::span<const u8> file, Part21& index, std::vector<u8>& scratch,
                 std::string& error) {
    usize dataStart = 0;
    bool unsupportedSection = false;
    if (!findData(file, dataStart, unsupportedSection)) {
        error = "This is not a STEP file: it has no DATA section.";
        return false;
    }
    index = {};
    index.needsWholeFile = unsupportedSection;

    // Pass one finds how many quotes precede each chunk, which fixes the string state at its start.
    const usize dataSize = file.size() - dataStart;
    const usize chunkCount = std::max<usize>(1, (dataSize + kChunkBytes - 1) / kChunkBytes);
    const auto chunkStart = [&](usize chunk) { return dataStart + chunk * kChunkBytes; };
    std::vector<ByteCounts> counts(chunkCount);
    parallelFor(chunkCount, [&](usize chunk) {
        const usize first = chunkStart(chunk);
        const usize last = std::min(first + kChunkBytes, file.size());
        counts[chunk] =
            countBytes(file.data() + first, last - first, first > 0 ? file[first - 1] : u8(0));
    });
    bool hasComment = false;
    for (const ByteCounts& count : counts) {
        hasComment |= count.hasComment;
    }
    if (hasComment) {
        scratch.assign(file.begin(), file.end());
        blankComments(scratch.data() + dataStart, dataSize);
        file = scratch;
    }

    std::vector<bool> startsInString(chunkCount);
    u64 quotes = 0;
    for (usize chunk = 0; chunk < chunkCount; ++chunk) {
        startsInString[chunk] = (quotes & 1) != 0;
        quotes += counts[chunk].quotes;
    }
    if (hasComment) {
        // Quotes inside comments have gone, so the counts above no longer apply.
        quotes = 0;
        for (usize chunk = 0; chunk < chunkCount; ++chunk) {
            startsInString[chunk] = (quotes & 1) != 0;
            const usize first = chunkStart(chunk);
            const usize last = std::min(first + kChunkBytes, file.size());
            quotes += countBytes(file.data() + first, last - first, 0).quotes;
        }
    }

    std::vector<ChunkOutput> chunks(chunkCount);
    parallelFor(chunkCount, [&](usize chunk) {
        const usize first = chunkStart(chunk);
        ChunkOutput& out = chunks[chunk];
        const usize reserve = kChunkBytes / 40;
        out.ids.reserve(reserve);
        out.kinds.reserve(reserve);
        out.offsets.reserve(reserve);
        out.lengths.reserve(reserve);
        out.refCounts.reserve(reserve);
        out.refs.reserve(reserve * 3);
        scanChunk(file, first, std::min(first + kChunkBytes, file.size()), dataStart,
                  startsInString[chunk], out);
    });

    for (const ChunkOutput& chunk : chunks) {
        index.needsWholeFile |= chunk.needsWholeFile;
        if (chunk.idOverflow) {
            index.needsWholeFile = true;
        }
    }

    std::vector<u32> refCounts;
    concatenate(chunks, &ChunkOutput::ids, index.ids);
    concatenate(chunks, &ChunkOutput::kinds, index.kinds);
    concatenate(chunks, &ChunkOutput::offsets, index.offsets);
    concatenate(chunks, &ChunkOutput::lengths, index.lengths);
    concatenate(chunks, &ChunkOutput::refCounts, refCounts);
    concatenate(chunks, &ChunkOutput::refs, index.refs);

    if (index.refs.size() >= 0xFFFFFFFF) {
        error = "This file has too many references.";
        return false;
    }
    index.refStarts.resize(refCounts.size() + 1);
    u32 running = 0;
    for (usize r = 0; r < refCounts.size(); ++r) {
        index.refStarts[r] = running;
        running += refCounts[r];
    }
    index.refStarts[refCounts.size()] = running;

    index.file = file;
    const usize firstRecord = index.offsets.empty() ? dataStart : usize(index.offsets[0]);
    index.header = {reinterpret_cast<const char*>(file.data()), firstRecord};
    resolveReferences(index);
    return true;
}

// ParamCursor.

namespace {

const char* skipSpaces(const char* p, const char* end) {
    while (p < end && isSpace(u8(*p))) {
        ++p;
    }
    return p;
}

// Past the opening bracket of the entity's parameters, or null.
const char* findParameters(std::string_view record, std::string_view name) {
    const char* p = record.data();
    const char* end = p + record.size();
    while (p < end && *p != '=') {
        ++p;
    }
    p = skipSpaces(p + (p < end ? 1 : 0), end);
    if (p >= end) {
        return nullptr;
    }
    if (*p != '(') {
        while (p < end && isKeywordCharacter(u8(*p))) {
            ++p;
        }
        p = skipSpaces(p, end);
        return p < end && *p == '(' ? p + 1 : nullptr;
    }
    // Complex: scan the sub-entities for `name`.
    ++p;
    while (true) {
        p = skipSpaces(p, end);
        const char* word = p;
        while (p < end && isKeywordCharacter(u8(*p))) {
            ++p;
        }
        if (p == word) {
            return nullptr;
        }
        const std::string_view keyword(word, usize(p - word));
        p = skipSpaces(p, end);
        if (p >= end || *p != '(') {
            return nullptr;
        }
        if (keyword == name) {
            return p + 1;
        }
        p = reinterpret_cast<const char*>(
            skipGroup(reinterpret_cast<const u8*>(p), reinterpret_cast<const u8*>(end)));
    }
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

void appendUtf8(std::string& out, u32 codePoint) {
    if (codePoint < 0x80) {
        out.push_back(char(codePoint));
    } else if (codePoint < 0x800) {
        out.push_back(char(0xC0 | (codePoint >> 6)));
        out.push_back(char(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        out.push_back(char(0xE0 | (codePoint >> 12)));
        out.push_back(char(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(char(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (codePoint >> 18)));
        out.push_back(char(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(char(0x80 | (codePoint & 0x3F)));
    }
}

// Decodes runs of hexadecimal digits of `width` each, stopping at the \X0\ terminator.
const char* decodeExtended(const char* p, const char* end, int width, std::string& out) {
    u32 pendingHigh = 0;
    while (end - p >= width) {
        u32 value = 0;
        bool digits = true;
        for (int i = 0; i < width; ++i) {
            const int digit = hexDigit(p[i]);
            digits &= digit >= 0;
            value = value << 4 | u32(digit < 0 ? 0 : digit);
        }
        if (!digits) {
            break;
        }
        p += width;
        if (width == 4 && value >= 0xD800 && value < 0xDC00) {
            pendingHigh = value;
            continue;
        }
        if (width == 4 && value >= 0xDC00 && value < 0xE000 && pendingHigh != 0) {
            value = 0x10000 + ((pendingHigh - 0xD800) << 10) + (value - 0xDC00);
            pendingHigh = 0;
        }
        appendUtf8(out, value);
    }
    if (end - p >= 4 && std::string_view(p, 4) == "\\X0\\") {
        p += 4;
    }
    return p;
}

} // namespace

ParamCursor::ParamCursor(std::string_view record, std::string_view name) {
    const char* start = findParameters(record, name);
    end_ = record.data() + record.size();
    cursor_ = start != nullptr ? start : end_;
    valid_ = start != nullptr;
    skipSeparators();
}

void ParamCursor::skipSeparators() {
    while (cursor_ < end_ && (isSpace(u8(*cursor_)) || *cursor_ == ',')) {
        ++cursor_;
    }
}

bool ParamCursor::atEnd() const {
    return cursor_ >= end_ || *cursor_ == ')' || *cursor_ == ';';
}

bool ParamCursor::inList() const {
    return cursor_ < end_ && *cursor_ != ')';
}

bool ParamCursor::skip() {
    if (atEnd()) {
        return false;
    }
    int depth = 0;
    while (cursor_ < end_) {
        const char c = *cursor_;
        if (c == '\'') {
            ++cursor_;
            while (cursor_ < end_ && *cursor_ != '\'') {
                ++cursor_;
            }
        } else if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth == 0) {
                break;
            }
            --depth;
        } else if (c == ',' && depth == 0) {
            break;
        }
        ++cursor_;
    }
    skipSeparators();
    return true;
}

bool ParamCursor::readString(std::string& value) {
    value.clear();
    if (cursor_ >= end_ || *cursor_ != '\'') {
        return false;
    }
    const char* p = cursor_ + 1;
    while (p < end_) {
        const char c = *p;
        if (c == '\'') {
            if (p + 1 < end_ && p[1] == '\'') {
                value.push_back('\'');
                p += 2;
                continue;
            }
            cursor_ = p + 1;
            skipSeparators();
            return true;
        }
        if (c == '\\' && end_ - p >= 3) {
            const std::string_view head(p, 3);
            if (head == "\\X\\" && end_ - p >= 5) {
                const int high = hexDigit(p[3]), low = hexDigit(p[4]);
                if (high >= 0 && low >= 0) {
                    appendUtf8(value, u32(high << 4 | low));
                    p += 5;
                    continue;
                }
            } else if (end_ - p >= 4 && std::string_view(p, 4) == "\\X2\\") {
                p = decodeExtended(p + 4, end_, 4, value);
                continue;
            } else if (end_ - p >= 4 && std::string_view(p, 4) == "\\X4\\") {
                p = decodeExtended(p + 4, end_, 8, value);
                continue;
            }
        }
        value.push_back(c);
        ++p;
    }
    return false;
}

bool ParamCursor::readReal(double& value) {
    const char* p = cursor_;
    if (p < end_ && ((*p >= 'A' && *p <= 'Z'))) {
        // A typed value such as LENGTH_MEASURE(25.4).
        while (p < end_ && isKeywordCharacter(u8(*p))) {
            ++p;
        }
        if (p >= end_ || *p != '(') {
            return false;
        }
        p = skipSpaces(p + 1, end_);
        const std::from_chars_result result = std::from_chars(p, end_, value);
        if (result.ec != std::errc()) {
            return false;
        }
        p = result.ptr;
        while (p < end_ && *p != ')') {
            ++p;
        }
        cursor_ = p < end_ ? p + 1 : end_;
        skipSeparators();
        return true;
    }
    if (p < end_ && *p == '+') {
        ++p;
    }
    const std::from_chars_result result = std::from_chars(p, end_, value);
    if (result.ec != std::errc() && result.ec != std::errc::result_out_of_range) {
        return false;
    }
    cursor_ = result.ptr;
    skipSeparators();
    return true;
}

bool ParamCursor::readEnum(std::string_view& value) {
    if (cursor_ >= end_ || *cursor_ != '.') {
        return false;
    }
    const char* first = cursor_ + 1;
    const char* p = first;
    while (p < end_ && *p != '.') {
        ++p;
    }
    if (p >= end_) {
        return false;
    }
    value = {first, usize(p - first)};
    cursor_ = p + 1;
    skipSeparators();
    return true;
}

bool ParamCursor::readId(u32& id) {
    if (cursor_ >= end_ || *cursor_ != '#') {
        return false;
    }
    bool overflow = false;
    cursor_ = reinterpret_cast<const char*>(parseId(
        reinterpret_cast<const u8*>(cursor_ + 1), reinterpret_cast<const u8*>(end_), id, overflow));
    skipSeparators();
    return !overflow;
}

bool ParamCursor::enterList() {
    if (cursor_ >= end_ || *cursor_ != '(') {
        return false;
    }
    ++cursor_;
    skipSeparators();
    return true;
}

bool ParamCursor::leaveList() {
    while (inList()) {
        if (!skip()) {
            break;
        }
    }
    if (cursor_ >= end_) {
        return false;
    }
    ++cursor_;
    skipSeparators();
    return true;
}

} // namespace geo::import::step
