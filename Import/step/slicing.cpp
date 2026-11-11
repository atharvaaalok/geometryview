#include "step/slicing.h"

#include "base/time.h"
#include "import.h"
#include "mesh/packer.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <tuple>

namespace geo::import {

using namespace step;
using worker::FaceHeader;
using worker::Reply;
using worker::ReplyStatus;
using worker::Request;
using worker::RequestKind;

namespace {

// Tolerances as multiples of the model's diagonal. See Docs/PLAN.md.
constexpr double kLinearOfDiagonal = 2e-4;
constexpr double kMinSizeOfDiagonal = 1.5e-3;
constexpr double kBaseAngular = 0.35;
constexpr double kMinAngular = 0.05;
// Rounding to the vertex grid must stay well inside the tessellation's own error.
constexpr double kGridStepOfDeflection = 0.5;
constexpr double kRetryToleranceScale = 0.125;
// A worker that crashes or runs out of time on one face alone has usually run out of memory or
// time meshing it this finely, as on a surface fitted to a scan, rippled finer than the tolerance.
// It is tried at four, then sixteen times the tolerances: four times took one such face from
// 2.8 GB and 38 s to 480 MB and 5 s.
constexpr u8 kCoarserRungs = 2;
// Each level halves the chord error, so four levels are sixteen times finer.
constexpr u32 kMaxLevel = 4;


double tetrahedronVolume(const float* a, const float* b, const float* c) {
    return (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) -
            double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) +
            double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) /
           6.0;
}

// Makes every shell of a closed body face the right way: the outer shell encloses positive volume
// and each cavity negative. The file's own flags cannot be trusted for this, because some writers
// reverse a cavity twice. OCCT's ShapeFix corrects the same cases when it sees the whole solid.
void orientShells(IndexedMesh& mesh, std::span<const FaceUse> faces, const Body& body) {
    if (body.shellCount == 0) {
        return;
    }
    std::vector<double> volumes(body.shellCount, 0.0);
    for (usize t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const u32 shell = faces[mesh.faceOfVertex[mesh.indices[t]]].shell;
        volumes[shell] += tetrahedronVolume(mesh.positions.data() + usize(mesh.indices[t]) * 3,
                                            mesh.positions.data() + usize(mesh.indices[t + 1]) * 3,
                                            mesh.positions.data() + usize(mesh.indices[t + 2]) * 3);
    }
    // The outer shell is the largest, which is not always the one the file lists first.
    u32 outer = 0;
    for (u32 shell = 1; shell < body.shellCount; ++shell) {
        if (std::fabs(volumes[shell]) > std::fabs(volumes[outer])) {
            outer = shell;
        }
    }
    std::vector<u8> flip(body.shellCount);
    bool any = false;
    for (u32 shell = 0; shell < body.shellCount; ++shell) {
        const bool cavity = body.hasVoids && shell != outer;
        flip[shell] = cavity ? volumes[shell] > 0.0 : volumes[shell] < 0.0;
        any |= flip[shell] != 0;
    }
    if (!any) {
        return;
    }
    for (usize t = 0; t + 2 < mesh.indices.size(); t += 3) {
        if (flip[faces[mesh.faceOfVertex[mesh.indices[t]]].shell] != 0) {
            std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
        }
    }
    for (usize v = 0; v < mesh.faceOfVertex.size(); ++v) {
        if (flip[faces[mesh.faceOfVertex[v]].shell] != 0) {
            mesh.normals[v * 3] = -mesh.normals[v * 3];
            mesh.normals[v * 3 + 1] = -mesh.normals[v * 3 + 1];
            mesh.normals[v * 3 + 2] = -mesh.normals[v * 3 + 2];
        }
    }
}

Tolerances tolerancesFor(double diagonal, u32 level) {
    const double scale = std::ldexp(1.0, -int(level));
    return {diagonal * kLinearOfDiagonal * scale,
            std::max(kMinAngular, kBaseAngular * std::sqrt(scale)),
            diagonal * kMinSizeOfDiagonal * scale};
}

