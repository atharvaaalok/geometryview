#include "library/gate.h"

#include <algorithm>
#include <sys/sysctl.h>
#include <thread>

namespace geo::library {

u32 performanceCores() {
    int cores = 0;
    usize size = sizeof(cores);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &cores, &size, nullptr, 0) != 0 || cores <= 0) {
        return std::max(1u, std::thread::hardware_concurrency());
    }
    return u32(cores);
}

ImportGate& ImportGate::shared() {
    static ImportGate gate(performanceCores());
    return gate;
}

ImportGate::ImportGate(u32 cap) : cap_(std::max(1u, cap)) {}

ImportGate::Ticket& ImportGate::Ticket::operator=(Ticket&& other) noexcept {
    if (this != &other) {
        release();
        gate_ = other.gate_;
        workers_ = other.workers_;
        open_ = other.open_;
        other.gate_ = nullptr;
    }
    return *this;
}

void ImportGate::Ticket::release() {
    if (gate_ != nullptr) {
        ImportGate* gate = gate_;
        gate_ = nullptr;
        gate->returned(workers_, open_);
    }
}

ImportGate::Ticket ImportGate::open() {
    {
        std::lock_guard lock(mutex_);
        ++opens_;
    }
    notify(); // so gallery imports a file being opened needs can be cancelled
    return Ticket(this, 0, true);
}

ImportGate::Ticket ImportGate::tryEnter(u32 workers) {
    workers = std::clamp(workers, 1u, cap_);
    std::lock_guard lock(mutex_);
    if (opens_ != 0 || held_ + workers > cap_) {
        return {};
    }
    held_ += workers;
    return Ticket(this, workers, false);
}

void ImportGate::returned(u32 workers, bool open) {
    {
        std::lock_guard lock(mutex_);
        if (open) {
            --opens_;
        } else {
            held_ -= workers;
        }
    }
    notify();
}

void ImportGate::notify() {
    std::lock_guard calling(calling_);
    std::vector<std::function<void()>> calls;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, changed] : listeners_) {
            calls.push_back(changed);
        }
    }
    for (const std::function<void()>& changed : calls) {
        changed();
    }
}

u64 ImportGate::listen(std::function<void()> changed) {
    std::lock_guard lock(mutex_);
    const u64 id = nextListener_++;
    listeners_.emplace_back(id, std::move(changed));
    return id;
}

void ImportGate::unlisten(u64 id) {
    std::lock_guard calling(calling_);
    std::lock_guard lock(mutex_);
    std::erase_if(listeners_, [id](const auto& entry) { return entry.first == id; });
}

u32 ImportGate::held() const {
    std::lock_guard lock(mutex_);
    return held_;
}

u32 ImportGate::opens() const {
    std::lock_guard lock(mutex_);
    return opens_;
}

} // namespace geo::library
