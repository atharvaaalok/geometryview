#include "step/step_import.h"

#include "base/time.h"
#include "import.h"
#include "mesh/indexed_mesh.h"
#include "mesh/packer.h"
#include "occt/worker_protocol.h"
#include "step/definitions.h"
#include "step/part21.h"
#include "step/structure.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <format>
#include <libproc.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace geo::import {
namespace {

using namespace step;
using worker::FaceHeader;
using worker::Reply;
using worker::ReplyStatus;
using worker::Request;
using worker::RequestKind;

// Tolerances as multiples of the model's diagonal. See Docs/PLAN.md.
constexpr double kLinearOfDiagonal = 2e-4;
constexpr double kMinSizeOfDiagonal = 1.5e-3;
constexpr double kBaseAngular = 0.35;
constexpr double kMinAngular = 0.05;
// Rounding to the vertex grid must stay well inside the tessellation's own error.
constexpr double kGridStepOfDeflection = 0.5;
constexpr double kRetryToleranceScale = 0.125;
// Each level halves the chord error, so four levels are sixteen times finer.
constexpr u32 kMaxLevel = 4;

constexpr int kWatchIntervalMs = 200;
constexpr double kSliceTimeoutSeconds = 120.0;
constexpr double kWholeFileTimeoutSeconds = 1800.0;
// One face of a heavy surface was measured at 2.9 GB, so a limit must leave room for that.
constexpr u64 kMinWorkerMemoryLimit = u64(4) << 30;
constexpr u64 kWorkerRecycleFootprint = u64(512) << 20;
constexpr int kFirstHighFd = 32;

u64 workerMemoryLimit() {
    u64 memory = 0;
    usize size = sizeof(memory);
    sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);
    return std::max(kMinWorkerMemoryLimit, memory / 8);
}

usize performanceCores() {
    int cores = 0;
    usize size = sizeof(cores);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &cores, &size, nullptr, 0) != 0 || cores <= 0) {
        return std::max<usize>(1, std::thread::hardware_concurrency());
    }
    return usize(cores);
}

class Descriptor {
public:
    Descriptor() = default;
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// Moves a descriptor out of the range a worker uses, so the spawn's dup2 calls cannot collide.
int moveHigh(int fd) {
    const int moved = fcntl(fd, F_DUPFD_CLOEXEC, kFirstHighFd);
    close(fd);
    return moved;
}

bool makePipe(Descriptor& readEnd, Descriptor& writeEnd) {
    int ends[2];
    if (pipe(ends) != 0) {
        return false;
    }
    readEnd = Descriptor(moveHigh(ends[0]));
    writeEnd = Descriptor(moveHigh(ends[1]));
    return readEnd.get() >= 0 && writeEnd.get() >= 0;
}

// One OCCT worker process. It lives while there is work, and a request that kills it costs only
// that request.
class WorkerProcess {
public:
    ~WorkerProcess() { stop(); }

    bool running() const { return pid_ != 0; }

    bool start(const std::string& path, int sourceFd, std::string& error) {
        Descriptor requestRead, replyWrite;
        if (!makePipe(requestRead, request_) || !makePipe(reply_, replyWrite)) {
            error = "The import workers could not be connected.";
            return false;
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, requestRead.get(), worker::kRequestFd);
        posix_spawn_file_actions_adddup2(&actions, replyWrite.get(), worker::kReplyFd);
        if (sourceFd >= 0) {
            posix_spawn_file_actions_adddup2(&actions, sourceFd, worker::kSourceFd);
        }
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);

        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);

        std::array<char*, 2> arguments = {const_cast<char*>(path.c_str()), nullptr};
        const int status =
            posix_spawn(&pid_, path.c_str(), &actions, &attributes, arguments.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        if (status != 0) {
            pid_ = 0;
            error = std::format("The import worker could not start: {}.", std::strerror(status));
            return false;
        }
        return true;
    }