std::array<float, 12> toColumns(const Affine& transform) {
    const simd_double3x3& m = transform.linear;
    const simd_double3 t = transform.translation;
    return {float(m.columns[0].x),
            float(m.columns[0].y),
            float(m.columns[0].z),
            float(m.columns[1].x),
            float(m.columns[1].y),
            float(m.columns[1].z),
            float(m.columns[2].x),
            float(m.columns[2].y),
            float(m.columns[2].z),
            float(t.x),
            float(t.y),
            float(t.z)};
}

double worldDiagonal(const Structure& structure, const Definitions& definitions) {
    simd_double3 lo = {INFINITY, INFINITY, INFINITY}, hi = {-INFINITY, -INFINITY, -INFINITY};
    for (const BodyOccurrence& occurrence : structure.occurrences) {
        const Definition& d = definitions.items[definitions.definitionOfBody[occurrence.body]];
        if (d.boundsMin[0] > d.boundsMax[0]) {
            continue;
        }
        for (int corner = 0; corner < 8; ++corner) {
            const simd_double3 local = {corner & 1 ? d.boundsMax[0] : d.boundsMin[0],
                                        corner & 2 ? d.boundsMax[1] : d.boundsMin[1],
                                        corner & 4 ? d.boundsMax[2] : d.boundsMin[2]};
            const simd_double3 world =
                simd_mul(occurrence.world.linear, local) + occurrence.world.translation;
            lo = simd_min(lo, world);
            hi = simd_max(hi, world);
        }
    }
    const double diagonal = simd_length(hi - lo);
    return std::isfinite(diagonal) && diagonal > 0.0 ? diagonal : 1.0;
}

ModelData modelFrom(const Structure& structure, const Definitions& definitions) {
    ModelData model;
    model.definitions.reserve(definitions.items.size());
    for (const Definition& d : definitions.items) {
        format::DefinitionInfo info{};
        info.boundsMin = d.boundsMin;
        info.boundsMax = d.boundsMax;
        info.faceCount = d.faceCount;
        info.flags = structure.bodies[d.body].closed ? 0 : u32(kDefinitionOpen);
        model.definitions.push_back(info);
    }
    model.occurrences.reserve(structure.occurrences.size());
    for (const BodyOccurrence& occurrence : structure.occurrences) {
        format::OccurrenceRecord record{};
        record.transform = toColumns(occurrence.world);
        record.definition = definitions.definitionOfBody[occurrence.body];
        record.node = occurrence.node;
        record.color = structure.bodies[occurrence.body].color;
        model.occurrences.push_back(record);
    }
    model.nodes.reserve(structure.nodes.size());
    for (const TreeNode& node : structure.nodes) {
        format::NodeRecord record{};
        record.parent = node.parent;
        record.subtreeEnd = node.subtreeEnd;
        record.firstOccurrence = node.firstOccurrence;
        record.occurrenceEnd = node.occurrenceEnd;
        record.nameOffset = node.nameOffset;
        record.nameLength = node.nameLength;
        record.kind = u32(node.kind);
        model.nodes.push_back(record);
    }
    model.strings = structure.names;
    return model;
}

double micros(const std::atomic<u64>& value) {
    return double(value.load(std::memory_order_relaxed)) * 1e-6;
}

void addTime(std::atomic<u64>& total, double seconds) {
    total.fetch_add(u64(seconds * 1e6), std::memory_order_relaxed);
}

} // namespace

// ---- Faces back from workers ------------------------------------------------------------------

void FaceSink::reserve(usize vertices, usize triangles) {
    mesh.positions.reserve(vertices * 3);
    mesh.normals.reserve(vertices * 3);
    mesh.faceOfVertex.reserve(vertices);
    mesh.indices.reserve(triangles * 3);
}

void FaceSink::add(const FaceHeader& header, const u8* positions, const u8* normals,
                   const u8* indices, u32 face, bool flipped) {
    const u32 base = u32(mesh.faceOfVertex.size());
    const usize floats = usize(header.vertexCount) * 3;
    const usize positionStart = mesh.positions.size();
    const usize indexStart = mesh.indices.size();
    mesh.positions.resize(positionStart + floats);
    mesh.normals.resize(positionStart + floats);
    std::memcpy(mesh.positions.data() + positionStart, positions, floats * sizeof(float));
    std::memcpy(mesh.normals.data() + positionStart, normals, floats * sizeof(float));
    mesh.faceOfVertex.insert(mesh.faceOfVertex.end(), header.vertexCount, face);
    mesh.indices.resize(indexStart + usize(header.triangleCount) * 3);
    std::memcpy(mesh.indices.data() + indexStart, indices,
                usize(header.triangleCount) * 3 * sizeof(u32));
    if (flipped) {
        for (usize n = positionStart; n < positionStart + floats; ++n) {
            mesh.normals[n] = -mesh.normals[n];
        }
    }
    for (usize t = indexStart; t < mesh.indices.size(); t += 3) {
        mesh.indices[t] += base;
        mesh.indices[t + 1] += base;
        mesh.indices[t + 2] += base;
        if (flipped) {
            std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
        }
    }
}

