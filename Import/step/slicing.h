// Slicing a STEP file for OCCT, the same on every platform. A StepImport indexes the file, reads
// its structure and definitions, and writes the model; its SliceScheduler then hands out jobs, a
// few faces each, takes back what workers make of them, retries failures down the failure ladder,
// and packs each definition as its last job returns. Who runs the jobs is up to a driver: native
// worker processes on the Mac, Web Workers in the browser.
#pragma once

#include "base/types.h"
#include "cache/blob_writer.h"
#include "mesh/indexed_mesh.h"
#include "occt/worker_protocol.h"
#include "step/definitions.h"
#include "step/part21.h"
#include "step/step_import.h"
#include "step/structure.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace geo::import {

struct Tolerances {
    double linear, angular, minSize;
};

/// A run of a definition's faces to read, or the whole body.
struct Job {
    u32 definition = 0;
    u32 firstFace = 0;
    u32 faceCount = 0;
    bool wholeBody = false;
    bool retry = false; // a last attempt for faces still missing, at a finer tolerance
};

/// What writing a job's text needs, kept between jobs so it allocates rarely. One per driver thread.
class SliceWriter {
public:
    explicit SliceWriter(const step::Part21& index) : walker_(index) {}

private:
    friend class SliceScheduler;
    step::ClosureWalker walker_;
    std::vector<u32> faceRecords_;
};

/// Hands out a file's slices and gathers what comes back. Safe to call from several threads, as
/// the native driver does, or from one, as an event loop does.
class SliceScheduler {
public:
    SliceScheduler(const step::Part21& index, const step::Structure& structure,
                   const step::Definitions& definitions, BlobStream& stream,
                   SegmentListener* listener, const StepOptions& options,
                   const Tolerances& tolerances);

    /// Queues every definition's jobs, largest and most placed first. Returns how many.
    usize start();

    /// The next job, waiting while replies still due may add retries; nothing once all are done.
    std::optional<Job> waitForJob();
    /// The next job if one is queued now, without waiting.
    std::optional<Job> takeJob();
    /// Whether every job has been answered, including retries.
    bool finished();

    /// Writes a job's STEP text and the request that goes with it.
    void prepare(const Job& job, SliceWriter& writer, std::string& text,
                 worker::Request& request) const;
    /// A worker answered. OCCT's own failure sends the job down the failure ladder.
    void onReply(const Job& job, const worker::Reply& reply, std::vector<u8> payload);
    /// A worker crashed or timed out on the job, which goes down the failure ladder.
    void onCrash(const Job& job);
    /// No worker could be started for the job, which is given up.
    void abandon(const Job& job, const std::string& error);

    /// Fills `stats` with what the jobs did. Returns false with the first error, if there was one.
    bool finish(StepStats& stats, std::string& error);

private:
    struct Result {
        std::vector<u8> payload;
        u32 firstFace = 0;
        bool retry = false;
    };
    struct DefinitionProgress {
        std::mutex mutex;
        std::vector<Result> results;
        std::atomic<u32> pendingJobs{0};
        bool triedWholeBody = false; // touched only by whoever finishes the last job
    };

    void finishJob();
    void requeue(std::initializer_list<Job> jobs);
    void retry(const Job& job);
    void failJob(const Job& job);
    void complete(const Job& job, std::vector<u8> payload);
    void finishDefinition(u32 d);
    std::vector<std::pair<u32, u32>> faceIds(const step::Definition& definition) const;
    u32 countMissingFaces(u32 d) const;
    void reportProgress(u32 faces);
    void packDefinition(u32 d);
    void fail(const std::string& message);

    const step::Part21& index_;
    const step::Structure& structure_;
    const step::Definitions& definitions_;
    BlobStream& stream_;
    SegmentListener* listener_;
    const StepOptions& options_;
    const Tolerances tolerances_;

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

/// A STEP import in stages, so a driver can run its jobs in between: `begin`, then the
/// scheduler's jobs, then `finish`.
class StepImport {
public:
    /// Indexes the file, reads its structure and definitions, and writes the model. If the file
    /// cannot be sliced, `needsWholeFile` is set and nothing more is done.
    bool begin(std::span<const u8> file, int outputFd, SegmentListener* listener,
               const StepOptions& options, StepStats& stats, std::string& error);
    bool needsWholeFile() const { return needsWholeFile_; }
    SliceScheduler& scheduler() { return *scheduler_; }
    const step::Part21& index() const { return index_; }
    /// After every job: records what they did and completes the cache file.
    bool finish(StepStats& stats, std::string& error);

private:
    std::span<const u8> file_;
    step::Part21 index_;
    std::vector<u8> scratch_;
    step::Structure structure_;
    step::Definitions definitions_;
    std::optional<BlobStream> stream_;
    std::optional<SliceScheduler> scheduler_;
    bool needsWholeFile_ = false;
};

/// Reads the faces in a reply's payload into `visit(header, positions, normals, indices)`,
/// rejecting anything that runs past its end.
template <typename Visit>
bool forEachFace(const std::vector<u8>& payload, Visit&& visit) {
    usize offset = 0;
    while (offset < payload.size()) {
        if (payload.size() - offset < sizeof(worker::FaceHeader)) {
            return false;
        }
        worker::FaceHeader header;
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

/// A mesh built from the faces of replies, for packing.
struct FaceSink {
    IndexedMesh mesh;
    void reserve(usize vertices, usize triangles);
    void add(const worker::FaceHeader& header, const u8* positions, const u8* normals,
             const u8* indices, u32 face, bool flipped);
};

} // namespace geo::import