    bool send(const Request& request, std::string_view text) {
        return writeAll(&request, sizeof(request)) && writeAll(text.data(), text.size());
    }

    // Waits for the reply, watching the worker's time and memory.
    bool receive(Reply& reply, std::vector<u8>& payload, double timeoutSeconds) {
        const double deadline = nowSeconds() + timeoutSeconds;
        if (!readAll(&reply, sizeof(reply), deadline)) {
            return false;
        }
        if (reply.magic != worker::kMagic) {
            return false;
        }
        payload.resize(reply.payloadBytes);
        return readAll(payload.data(), payload.size(), deadline);
    }

    u64 footprint() const {
        rusage_info_v4 usage{};
        if (pid_ == 0 ||
            proc_pid_rusage(pid_, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
            return 0;
        }
        return usage.ri_phys_footprint;
    }

    void kill() {
        if (pid_ != 0) {
            ::kill(pid_, SIGKILL);
        }
        stop();
    }

    // Closing the request pipe tells the worker to exit.
    void stop() {
        request_.reset();
        reply_.reset();
        if (pid_ != 0) {
            int status = 0;
            while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
            pid_ = 0;
        }
    }

private:
    bool writeAll(const void* data, usize size) {
        const auto* cursor = static_cast<const u8*>(data);
        while (size != 0) {
            const ssize_t count = write(request_.get(), cursor, size);
            if (count >= 0) {
                cursor += count;
                size -= usize(count);
            } else if (errno != EINTR) {
                return false;
            }
        }
        return true;
    }

    bool readAll(void* data, usize size, double deadline) {
        auto* cursor = static_cast<u8*>(data);
        while (size != 0) {
            pollfd watch{reply_.get(), POLLIN, 0};
            const int ready = poll(&watch, 1, kWatchIntervalMs);
            if (ready < 0 && errno != EINTR) {
                return false;
            }
            if (ready <= 0) {
                static const u64 memoryLimit = workerMemoryLimit();
                if (nowSeconds() > deadline || footprint() > memoryLimit) {
                    return false;
                }
                continue;
            }
            const ssize_t count = read(reply_.get(), cursor, size);
            if (count > 0) {
                cursor += count;
                size -= usize(count);
            } else if (count == 0 || errno != EINTR) {
                return false;
            }
        }
        return true;
    }

    pid_t pid_ = 0;
    Descriptor request_, reply_;
};

// Reads the faces in a reply's payload, rejecting anything that runs past its end.
template <typename Visit>
bool forEachFace(const std::vector<u8>& payload, Visit&& visit) {
    usize offset = 0;
    while (offset < payload.size()) {
        if (payload.size() - offset < sizeof(FaceHeader)) {
            return false;
        }
        FaceHeader header;
        std::memcpy(&header, payload.data() + offset, sizeof(header));
        offset += sizeof(header);
        const usize positionBytes = usize(header.vertexCount) * 3 * sizeof(float);
        const usize indexBytes = usize(header.triangleCount) * 3 * sizeof(u32);
        if (payload.size() - offset < positionBytes * 2 + indexBytes) {
            return false;
        }
        const u8* positions = payload.data() + offset;
        const u8* normals = positions + positionBytes;
        const u8* indices = normals + positionBytes;
        visit(header, positions, normals, indices);
        offset += positionBytes * 2 + indexBytes;
    }
    return true;
}

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

struct FaceSink {
    IndexedMesh mesh;

    void reserve(usize vertices, usize triangles) {
        mesh.positions.reserve(vertices * 3);
        mesh.normals.reserve(vertices * 3);
        mesh.faceOfVertex.reserve(vertices);
        mesh.indices.reserve(triangles * 3);
    }