// ---- The scheduler -----------------------------------------------------------------------------

SliceScheduler::SliceScheduler(const Part21& index, const Structure& structure,
                               const Definitions& definitions, BlobStream& stream,
                               SegmentListener* listener, const StepOptions& options,
                               const Tolerances& tolerances)
    : index_(index), structure_(structure), definitions_(definitions), stream_(stream),
      listener_(listener), options_(options), tolerances_(tolerances),
      progress_(definitions.items.size()) {}

// Definitions placed most often and largest go first, since they fill the screen fastest.
usize SliceScheduler::start() {
    std::vector<u32> order;
    if (!options_.definitions.empty()) {
        for (const u32 d : options_.definitions) {
            if (d < definitions_.items.size()) {
                order.push_back(d);
            }
        }
    } else {
        order.resize(definitions_.items.size());
        for (u32 d = 0; d < order.size(); ++d) {
            order[d] = d;
        }
    }
    const auto weight = [&](u32 d) {
        const Definition& item = definitions_.items[d];
        double size = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
            const double extent = double(item.boundsMax[axis]) - item.boundsMin[axis];
            size += extent * extent;
        }
        return size * item.occurrenceCount;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](u32 a, u32 b) { return weight(a) > weight(b); });
    std::lock_guard lock(mutex_);
    totalFaces_ = 0;
    for (const u32 d : order) {
        const Definition& item = definitions_.items[d];
        totalFaces_ += item.faceCount;
        if (item.chunkCount == 0) {
            continue;
        }
        progress_[d].pendingJobs.store(item.chunkCount, std::memory_order_relaxed);
        for (u32 c = 0; c < item.chunkCount; ++c) {
            const Chunk& chunk = definitions_.chunks[item.firstChunk + c];
            queue_.push_back(
                {chunk.definition, chunk.firstFace, chunk.faceCount, chunk.wholeBody, false});
        }
    }
    outstanding_ = queue_.size();
    return queue_.size();
}

std::optional<Job> SliceScheduler::waitForJob() {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [&] { return !queue_.empty() || outstanding_ == 0; });
    if (queue_.empty()) {
        return std::nullopt;
    }
    const Job job = queue_.front();
    queue_.pop_front();
    return job;
}

std::optional<Job> SliceScheduler::takeJob() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
        return std::nullopt;
    }
    const Job job = queue_.front();
    queue_.pop_front();
    return job;
}

bool SliceScheduler::finished() {
    std::lock_guard lock(mutex_);
    return outstanding_ == 0;
}

void SliceScheduler::finishJob() {
    std::lock_guard lock(mutex_);
    --outstanding_;
    if (outstanding_ == 0) {
        wake_.notify_all();
    }
}

void SliceScheduler::requeue(std::initializer_list<Job> jobs) {
    {
        std::lock_guard lock(mutex_);
        outstanding_ += jobs.size();
        for (const Job& job : jobs) {
            queue_.push_front(job);
        }
    }
    wake_.notify_all();
}

