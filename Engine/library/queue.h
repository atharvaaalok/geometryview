// The order in which a gallery's files are imported and drawn. Pure policy, with no threads or
// processes, so the browser's import queue can run the same cases (Tests/data/gallery-queue.json).
//
// What is wanted is replaced whole as the view moves: each job has a band (a file being opened, one
// in view, one near the view, or one prefetched while nothing else waits) and an order within its
// band. Jobs start best first while slots are free. A job no longer wanted is dropped before it
// starts; once running it runs on, since its result is kept, unless a job in view needs its slot.
// Only a file being opened or one in view ever cancels a running job, and only one in a lower band.
#pragma once

#include "base/types.h"

#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace geo::library {

enum class Band : u8 { Open = 0, Visible = 1, Near = 2, Prefetch = 3 };

/// The band named as the shared cases name it: open, visible, near or prefetch.
std::optional<Band> bandNamed(std::string_view name);

struct Want {
    std::string id;
    Band band = Band::Visible;
    double order = 0.0;
    u64 bytes = 0; // the source file's size, when known
};

class Queue {
public:
    /// Files larger than this are never prefetched: they are imported only when in view.
    static constexpr u64 kMaxPrefetchBytes = 300'000'000;

    explicit Queue(u32 slots = 1, bool prefetch = true) : slots_(slots), prefetch_(prefetch) {}

    /// Replaces what is wanted. A waiting job not in `jobs` is dropped; a running one runs on,
    /// stale. A job is started again only if it is wanted after it finished.
    void want(std::span<const Want> jobs);

    /// Starts the best waiting job, if a slot is free and one may start, and returns its id.
    std::optional<std::string> next();

    /// A running job completed, or failed: it is done with.
    void finish(std::string_view id);
    /// A running job stopped before completing, as when cancelled: if still wanted it waits again.
    void stopped(std::string_view id);

    /// Running jobs to cancel now, worst first, so that waiting jobs in view can start. Each is
    /// returned once, and keeps its slot until it has stopped.
    std::vector<std::string> toCancel();

    /// Fewer slots, as memory runs short, start nothing new until running jobs fall below them.
    void setSlots(u32 slots) { slots_ = slots; }
    u32 slots() const { return slots_; }
    /// Whether prefetched jobs may start.
    void setPrefetch(bool allowed) { prefetch_ = allowed; }

    std::vector<std::string> waiting() const;
    std::vector<std::string> running() const;
    bool idle() const { return waiting_.empty() && runningCount_ == 0; }
    usize waitingCount() const { return waiting_.size(); }
    usize runningCount() const { return runningCount_; }
    /// A job's band, if the queue holds it.
    std::optional<Band> bandOf(std::string_view id) const;

private:
    struct Job {
        Band band = Band::Visible;
        double order = 0.0;
        u64 bytes = 0;
        u64 position = 0; // in the latest wants, to keep ties in order
        bool wanted = true;
        bool running = false;
        bool cancelling = false;
    };
    using Key = std::tuple<u8, double, u64, std::string>;
    static Key keyOf(const std::string& id, const Job& job) {
        return {u8(job.band), job.order, job.position, id};
    }

    u32 slots_;
    bool prefetch_;
    usize runningCount_ = 0;
    std::unordered_map<std::string, Job> jobs_;
    std::set<Key> waiting_;
};

} // namespace geo::library
