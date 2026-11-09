// The STEP importer in the browser: the portable StepImport, whose jobs the page hands to its own
// pool of OCCT Web Workers. The page calls, in turn:
//
//   step_open(size)              room for the file, in the heap, for the page to fill
//   step_refine(level, count)    optional, before step_begin: imports only `count` definitions,
//                                whose numbers the page writes at the room it returns, at `level`
//                                rather than the coarsest, and writes no model: a refinement
//   step_begin()                 indexes the file and writes the model: 0 if it will be sliced,
//                                1 if it needs a whole-file read, which the browser cannot do yet,
//                                or -1 on error (step_error says why)
//   step_next()                  a job's id, with its text at step_text() for step_text_size()
//                                bytes and its tolerances at step_request(); -1 if none is queued
//                                now but replies are due; -2 once every job is answered
//   step_payload(size)           room for a reply's payload, for the page to fill
//   step_reply(id, ...)          what a worker made of the job, with that payload
//   step_crash(id)               the job's worker trapped or ran out of memory
//   step_finish()                completes the cache file: step_blob() for step_blob_size() bytes
//   step_stats()                 timings and counts, as doubles (see below)
//   step_jobs()                  how many slices step_begin queued, for showing progress
//   step_take_segment(out)       a segment written since the last call, as its offset and size
//                                in the cache file, or 0 if none; step_read(offset, size) reads it,
//                                so the page can show the model while it imports
//
// An STL file needs no OCCT: after step_open, mesh_import() makes its cache file whole, to read at
// step_blob() as a STEP import's is. A binary STL file can instead be read in pieces, so it is
// never held whole: stl_open(size) gives room for a piece of stl_piece_size() bytes,
// stl_take(bytes) takes each in turn (0 on the first if the file must be read whole after all), and
// mesh_import_pieces() makes the cache file.
#include "import.h"
#include "occt/worker_protocol.h"
#include "step/slicing.h"
#include "stl/stl_reader.h"

#include <emscripten/emscripten.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geo;
using namespace geo::import;

namespace {

constexpr const char* kOutputPath = "/import.geoblob";

// Notes each segment as the importer writes it, for the page to take.
struct Written final : SegmentListener {
    std::vector<std::pair<u64, u64>> segments;
    usize taken = 0;
    void segmentWritten(u64 offset, u64 size) override { segments.emplace_back(offset, size); }
};

struct Session {
    std::vector<u8> file;
    // An STL file read in pieces.
    std::vector<u8> piece;
    u64 fileSize = 0;
    u64 taken = 0;
    std::optional<StlPieces> pieces;
    std::optional<ContentHash> hash;
    Written written;
    std::vector<u8> read;
    int output = -1;
    StepOptions options;
    StepStats stats;
    StepImport import;
    std::optional<SliceWriter> writer;
    std::unordered_map<u32, Job> pending;
    u32 nextId = 0;
    std::string text;
    std::vector<u8> payload;
    worker::Request request{};
    double tolerances[3] = {0, 0, 0};
    std::vector<u8> blob;
    std::string error;
    double values[17] = {};
    u32 jobs = 0;
};

// Reads the finished cache file back from the in-memory file system, then removes it.
bool readBack(Session& s) {
    const off_t size = lseek(s.output, 0, SEEK_END);
    s.blob.resize(usize(size));
    const bool read = pread(s.output, s.blob.data(), s.blob.size(), 0) == ssize_t(size);
    close(s.output);
    unlink(kOutputPath);
    s.output = -1;
    std::vector<u8>().swap(s.file); // no longer needed; assigning {} would keep its memory
    if (!read) {
        s.error = "The cache file could not be read back.";
    }
    return read;
}

std::unique_ptr<Session> session;

} // namespace