void SliceScheduler::prepare(const Job& job, SliceWriter& writer, std::string& text,
                             Request& request) const {
    const Definition& definition = definitions_.items[job.definition];
    const Body& body = structure_.bodies[definition.body];
    if (job.wholeBody) {
        writeBodySlice(index_, body.record, body.context, writer.walker_, text);
    } else {
        writer.faceRecords_.clear();
        for (u32 f = 0; f < job.faceCount; ++f) {
            writer.faceRecords_.push_back(definitions_.faces[job.firstFace + f].record);
        }
        writeSlice(index_, writer.faceRecords_, body.context, writer.walker_, text);
    }
    request = {};
    request.magic = worker::kMagic;
    request.kind = u32(RequestKind::Slice);
    request.textBytes = text.size();
    // A face that would not mesh is usually smaller than the minimum edge, so the retry of a whole
    // body works finer. Only the faces still missing are taken from it.
    const double scale = job.retry ? kRetryToleranceScale : std::ldexp(1.0, 2 * job.coarser);
    request.linearDeflection = tolerances_.linear * scale;
    request.angularDeflection = tolerances_.angular;
    request.minSize = tolerances_.minSize * scale;
}

void SliceScheduler::onReply(const Job& job, const Reply& reply, std::vector<u8> payload) {
    if (reply.status == u32(ReplyStatus::OutOfMemory)) {
        outOfMemory(job);
        return;
    }
    if (reply.status != u32(ReplyStatus::Ok)) {
        retry(job);
        return;
    }
    addTime(readMicros_, reply.readSeconds);
    addTime(transferMicros_, reply.transferSeconds);
    addTime(meshMicros_, reply.meshSeconds);
    unmappedFaces_.fetch_add(reply.unmappedFaces, std::memory_order_relaxed);
    complete(job, std::move(payload));
}

void SliceScheduler::onCrash(const Job& job) {
    crashedWorkers_.fetch_add(1, std::memory_order_relaxed);
    outOfMemory(job);
}

// A crash, a timeout or a failed allocation: the slice was too much for its worker, so a face
// alone is tried coarser before the ladder goes on.
void SliceScheduler::outOfMemory(const Job& job) {
    if (job.faceCount == 1 && !job.retry && job.coarser < kCoarserRungs) {
        Job coarser = job;
        ++coarser.coarser;
        requeue({coarser});
        finishJob();
        return;
    }
    retry(job);
}

void SliceScheduler::abandon(const Job& job, const std::string& error) {
    fail(error);
    failJob(job);
}

// The failure ladder: halve the slice until one face is alone. `finishDefinition` then gives any
// face still without triangles to OCCT as part of the whole body.
void SliceScheduler::retry(const Job& job) {
    if (job.retry) {
        failJob(job);
        return;
    }
    if (job.faceCount > 1) {
        // A whole body that fails is split into faces, which no longer need its shell.
        const u32 half = job.faceCount / 2;
        progress_[job.definition].pendingJobs.fetch_add(1, std::memory_order_relaxed);
        requeue({Job{job.definition, job.firstFace, half, false, false},
                 Job{job.definition, job.firstFace + half, job.faceCount - half, false, false}});
        finishJob();
        return;
    }
    failJob(job);
}

void SliceScheduler::failJob(const Job& job) {
    reportProgress(job.faceCount);
    if (progress_[job.definition].pendingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        finishDefinition(job.definition);
    }
    finishJob();
}

void SliceScheduler::complete(const Job& job, std::vector<u8> payload) {
    if (job.coarser != 0) {
        coarserFaces_.fetch_add(job.faceCount, std::memory_order_relaxed);
    }
    DefinitionProgress& state = progress_[job.definition];
    {
        std::lock_guard lock(state.mutex);
        state.results.push_back({std::move(payload), job.firstFace, job.retry});
    }
    reportProgress(job.retry ? 0 : job.faceCount);
    if (state.pendingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        finishDefinition(job.definition);
    }
    finishJob();
}

// Every job for a definition is done, and only this caller can touch its results. If a face has
// no triangles, OCCT gets one chance to mesh it as part of the real solid.
void SliceScheduler::finishDefinition(u32 d) {
    DefinitionProgress& state = progress_[d];
    if (!state.triedWholeBody && countMissingFaces(d) != 0) {
        state.triedWholeBody = true;
        const Definition& definition = definitions_.items[d];
        state.pendingJobs.store(1, std::memory_order_relaxed);
        requeue({Job{d, definition.firstFace, definition.faceCount, true, true}});
        return;
    }
    packDefinition(d);
}

