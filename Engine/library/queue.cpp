#include "library/queue.h"

#include <algorithm>

namespace geo::library {

std::optional<Band> bandNamed(std::string_view name) {
    if (name == "open") {
        return Band::Open;
    }
    if (name == "visible") {
        return Band::Visible;
    }
    if (name == "near") {
        return Band::Near;
    }
    if (name == "prefetch") {
        return Band::Prefetch;
    }
    return std::nullopt;
}

void Queue::want(std::span<const Want> jobs) {
    for (auto& [id, job] : jobs_) {
        job.wanted = false;
    }
    u64 position = 0;
    for (const Want& want : jobs) {
        auto [it, added] = jobs_.try_emplace(want.id);
        Job& job = it->second;
        if (!added && !job.running) {
            waiting_.erase(keyOf(it->first, job));
        }
        job.band = want.band;
        job.order = want.order;
        job.bytes = want.bytes;
        job.position = position++;
        job.wanted = true;
        if (!job.running) {
            waiting_.insert(keyOf(it->first, job));
        }
    }
    // What is no longer wanted is dropped, unless it is running.
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!it->second.wanted && !it->second.running) {
            waiting_.erase(keyOf(it->first, it->second));
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

std::optional<std::string> Queue::next() {
    if (runningCount_ >= slots_) {
        return std::nullopt;
    }
    for (auto it = waiting_.begin(); it != waiting_.end(); ++it) {
        const std::string& id = std::get<3>(*it);
        Job& job = jobs_.at(id);
        if (job.band == Band::Prefetch) {
            if (!prefetch_) {
                break; // prefetched jobs come last
            }
            if (job.bytes > kMaxPrefetchBytes) {
                continue;
            }
        }
        std::string started = id;
        waiting_.erase(it);
        job.running = true;
        ++runningCount_;
        return started;
    }
    return std::nullopt;
}

void Queue::finish(std::string_view id) {
    const auto it = jobs_.find(std::string(id));
    if (it == jobs_.end() || !it->second.running) {
        return;
    }
    --runningCount_;
    jobs_.erase(it);
}

void Queue::stopped(std::string_view id) {
    const auto it = jobs_.find(std::string(id));
    if (it == jobs_.end() || !it->second.running) {
        return;
    }
    --runningCount_;
    Job& job = it->second;
    if (!job.wanted) {
        jobs_.erase(it);
        return;
    }
    job.running = false;
    job.cancelling = false;
    waiting_.insert(keyOf(it->first, job));
}

std::vector<std::string> Queue::toCancel() {
    // Slots that are free, or will be once cancelled jobs stop, go to waiting jobs first.
    usize available = slots_ > runningCount_ ? slots_ - runningCount_ : 0;
    struct Candidate {
        u8 rank; // a stale job ranks below every band
        double order;
        u64 position;
        const std::string* id;
    };
    std::vector<Candidate> candidates;
    for (const auto& [id, job] : jobs_) {
        if (!job.running) {
            continue;
        }
        if (job.cancelling) {
            ++available;
            continue;
        }
        candidates.push_back({job.wanted ? u8(job.band) : u8(4), job.order, job.position, &id});
    }
    // Worst first.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return std::tie(b.rank, b.order, b.position, *b.id) <
               std::tie(a.rank, a.order, a.position, *a.id);
    });

    std::vector<std::string> cancelled;
    for (const Key& key : waiting_) {
        const u8 band = std::get<0>(key);
        if (band > u8(Band::Visible)) {
            break;
        }
        if (available > 0) {
            --available;
            continue;
        }
        const auto worse = std::find_if(candidates.begin(), candidates.end(),
                                        [&](const Candidate& c) { return c.rank > band; });
        if (worse == candidates.end()) {
            break; // later jobs are in the same band or lower, and can cancel no more
        }
        cancelled.push_back(*worse->id);
        candidates.erase(worse);
    }
    for (const std::string& id : cancelled) {
        jobs_.at(id).cancelling = true;
    }
    return cancelled;
}

std::vector<std::string> Queue::waiting() const {
    std::vector<std::string> ids;
    ids.reserve(waiting_.size());
    for (const Key& key : waiting_) {
        ids.push_back(std::get<3>(key));
    }
    return ids;
}

std::vector<std::string> Queue::running() const {
    std::vector<std::pair<Key, std::string>> ordered;
    for (const auto& [id, job] : jobs_) {
        if (job.running) {
            ordered.emplace_back(keyOf(id, job), id);
        }
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::string> ids;
    for (auto& [key, id] : ordered) {
        ids.push_back(std::move(id));
    }
    return ids;
}

std::optional<Band> Queue::bandOf(std::string_view id) const {
    const auto it = jobs_.find(std::string(id));
    if (it == jobs_.end()) {
        return std::nullopt;
    }
    return it->second.band;
}

} // namespace geo::library