    void add(const FaceHeader& header, const u8* positions, const u8* normals, const u8* indices,
             u32 face, bool flipped) {
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
};

struct Tolerances {
    double linear, angular, minSize;
};

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

// A run of a definition's faces to read, or the whole body as a last resort.
struct Job {
    u32 definition = 0;
    u32 firstFace = 0;
    u32 faceCount = 0;
    bool wholeBody = false;
};

// One reply kept until its definition is complete.
struct Result {
    std::vector<u8> payload;
    u32 firstFace = 0;
    bool wholeBody = false;
};

struct DefinitionProgress {
    std::mutex mutex;
    std::vector<Result> results;
    std::atomic<u32> pendingJobs{0};
    bool triedWholeBody = false; // touched only by the thread that finishes the last job
};

class SliceImporter {
public:
    SliceImporter(const Part21& index, const Structure& structure, const Definitions& definitions,
                  BlobStream& stream, SegmentListener* listener, const StepOptions& options,
                  const Tolerances& tolerances, StepStats& stats)
        : index_(index), structure_(structure), definitions_(definitions), stream_(stream),
          listener_(listener), options_(options), tolerances_(tolerances), stats_(stats),
          progress_(definitions.items.size()) {}

    bool run(std::string& error) {
        queueWork();
        if (queue_.empty()) {
            return true;
        }
        const usize cores = options_.maxWorkers != 0 ? options_.maxWorkers : performanceCores();
        const usize count = std::min(cores, queue_.size());
        stats_.workers = count;
        std::vector<std::thread> threads;
        threads.reserve(count);
        for (usize i = 0; i < count; ++i) {
            threads.emplace_back([this] { workLoop(); });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        stats_.readSeconds = micros(readMicros_);
        stats_.transferSeconds = micros(transferMicros_);
        stats_.meshSeconds = micros(meshMicros_);
        stats_.packSeconds = micros(packMicros_);
        stats_.failedFaces = failedFaces_;
        stats_.unmappedFaces = unmappedFaces_;
        stats_.crashedWorkers = crashedWorkers_;
        stats_.triangles = triangles_;
        stats_.meshlets = meshlets_;
        if (!firstError_.empty()) {
            error = firstError_;
            return false;
        }
        return true;
    }

private:
    static double micros(const std::atomic<u64>& value) {
        return double(value.load(std::memory_order_relaxed)) * 1e-6;
    }
    static void addTime(std::atomic<u64>& total, double seconds) {
        total.fetch_add(u64(seconds * 1e6), std::memory_order_relaxed);
    }

    // Definitions placed most often and largest go first, since they fill the screen fastest.
    void queueWork() {
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
                queue_.push_back({chunk.definition, chunk.firstFace, chunk.faceCount, false});
            }
        }
        outstanding_ = queue_.size();
    }

