// The STEP importer on the Mac: the portable StepImport (step/slicing.h) driven by OCCT worker
// processes, one thread each, and the whole-file fallback for files that cannot be sliced.
#include "step/step_import.h"

#include "base/time.h"
#include "import.h"
#include "mesh/packer.h"
#include "step/slicing.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <libproc.h>
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

// Runs a scheduler's jobs in OCCT worker processes, one thread each, until every job is answered.
void runWorkerProcesses(SliceScheduler& scheduler, const step::Part21& index,
                        const StepOptions& options, usize jobs, StepStats& stats) {
    const usize cores = options.maxWorkers != 0 ? options.maxWorkers : performanceCores();
    const usize count = std::min(cores, jobs);
    stats.workers = count;
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (usize i = 0; i < count; ++i) {
        threads.emplace_back([&] {
            SliceWriter writer(index);
            WorkerProcess process;
            std::string text;
            std::vector<u8> payload;
            while (const std::optional<Job> job = scheduler.waitForJob()) {
                Request request{};
                scheduler.prepare(*job, writer, text, request);
                if (!process.running()) {
                    std::string error;
                    if (!process.start(options.workerPath, -1, error)) {
                        scheduler.abandon(*job, error);
                        continue;
                    }
                }
                Reply reply{};
                payload.clear();
                if (!process.send(request, text) ||
                    !process.receive(reply, payload, kSliceTimeoutSeconds)) {
                    process.kill();
                    scheduler.onCrash(*job);
                    continue;
                }
                scheduler.onReply(*job, reply, std::move(payload));
                payload = {};
                if (process.footprint() > kWorkerRecycleFootprint) {
                    process.stop();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

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

bool importStep(std::span<const u8> file, int outputFd, SegmentListener* listener,
                const StepOptions& options, StepStats& stats, std::string& error) {
    StepImport import;
    if (!import.begin(file, outputFd, listener, options, stats, error)) {
        return false;
    }
    if (import.needsWholeFile()) {
        if (!options.definitions.empty()) {
            error = "This file cannot be refined.";
            return false;
        }
        if (options.structureOnly) {
            error = "This file's structure can only be read with its geometry.";
            return false;
        }
        return importWholeFile(outputFd, listener, options, hashContents(file), stats, error);
    }
    if (options.structureOnly) {
        return import.finishStructure(error);
    }
    const double start = nowSeconds();
    const usize jobs = import.scheduler().start();
    if (jobs != 0) {
        runWorkerProcesses(import.scheduler(), import.index(), options, jobs, stats);
    }
    stats.workerSeconds = nowSeconds() - start;
    return import.finish(stats, error);
}

} // namespace geo::import
