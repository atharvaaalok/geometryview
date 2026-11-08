// probe: runs the portable import stages on a real file and prints how long each took and a hash
// of exactly what it produced. Built natively and as WebAssembly, the hashes show the two builds
// agree bit for bit, and the times show what WebAssembly costs.
//
//   probe stl <file.stl> <out.geoblob>    imports a mesh into a cache file and hashes the file
//   probe step <file.step>                 tokenizes, reads the structure, finds definitions
#include "base/parallel.h"
#include "base/time.h"
#include "import.h"
#include "step/definitions.h"
#include "step/part21.h"
#include "step/structure.h"

#define XXH_INLINE_ALL
#include "base/xxhash.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace geo;

bool readFile(const char* path, std::vector<u8>& bytes) {
    const int fd = open(path, O_RDONLY);
    struct stat info{};
    if (fd < 0 || fstat(fd, &info) != 0) {
        return false;
    }
    bytes.resize(usize(info.st_size));
    usize done = 0;
    while (done < bytes.size()) {
        const ssize_t got = read(fd, bytes.data() + done, bytes.size() - done);
        if (got <= 0) {
            close(fd);
            return false;
        }
        done += usize(got);
    }
    close(fd);
    return true;
}

std::string hex(XXH128_hash_t hash) {
    return std::format("{:016x}{:016x}", hash.high64, hash.low64);
}

// Appends fields one by one, so padding inside structs never reaches the hash.
class Canonical {
public:
    template <typename T> void add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes_.append(reinterpret_cast<const char*>(&value), sizeof(T));
    }
    void add(std::string_view text) {
        add(u64(text.size()));
        bytes_.append(text);
    }
    void add(const simd_double3& v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }
    template <typename T> void addAll(const std::vector<T>& values) {
        add(u64(values.size()));
        for (const T& value : values) {
            add(value);
        }
    }
    std::string hash() const { return hex(XXH3_128bits(bytes_.data(), bytes_.size())); }

private:
    std::string bytes_;
};

int stlCommand(const char* source, const char* output) {
    std::vector<u8> file;
    if (!readFile(source, file)) {
        std::println(stderr, "could not read {}", source);
        return 1;
    }
    const int fd = open(output, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        std::println(stderr, "could not create {}", output);
        return 1;
    }
    import::ImportResult result;
    std::string error;
    const double start = nowSeconds();
    const bool ok = import::importMesh(file, fd, nullptr, result, error);
    const double seconds = nowSeconds() - start;
    close(fd);
    std::vector<u8> blob;
    if (!ok || !readFile(output, blob)) {
        std::println(stderr, "import failed: {}", error);
        return 1;
    }
    std::println("{{\"triangles\": {}, \"seconds\": {:.4f}, \"parse\": {:.4f}, \"pack\": {:.4f}, "
                 "\"blobBytes\": {}, \"threads\": {}, \"hash\": \"{}\"}}",
                 result.triangles, seconds, result.times.parse, result.times.pack, blob.size(),
                 parallelWidth(),
                 hex(XXH3_128bits(blob.data(), blob.size())));
    return 0;
}

int stepCommand(const char* source) {
    std::vector<u8> file;
    if (!readFile(source, file)) {
        std::println(stderr, "could not read {}", source);
        return 1;
    }
    import::step::Part21 index;
    std::vector<u8> scratch;
    std::string error;
    double start = nowSeconds();
    if (!import::step::indexPart21(file, index, scratch, error)) {
        std::println(stderr, "tokenizing failed: {}", error);
        return 1;
    }
    const double tokenize = nowSeconds() - start;
    start = nowSeconds();
    const import::step::Structure structure = import::step::readStructure(index);
    const double structureSeconds = nowSeconds() - start;
    start = nowSeconds();
    const import::step::Definitions definitions = import::step::buildDefinitions(index, structure);
    const double definitionsSeconds = nowSeconds() - start;

    Canonical canonical;
    canonical.add(index.maxId);
    canonical.add(index.needsWholeFile);
    canonical.addAll(index.ids);
    canonical.addAll(index.kinds);
    canonical.addAll(index.offsets);
    canonical.addAll(index.lengths);
    canonical.addAll(index.refStarts);
    canonical.addAll(index.refs);
    for (const auto& node : structure.nodes) {
        canonical.add(node.parent);
        canonical.add(node.subtreeEnd);
        canonical.add(node.firstOccurrence);
        canonical.add(node.occurrenceEnd);
        canonical.add(node.nameOffset);
        canonical.add(node.nameLength);
        canonical.add(node.kind);
    }
    canonical.add(std::string_view(structure.names));
    for (const auto& body : structure.bodies) {
        canonical.add(body.record);
        canonical.add(body.context);
        canonical.add(body.firstFace);
        canonical.add(body.faceCount);
        canonical.add(body.color);
        canonical.add(body.shellCount);
        canonical.add(body.closed);
        canonical.add(body.hasVoids);
    }
    for (const auto& face : structure.faces) {
        canonical.add(face.record);
        canonical.add(face.color);
        canonical.add(face.shell);
        canonical.add(face.flipped);
    }
    for (const auto& occurrence : structure.occurrences) {
        canonical.add(occurrence.body);
        canonical.add(occurrence.node);
        for (int column = 0; column < 3; ++column) {
            canonical.add(occurrence.world.linear.columns[column]);
        }
        canonical.add(occurrence.world.translation);
    }
    canonical.add(structure.needsWholeFile);
    for (const auto& item : definitions.items) {
        canonical.add(item.body);
        canonical.add(item.occurrenceCount);
        canonical.add(item.boundsMin);
        canonical.add(item.boundsMax);
        canonical.add(item.firstFace);
        canonical.add(item.faceCount);
        canonical.add(item.firstChunk);
        canonical.add(item.chunkCount);
    }
    canonical.addAll(definitions.definitionOfBody);
    for (const auto& chunk : definitions.chunks) {
        canonical.add(chunk.definition);
        canonical.add(chunk.firstFace);
        canonical.add(chunk.faceCount);
        canonical.add(chunk.wholeBody);
    }
    std::println("{{\"records\": {}, \"nodes\": {}, \"occurrences\": {}, \"definitions\": {}, "
                 "\"chunks\": {}, \"tokenize\": {:.4f}, \"structure\": {:.4f}, "
                 "\"definitionsSeconds\": {:.4f}, \"hash\": \"{}\"}}",
                 index.recordCount(), structure.nodes.size(), structure.occurrences.size(),
                 definitions.items.size(), definitions.chunks.size(), tokenize, structureSeconds,
                 definitionsSeconds, canonical.hash());
    return 0;
}

} // namespace

int main(int argc, const char** argv) {
    const std::string_view command = argc > 1 ? argv[1] : "";
    if (command == "stl" && argc == 4) {
        return stlCommand(argv[2], argv[3]);
    }
    if (command == "step" && argc == 3) {
        return stepCommand(argv[2]);
    }
    std::println(stderr, "usage: probe stl <file.stl> <out.geoblob>\n       probe step <file.step>");
    return 2;
}