    std::optional<Job> nextJob() {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return !queue_.empty() || outstanding_ == 0; });
        if (queue_.empty()) {
            return std::nullopt;
        }
        const Job job = queue_.front();
        queue_.pop_front();
        return job;
    }

    void finishJob() {
        std::lock_guard lock(mutex_);
        --outstanding_;
        if (outstanding_ == 0) {
            wake_.notify_all();
        }
    }

    void requeue(std::initializer_list<Job> jobs) {
        {
            std::lock_guard lock(mutex_);
            outstanding_ += jobs.size();
            for (const Job& job : jobs) {
                queue_.push_front(job);
            }
        }
        wake_.notify_all();
    }

    void buildText(const Job& job, ClosureWalker& walker) {
        const Definition& definition = definitions_.items[job.definition];
        const Body& body = structure_.bodies[definition.body];
        const u32 context = body.context;
        if (job.wholeBody) {
            writeBodySlice(index_, body.record, context, walker, text_);
            return;
        }
        faceRecords_.clear();
        for (u32 f = 0; f < job.faceCount; ++f) {
            faceRecords_.push_back(definitions_.faces[job.firstFace + f].record);
        }
        writeSlice(index_, faceRecords_, context, walker, text_);
    }

    static thread_local std::vector<u32> faceRecords_;
    static thread_local std::string text_;

    void workLoop() {
        ClosureWalker walker(index_);
        WorkerProcess process;
        std::vector<u8> payload;
        while (const std::optional<Job> job = nextJob()) {
            buildText(*job, walker);
            if (!process.running()) {
                std::string error;
                if (!process.start(options_.workerPath, -1, error)) {
                    fail(error);
                    failJob(*job);
                    continue;
                }
            }
            Request request{};
            request.magic = worker::kMagic;
            request.kind = u32(RequestKind::Slice);
            request.textBytes = text_.size();
            // A face that would not mesh is usually smaller than the minimum edge, so the retry of
            // a whole body works finer. Only the faces still missing are taken from it.
            const double scale = job->wholeBody ? kRetryToleranceScale : 1.0;
            request.linearDeflection = tolerances_.linear * scale;
            request.angularDeflection = tolerances_.angular;
            request.minSize = tolerances_.minSize * scale;

            Reply reply{};
            payload.clear();
            const bool answered = process.send(request, text_) &&
                                  process.receive(reply, payload, kSliceTimeoutSeconds);
            if (!answered) {
                crashedWorkers_.fetch_add(1, std::memory_order_relaxed);
                process.kill();
                retry(*job);
                continue;
            }
            if (reply.status != u32(ReplyStatus::Ok)) {
                retry(*job);
                continue;
            }
            addTime(readMicros_, reply.readSeconds);
            addTime(transferMicros_, reply.transferSeconds);
            addTime(meshMicros_, reply.meshSeconds);
            unmappedFaces_.fetch_add(reply.unmappedFaces, std::memory_order_relaxed);
            complete(*job, std::move(payload));
            payload = {};
            if (process.footprint() > kWorkerRecycleFootprint) {
                process.stop();
            }
        }
    }

    // The failure ladder: halve the slice until one face is alone. `finishDefinition` then gives
    // any face still without triangles to OCCT as part of the whole body.
    void retry(const Job& job) {
        if (job.wholeBody) {
            failJob(job);
            return;
        }
        if (job.faceCount > 1) {
            const u32 half = job.faceCount / 2;
            progress_[job.definition].pendingJobs.fetch_add(1, std::memory_order_relaxed);
            requeue({Job{job.definition, job.firstFace, half, false},
                     Job{job.definition, job.firstFace + half, job.faceCount - half, false}});
            finishJob();
            return;
        }
        failJob(job);
    }

    void failJob(const Job& job) {
        reportProgress(job.faceCount);
        if (progress_[job.definition].pendingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            finishDefinition(job.definition);
        }
        finishJob();
    }

    void complete(const Job& job, std::vector<u8> payload) {
        DefinitionProgress& state = progress_[job.definition];
        {
            std::lock_guard lock(state.mutex);
            state.results.push_back({std::move(payload), job.firstFace, job.wholeBody});
        }
        reportProgress(job.wholeBody ? 0 : job.faceCount);
        if (state.pendingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            finishDefinition(job.definition);
        }
        finishJob();
    }

    // Every job for a definition is done, and only this thread can touch its results. If a face has
    // no triangles, OCCT gets one chance to mesh it as part of the real solid.
    void finishDefinition(u32 d) {
        DefinitionProgress& state = progress_[d];
        if (!state.triedWholeBody && countMissingFaces(d) != 0) {
            state.triedWholeBody = true;
            const Definition& definition = definitions_.items[d];
            state.pendingJobs.store(1, std::memory_order_relaxed);
            requeue({Job{d, definition.firstFace, definition.faceCount, true}});
            return;
        }
        packDefinition(d);
    }

    std::vector<std::pair<u32, u32>> faceIds(const Definition& definition) const {
        std::vector<std::pair<u32, u32>> ids(definition.faceCount);
        for (u32 f = 0; f < definition.faceCount; ++f) {
            ids[f] = {index_.ids[definitions_.faces[definition.firstFace + f].record], f};
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    u32 countMissingFaces(u32 d) const {
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

    void reportProgress(u32 faces) {
        const u32 done = doneFaces_.fetch_add(faces, std::memory_order_relaxed) + faces;
        if (listener_ != nullptr) {
            listener_->progress(done, totalFaces_);
        }
    }

    void packDefinition(u32 d) {
        const double start = nowSeconds();
        const Definition& definition = definitions_.items[d];
        const Body& body = structure_.bodies[definition.body];
        DefinitionProgress& state = progress_[d];
        const std::span<const FaceUse> faces(definitions_.faces.data() + definition.firstFace,
                                             definition.faceCount);

        const std::vector<std::pair<u32, u32>> faceOfId = faceIds(definition);

        // Slices are read before any whole-body retry, so a retry only ever fills gaps.
        std::stable_sort(
            state.results.begin(), state.results.end(),
            [](const Result& a, const Result& b) { return a.wholeBody < b.wholeBody; });

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

    void fail(const std::string& message) {
        std::lock_guard lock(mutex_);
        if (firstError_.empty()) {
            firstError_ = message;
        }
    }

    const Part21& index_;
    const Structure& structure_;
    const Definitions& definitions_;
    BlobStream& stream_;
    SegmentListener* listener_;
    const StepOptions& options_;
    const Tolerances tolerances_;
    StepStats& stats_;

    std::vector<DefinitionProgress> progress_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> queue_;
    usize outstanding_ = 0;
    std::string firstError_;
    u32 totalFaces_ = 0;

    std::atomic<u32> doneFaces_{0};
    std::atomic<u32> failedFaces_{0};
    std::atomic<u32> unmappedFaces_{0};
    std::atomic<u32> crashedWorkers_{0};
    std::atomic<u64> triangles_{0};
    std::atomic<u64> meshlets_{0};
    std::atomic<u64> readMicros_{0};
    std::atomic<u64> transferMicros_{0};
    std::atomic<u64> meshMicros_{0};
    std::atomic<u64> packMicros_{0};
};

thread_local std::vector<u32> SliceImporter::faceRecords_;
thread_local std::string SliceImporter::text_;

// The fallback for files the slicer does not understand: one OCCT read of the whole file, one
// definition per body, and a flat tree.
bool importWholeFile(int outputFd, SegmentListener* listener, const StepOptions& options,
                     const std::array<u8, 16>& hash, StepStats& stats, std::string& error) {
    stats.wholeFile = true;
    if (options.sourceFd < 0) {
        error = "This file needs a full read, but its source is not available.";
        return false;
    }
    // ModelInfo stays zeroed: a whole-file read has no tolerances to refine from.
    Descriptor source(fcntl(options.sourceFd, F_DUPFD_CLOEXEC, kFirstHighFd));
    WorkerProcess process;
    if (!process.start(options.workerPath, source.get(), error)) {
        return false;
    }
    Request request{};
    request.magic = worker::kMagic;
    request.kind = u32(RequestKind::WholeFile);
    Reply reply{};
    std::vector<u8> payload;
    if (!process.send(request, {}) || !process.receive(reply, payload, kWholeFileTimeoutSeconds) ||
        reply.status != u32(ReplyStatus::Ok)) {
        process.kill();
        error = "This file could not be read.";
        return false;
    }
    process.stop();

    stats.readSeconds = reply.readSeconds;
    stats.transferSeconds = reply.transferSeconds;
    stats.meshSeconds = reply.meshSeconds;

    std::vector<FaceSink> groups(reply.groupCount);
    if (!forEachFace(payload, [&](const FaceHeader& header, const u8* positions, const u8* normals,
                                  const u8* indices) {
            if (header.group < groups.size()) {
                FaceSink& sink = groups[header.group];
                sink.add(header, positions, normals, indices, sink.mesh.faceCount++, false);
            }
        })) {
        error = "An import worker returned damaged data.";
        return false;
    }

    BlobStream stream(outputFd, listener);
    ModelData model;
    format::NodeRecord root{};
    root.parent = format::kNoParent;
    root.subtreeEnd = u32(groups.size()) + 1;
    root.occurrenceEnd = u32(groups.size());
    root.kind = u32(format::NodeKind::Assembly);
    model.nodes.push_back(root);
    for (u32 g = 0; g < groups.size(); ++g) {
        PackedDefinition packed = packMesh(groups[g].mesh);
        stats.triangles += packed.triangles.size();
        stats.meshlets += packed.meshlets.size();

        format::DefinitionInfo info{};
        info.boundsMin = packed.boundsMin;
        info.boundsMax = packed.boundsMax;
        info.faceCount = groups[g].mesh.faceCount;
        info.flags = packed.flags;
        model.definitions.push_back(info);

        format::OccurrenceRecord occurrence{};
        occurrence.transform = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        occurrence.definition = g;
        occurrence.node = g + 1;
        model.occurrences.push_back(occurrence);

        format::NodeRecord node{};
        node.parent = 0;
        node.subtreeEnd = g + 2;
        node.firstOccurrence = g;
        node.occurrenceEnd = g + 1;
        node.kind = u32(format::NodeKind::Body);
        model.nodes.push_back(node);

        std::vector<format::FaceRecord> records(groups[g].mesh.faceCount);
        for (u32 f = 0; f < records.size(); ++f) {
            records[f].sourceId = packed.originalFace.empty() ? f : packed.originalFace[f];
        }
        if (!stream.addDefinition(g, 0, std::move(packed), std::move(records))) {
            error = stream.error();
            return false;
        }
    }
    stats.definitions = stats.occurrences = groups.size();
    if (!stream.writeModel(model) || !stream.finish(hash)) {
        error = stream.error();
        return false;
    }
    return true;
}

} // namespace

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

bool importStep(std::span<const u8> file, int outputFd, SegmentListener* listener,
                const StepOptions& options, StepStats& stats, std::string& error) {
    double lap = nowSeconds();
    const auto stage = [&](double& seconds) {
        const double now = nowSeconds();
        seconds = now - lap;
        lap = now;
    };

    Part21 index;
    std::vector<u8> scratch;
    if (!indexPart21(file, index, scratch, error)) {
        return false;
    }
    stats.records = index.recordCount();
    stage(stats.tokenizeSeconds);

    Structure structure = readStructure(index);
    stats.warnings = std::move(structure.warnings);
    stats.bodies = structure.bodies.size();
    stats.occurrences = structure.occurrences.size();
    stage(stats.structureSeconds);

    if (index.needsWholeFile || structure.needsWholeFile || structure.occurrences.empty()) {
        if (!options.definitions.empty()) {
            error = "This file cannot be refined.";
            return false;
        }
        return importWholeFile(outputFd, listener, options, hashContents(file), stats, error);
    }

    const Definitions definitions = buildDefinitions(index, structure);
    stats.definitions = definitions.items.size();
    stats.chunks = definitions.chunks.size();
    stats.faces = definitions.faces.size();
    stage(stats.definitionSeconds);

    const Tolerances tolerances =
        tolerancesFor(worldDiagonal(structure, definitions), options.level);
    stats.linearDeflection = tolerances.linear;
    stats.angularDeflection = tolerances.angular;
    stats.minSize = tolerances.minSize;

    BlobStream stream(outputFd, listener);
    if (options.definitions.empty()) {
        ModelData model = modelFrom(structure, definitions);
        const Tolerances base = tolerancesFor(worldDiagonal(structure, definitions), 0);
        model.info.linearDeflection = base.linear;
        model.info.angularDeflection = base.angular;
        model.info.maxLevel = kMaxLevel;
        if (!stream.writeModel(model)) {
            error = stream.error();
            return false;
        }
    }

    SliceImporter importer(index, structure, definitions, stream, listener, options, tolerances,
                           stats);
    if (!importer.run(error)) {
        return false;
    }
    stage(stats.workerSeconds);

    if (!stream.finish(hashContents(file))) {
        error = stream.error();
        return false;
    }
    return true;
}

} // namespace geo::import