std::vector<std::pair<u32, u32>> SliceScheduler::faceIds(const Definition& definition) const {
    std::vector<std::pair<u32, u32>> ids(definition.faceCount);
    for (u32 f = 0; f < definition.faceCount; ++f) {
        ids[f] = {index_.ids[definitions_.faces[definition.firstFace + f].record], f};
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

u32 SliceScheduler::countMissingFaces(u32 d) const {
    const Definition& definition = definitions_.items[d];
    const std::vector<std::pair<u32, u32>> ids = faceIds(definition);
    std::vector<u8> meshed(definition.faceCount, 0);
    for (const Result& result : progress_[d].results) {
        forEachFace(result.payload, [&](const FaceHeader& header, auto, auto, auto) {
            const auto it = std::lower_bound(ids.begin(), ids.end(),
                                             std::pair<u32, u32>(header.sourceId, 0));
            if (it != ids.end() && it->first == header.sourceId && header.triangleCount != 0) {
                meshed[it->second] = 1;
            }
        });
    }
    return u32(std::count(meshed.begin(), meshed.end(), 0));
}

void SliceScheduler::reportProgress(u32 faces) {
    const u32 done = doneFaces_.fetch_add(faces, std::memory_order_relaxed) + faces;
    if (listener_ != nullptr) {
        listener_->progress(done, totalFaces_);
    }
}

void SliceScheduler::packDefinition(u32 d) {
    const double start = nowSeconds();
    const Definition& definition = definitions_.items[d];
    const Body& body = structure_.bodies[definition.body];
    DefinitionProgress& state = progress_[d];
    const std::span<const FaceUse> faces(definitions_.faces.data() + definition.firstFace,
                                         definition.faceCount);

    const std::vector<std::pair<u32, u32>> faceOfId = faceIds(definition);

    // Slices are read in face order, so the mesh does not depend on which worker answered first,
    // and before any whole-body retry, so a retry only ever fills gaps.
    std::sort(state.results.begin(), state.results.end(), [](const Result& a, const Result& b) {
        return std::tie(a.retry, a.firstFace) < std::tie(b.retry, b.firstFace);
    });

    usize vertices = 0, triangles = 0;
    for (const Result& result : state.results) {
        forEachFace(result.payload, [&](const FaceHeader& header, auto, auto, auto) {
            vertices += header.vertexCount;
            triangles += header.triangleCount;
        });
    }
    FaceSink sink;
    sink.reserve(vertices, triangles);
    std::vector<u32> owner(faces.size(), 0xFFFFFFFF);
    for (u32 r = 0; r < state.results.size(); ++r) {
        const Result& result = state.results[r];
        const u32 fallback = result.firstFace - definition.firstFace;
        const bool valid =
            forEachFace(result.payload, [&](const FaceHeader& header, const u8* positions,
                                            const u8* normals, const u8* indices) {
                const auto it = std::lower_bound(faceOfId.begin(), faceOfId.end(),
                                                 std::pair<u32, u32>(header.sourceId, 0));
                const bool known = it != faceOfId.end() && it->first == header.sourceId;
                const u32 face = known ? it->second : fallback;
                if (header.triangleCount == 0 || face >= faces.size() ||
                    (owner[face] != 0xFFFFFFFF && owner[face] != r)) {
                    return;
                }
                owner[face] = r;
                sink.add(header, positions, normals, indices, face, faces[face].flipped);
            });
        if (!valid) {
            fail("An import worker returned damaged data.");
        }
    }
    state.results.clear();
    const u32 missing = u32(std::count(owner.begin(), owner.end(), 0xFFFFFFFF));
    failedFaces_.fetch_add(missing, std::memory_order_relaxed);

    IndexedMesh& mesh = sink.mesh;
    mesh.faceCount = definition.faceCount;
    mesh.closed = body.closed;
    mesh.faceOrderIsFixed = true;
    mesh.maxGridStep = float(tolerances_.linear * kGridStepOfDeflection);
    if (mesh.closed) {
        orientShells(mesh, faces, body);
    }

    PackedDefinition packed = packMesh(mesh);
    triangles_.fetch_add(packed.triangles.size(), std::memory_order_relaxed);
    meshlets_.fetch_add(packed.meshlets.size(), std::memory_order_relaxed);
    std::vector<format::FaceRecord> records(faces.size());
    for (u32 f = 0; f < faces.size(); ++f) {
        records[f] = {index_.ids[faces[f].record], faces[f].color};
    }
    addTime(packMicros_, nowSeconds() - start);
    if (!stream_.addDefinition(d, options_.level, std::move(packed), std::move(records))) {
        fail(stream_.error());
    }
}

void SliceScheduler::fail(const std::string& message) {
    std::lock_guard lock(mutex_);
    if (firstError_.empty()) {
        firstError_ = message;
    }
}

bool SliceScheduler::finish(StepStats& stats, std::string& error) {
    stats.readSeconds = micros(readMicros_);
    stats.transferSeconds = micros(transferMicros_);
    stats.meshSeconds = micros(meshMicros_);
    stats.packSeconds = micros(packMicros_);
    stats.failedFaces = failedFaces_;
    stats.unmappedFaces = unmappedFaces_;
    stats.crashedWorkers = crashedWorkers_;
    stats.coarserFaces = coarserFaces_;
    stats.triangles = usize(triangles_.load()); // far below 2^32 where usize is 32-bit
    stats.meshlets = usize(meshlets_.load());
    std::lock_guard lock(mutex_);
    if (!firstError_.empty()) {
        error = firstError_;
        return false;
    }
    return true;
}

// ---- The import in stages ----------------------------------------------------------------------

bool StepImport::begin(std::span<const u8> file, int outputFd, SegmentListener* listener,
                       const StepOptions& options, StepStats& stats, std::string& error) {
    file_ = file;
    double lap = nowSeconds();
    const auto stage = [&](double& seconds) {
        const double now = nowSeconds();
        seconds = now - lap;
        lap = now;
    };

    if (!indexPart21(file, index_, scratch_, error)) {
        return false;
    }
    stats.records = index_.recordCount();
    stage(stats.tokenizeSeconds);

    structure_ = readStructure(index_);
    stats.warnings = std::move(structure_.warnings);
    stats.bodies = structure_.bodies.size();
    stats.occurrences = structure_.occurrences.size();
    stage(stats.structureSeconds);

    if (index_.needsWholeFile || structure_.needsWholeFile || structure_.occurrences.empty()) {
        needsWholeFile_ = true;
        return true;
    }

    definitions_ = buildDefinitions(index_, structure_);
    stats.definitions = definitions_.items.size();
    stats.chunks = definitions_.chunks.size();
    stats.faces = definitions_.faces.size();
    stage(stats.definitionSeconds);

    const Tolerances tolerances =
        tolerancesFor(worldDiagonal(structure_, definitions_), options.level);
    stats.linearDeflection = tolerances.linear;
    stats.angularDeflection = tolerances.angular;
    stats.minSize = tolerances.minSize;

    stream_.emplace(outputFd, listener);
    if (options.definitions.empty()) {
        ModelData model = modelFrom(structure_, definitions_);
        const Tolerances base = tolerancesFor(worldDiagonal(structure_, definitions_), 0);
        model.info.linearDeflection = base.linear;
        model.info.angularDeflection = base.angular;
        model.info.maxLevel = kMaxLevel;
        if (!stream_->writeModel(model)) {
            error = stream_->error();
            return false;
        }
    }
    scheduler_.emplace(index_, structure_, definitions_, *stream_, listener, options, tolerances);
    return true;
}

bool StepImport::finish(StepStats& stats, std::string& error) {
    if (!scheduler_->finish(stats, error)) {
        return false;
    }
    if (stats.triangles == 0 && stats.faces != 0) {
        error = "None of this file's surfaces could be read.";
        return false;
    }
    if (!stream_->finish(hashContents(file_))) {
        error = stream_->error();
        return false;
    }
    return true;
}

bool StepImport::finishStructure(std::string& error) {
    if (!stream_->finish(hashContents(file_))) {
        error = stream_->error();
        return false;
    }
    return true;
}

bool looksLikeStep(std::span<const u8> file) {
    usize first = 0;
    while (
        first < file.size() && first < 64 &&
        (file[first] == ' ' || file[first] == '\n' || file[first] == '\r' || file[first] == '\t')) {
        ++first;
    }
    static constexpr std::string_view kSignature = "ISO-10303-21";
    return file.size() - first >= kSignature.size() &&
           std::memcmp(file.data() + first, kSignature.data(), kSignature.size()) == 0;
}

} // namespace geo::import