extern "C" {

/// The file is read into the session's own buffer, so it is in memory once, and outlives the
/// import, which keeps views into it.
EMSCRIPTEN_KEEPALIVE u8* step_open(u32 size) {
    session = std::make_unique<Session>();
    session->file.resize(size);
    return session->file.data();
}

EMSCRIPTEN_KEEPALIVE u32* step_refine(u32 level, u32 count) {
    Session& s = *session;
    s.options.level = level;
    s.options.definitions.assign(count, 0);
    return s.options.definitions.data();
}

EMSCRIPTEN_KEEPALIVE int step_begin() {
    Session& s = *session;
    s.output = open(kOutputPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (s.output < 0) {
        s.error = "The cache file could not be created.";
        return -1;
    }
    if (!s.import.begin(s.file, s.output, &s.written, s.options, s.stats, s.error)) {
        return -1;
    }
    if (s.import.needsWholeFile()) {
        s.error = "This file needs OCCT's whole-file read, which the browser does not do yet.";
        return 1;
    }
    s.jobs = u32(s.import.scheduler().start());
    s.writer.emplace(s.import.index());
    return 0;
}

EMSCRIPTEN_KEEPALIVE int step_next() {
    Session& s = *session;
    const std::optional<Job> job = s.import.scheduler().takeJob();
    if (!job) {
        return s.import.scheduler().finished() ? -2 : -1;
    }
    s.import.scheduler().prepare(*job, *s.writer, s.text, s.request);
    s.tolerances[0] = s.request.linearDeflection;
    s.tolerances[1] = s.request.angularDeflection;
    s.tolerances[2] = s.request.minSize;
    const u32 id = s.nextId++;
    s.pending.emplace(id, *job);
    return int(id);
}

EMSCRIPTEN_KEEPALIVE const char* step_text() {
    return session->text.data();
}
EMSCRIPTEN_KEEPALIVE u32 step_text_size() {
    return u32(session->text.size());
}
/// The job's linear deflection, angular deflection and minimum size.
EMSCRIPTEN_KEEPALIVE const double* step_request() {
    return session->tolerances;
}

EMSCRIPTEN_KEEPALIVE u8* step_payload(u32 size) {
    session->payload.resize(size);
    return session->payload.data();
}

EMSCRIPTEN_KEEPALIVE void step_reply(u32 id, u32 status, u32 faces, u32 unmapped, u32 unmeshed,
                                     double read, double transfer, double mesh) {
    Session& s = *session;
    std::vector<u8> payload = std::move(s.payload);
    s.payload = {};
    const auto it = s.pending.find(id);
    if (it == s.pending.end()) {
        return;
    }
    const Job job = it->second;
    s.pending.erase(it);
    worker::Reply reply{};
    reply.magic = worker::kMagic;
    reply.status = status;
    reply.faceCount = faces;
    reply.unmappedFaces = unmapped;
    reply.unmeshedFaces = unmeshed;
    reply.payloadBytes = u32(payload.size());
    reply.readSeconds = read;
    reply.transferSeconds = transfer;
    reply.meshSeconds = mesh;
    s.import.scheduler().onReply(job, reply, std::move(payload));
}

EMSCRIPTEN_KEEPALIVE void step_crash(u32 id) {
    Session& s = *session;
    const auto it = s.pending.find(id);
    if (it == s.pending.end()) {
        return;
    }
    const Job job = it->second;
    s.pending.erase(it);
    s.import.scheduler().onCrash(job);
}

EMSCRIPTEN_KEEPALIVE int step_finish() {
    Session& s = *session;
    if (!s.import.finish(s.stats, s.error)) {
        return -1;
    }
    return readBack(s) ? 0 : -1;
}

EMSCRIPTEN_KEEPALIVE u32 step_jobs() {
    return session->jobs;
}

EMSCRIPTEN_KEEPALIVE int step_take_segment(double* out) {
    Written& w = session->written;
    if (w.taken >= w.segments.size()) {
        return 0;
    }
    out[0] = double(w.segments[w.taken].first);
    out[1] = double(w.segments[w.taken].second);
    ++w.taken;
    return 1;
}

/// `size` bytes of the cache file being written, from `offset`, or null if they cannot be read.
EMSCRIPTEN_KEEPALIVE const u8* step_read(double offset, u32 size) {
    Session& s = *session;
    // Once finished, the file is no longer open, and its bytes are in the blob.
    if (s.output < 0) {
        return offset >= 0 && u64(offset) + size <= s.blob.size() ? s.blob.data() + u64(offset)
                                                                  : nullptr;
    }
    s.read.resize(size);
    return pread(s.output, s.read.data(), size, off_t(offset)) == ssize_t(size) ? s.read.data()
                                                                                : nullptr;
}

EMSCRIPTEN_KEEPALIVE int mesh_import() {
    Session& s = *session;
    s.output = open(kOutputPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (s.output < 0) {
        s.error = "The cache file could not be created.";
        return -1;
    }
    ImportResult result;
    if (!importMesh(s.file, s.output, nullptr, result, s.error)) {
        close(s.output);
        unlink(kOutputPath);
        s.output = -1;
        return -1;
    }
    s.stats.triangles = result.triangles;
    s.stats.faces = result.faces;
    s.stats.meshlets = result.meshlets;
    return readBack(s) ? 0 : -1;
}

constexpr u32 kStlPiece = 16 << 20;

EMSCRIPTEN_KEEPALIVE u8* stl_open(double fileSize) {
    session = std::make_unique<Session>();
    Session& s = *session;
    s.fileSize = u64(fileSize);
    s.piece.resize(kStlPiece);
    s.pieces.emplace();
    s.hash.emplace();
    return s.piece.data();
}

EMSCRIPTEN_KEEPALIVE u32 stl_piece_size() {
    return kStlPiece;
}

/// Takes the next `size` bytes of the file, at stl_open's room. Returns 1 to go on, or 0 on the
/// first piece if the file is not a binary STL that can be read in pieces.
EMSCRIPTEN_KEEPALIVE int stl_take(u32 size) {
    Session& s = *session;
    const std::span<const u8> bytes(s.piece.data(), std::min<usize>(size, s.piece.size()));
    if (s.taken == 0 && !s.pieces->begin(bytes, s.fileSize)) {
        return 0;
    }
    s.taken += bytes.size();
    s.hash->update(bytes);
    s.pieces->append(bytes);
    return 1;
}

EMSCRIPTEN_KEEPALIVE int mesh_import_pieces() {
    Session& s = *session;
    std::vector<u8>().swap(s.piece);
    TriangleSoup soup;
    if (!s.pieces->finish(soup, s.error)) {
        return -1;
    }
    s.pieces.reset();
    s.output = open(kOutputPath, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (s.output < 0) {
        s.error = "The cache file could not be created.";
        return -1;
    }
    ImportResult result;
    if (!importSoup(std::move(soup), s.hash->digest(), s.output, nullptr, result, s.error)) {
        close(s.output);
        unlink(kOutputPath);
        s.output = -1;
        return -1;
    }
    s.stats.triangles = result.triangles;
    s.stats.faces = result.faces;
    s.stats.meshlets = result.meshlets;
    return readBack(s) ? 0 : -1;
}

EMSCRIPTEN_KEEPALIVE const u8* step_blob() {
    return session->blob.data();
}
EMSCRIPTEN_KEEPALIVE u32 step_blob_size() {
    return u32(session->blob.size());
}
EMSCRIPTEN_KEEPALIVE const char* step_error() {
    return session ? session->error.c_str() : "";
}

/// tokenize, structure and definition seconds; OCCT's read, transfer and mesh seconds summed over
/// jobs; pack seconds; then records, bodies, definitions, faces, triangles, failed faces, unmapped
/// faces, crashed workers, meshlets, faces meshed coarser.
EMSCRIPTEN_KEEPALIVE const double* step_stats() {
    Session& s = *session;
    const StepStats& t = s.stats;
    const double values[17] = {
        t.tokenizeSeconds,     t.structureSeconds,      t.definitionSeconds,
        t.readSeconds,         t.transferSeconds,       t.meshSeconds,
        t.packSeconds,         double(t.records),       double(t.bodies),
        double(t.definitions), double(t.faces),         double(t.triangles),
        double(t.failedFaces), double(t.unmappedFaces), double(t.crashedWorkers),
        double(t.meshlets),    double(t.coarserFaces)};
    std::copy(std::begin(values), std::end(values), s.values);
    return s.values;
}

} // extern "C"
