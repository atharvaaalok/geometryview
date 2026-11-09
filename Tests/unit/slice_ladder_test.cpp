// The slices' failure ladder, driven in-process as the browser's event loop drives it, with a fake
// worker in OCCT's place so that each test chooses which requests fail.
#include "step/slicing.h"
#include "step_fixture.h"

#include <cmath>
#include <cstdlib>
#include <doctest/doctest.h>
#include <functional>
#include <regex>
#include <string>
#include <unistd.h>
#include <vector>

using namespace geo;
using namespace geo::import;

namespace {

struct Attempt {
    u32 faces = 0;
    bool retry = false;
    double minSize = 0.0;
};

enum class Outcome { Mesh, Crash, Fail, OutOfMemory };

// What a worker would send back for a slice: one triangle for each face its text names.
std::vector<u8> answer(const std::string& text, worker::Reply& reply) {
    std::vector<u8> payload;
    reply = {};
    reply.magic = worker::kMagic;
    reply.status = u32(worker::ReplyStatus::Ok);
    static const std::regex face(R"(#(\d+)=ADVANCED_FACE\()");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), face);
         it != std::sregex_iterator(); ++it) {
        const worker::FaceHeader header{u32(std::stoul((*it)[1].str())), 0, 3, 1};
        const float positions[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const float normals[9] = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        const u32 indices[3] = {0, 1, 2};
        const auto put = [&](const void* data, usize bytes) {
            const u8* first = static_cast<const u8*>(data);
            payload.insert(payload.end(), first, first + bytes);
        };
        put(&header, sizeof(header));
        put(positions, sizeof(positions));
        put(normals, sizeof(normals));
        put(indices, sizeof(indices));
        ++reply.faceCount;
    }
    reply.payloadBytes = payload.size();
    return payload;
}

// Imports a box, one job at a time, doing to each request what `outcome` says. Returns every
// attempt, and whether the import finished.
bool run(const std::function<Outcome(const worker::Request&, double baseMinSize)>& outcome,
         std::vector<Attempt>& attempts, StepStats& stats, std::string& error) {
    test::StepWriter writer;
    writer.part("box", writer.box({0, 0, 0}, {10, 10, 10}));
    const std::string text = writer.text();
    char path[] = "/tmp/geoladderXXXXXX";
    const int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    StepImport import;
    StepOptions options;
    options.maxWorkers = 1;
    REQUIRE(import.begin({reinterpret_cast<const u8*>(text.data()), text.size()}, fd, nullptr,
                         options, stats, error));
    REQUIRE_FALSE(import.needsWholeFile());
    const double base = stats.minSize;
    SliceScheduler& scheduler = import.scheduler();
    scheduler.start();
    SliceWriter slices(import.index());
    std::string slice;
    worker::Request request{};
    while (const std::optional<Job> job = scheduler.takeJob()) {
        scheduler.prepare(*job, slices, slice, request);
        attempts.push_back({job->faceCount, job->retry, request.minSize / base});
        const Outcome what = outcome(request, base);
        switch (what) {
        case Outcome::Crash:
            scheduler.onCrash(*job);
            break;
        case Outcome::Fail:
        case Outcome::OutOfMemory: {
            worker::Reply reply{};
            reply.magic = worker::kMagic;
            reply.status = u32(what == Outcome::Fail
                                   ? worker::ReplyStatus::Failed
                                   : worker::ReplyStatus::OutOfMemory);
            scheduler.onReply(*job, reply, {});
            break;
        }
        case Outcome::Mesh: {
            worker::Reply reply{};
            std::vector<u8> payload = answer(slice, reply);
            scheduler.onReply(*job, reply, std::move(payload));
            break;
        }
        }
    }
    CHECK(scheduler.finished());
    const bool ok = import.finish(stats, error);
    close(fd);
    unlink(path);
    return ok;
}

usize countAlone(const std::vector<Attempt>& attempts, double scale) {
    usize count = 0;
    for (const Attempt& attempt : attempts) {
        count += attempt.faces == 1 && !attempt.retry && std::abs(attempt.minSize - scale) < 1e-9;
    }
    return count;
}

} // namespace

TEST_CASE("a face whose worker crashes on it alone is tried four times coarser") {
    std::vector<Attempt> attempts;
    StepStats stats;
    std::string error;
    // As a worker out of memory would: everything at the asked tolerance crashes.
    CHECK(run([](const worker::Request& request, double base) {
        return request.minSize < 3.0 * base ? Outcome::Crash : Outcome::Mesh;
    }, attempts, stats, error));
    CHECK(stats.failedFaces == 0);
    CHECK(stats.coarserFaces == 6);
    CHECK(countAlone(attempts, 1.0) == 6);
    CHECK(countAlone(attempts, 4.0) == 6);
    CHECK(countAlone(attempts, 16.0) == 0);
    CHECK(stats.crashedWorkers == attempts.size() - 6);
}

TEST_CASE("a face that crashes at every tolerance is given up after two coarser tries") {
    std::vector<Attempt> attempts;
    StepStats stats;
    std::string error;
    CHECK_FALSE(run([](const worker::Request&, double) { return Outcome::Crash; }, attempts, stats,
                    error));
    CHECK(stats.failedFaces == 6);
    CHECK(stats.coarserFaces == 0);
    CHECK(countAlone(attempts, 1.0) == 6);
    CHECK(countAlone(attempts, 4.0) == 6);
    CHECK(countAlone(attempts, 16.0) == 6);
    CHECK(countAlone(attempts, 64.0) == 0);
    // Then the whole body, finer, once.
    usize retries = 0;
    for (const Attempt& attempt : attempts) {
        retries += attempt.retry;
    }
    CHECK(retries == 1);
}

TEST_CASE("a face OCCT fails on is retried finer with its body, not coarser") {
    std::vector<Attempt> attempts;
    StepStats stats;
    std::string error;
    CHECK(run([](const worker::Request& request, double base) {
        return request.minSize < 0.5 * base ? Outcome::Mesh : Outcome::Fail;
    }, attempts, stats, error));
    CHECK(stats.failedFaces == 0);
    CHECK(stats.coarserFaces == 0);
    CHECK(stats.crashedWorkers == 0);
    CHECK(countAlone(attempts, 4.0) == 0);
}

TEST_CASE("a face whose worker runs out of memory on it is tried coarser, as after a crash") {
    std::vector<Attempt> attempts;
    StepStats stats;
    std::string error;
    // As OCCT's allocator does in a worker whose memory is full: it throws, and the worker lives.
    CHECK(run([](const worker::Request& request, double base) {
        return request.minSize < 3.0 * base ? Outcome::OutOfMemory : Outcome::Mesh;
    }, attempts, stats, error));
    CHECK(stats.failedFaces == 0);
    CHECK(stats.coarserFaces == 6);
    CHECK(stats.crashedWorkers == 0);
    CHECK(countAlone(attempts, 4.0) == 6);
}
