// Shares the cores between the imports a process runs. OCCT workers are the costly part of an
// import, so the gate counts them: a gallery's imports together hold at most the cap, one ticket
// each. A file the user opens is never asked to wait: its ticket is granted at once, and while any
// such ticket is held no gallery import starts.
#pragma once

#include "base/types.h"

#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace geo::library {

class ImportGate {
public:
    /// The process's gate, capped at its performance cores.
    static ImportGate& shared();

    explicit ImportGate(u32 cap);
    ImportGate(const ImportGate&) = delete;
    ImportGate& operator=(const ImportGate&) = delete;

    /// Held while an import runs; returns its workers to the gate when it goes.
    class Ticket {
    public:
        Ticket() = default;
        Ticket(Ticket&& other) noexcept { *this = std::move(other); }
        Ticket& operator=(Ticket&& other) noexcept;
        ~Ticket() { release(); }
        explicit operator bool() const { return gate_ != nullptr; }
        u32 workers() const { return workers_; }
        void release();

    private:
        friend class ImportGate;
        Ticket(ImportGate* gate, u32 workers, bool open)
            : gate_(gate), workers_(workers), open_(open) {}
        ImportGate* gate_ = nullptr;
        u32 workers_ = 0;
        bool open_ = false;
    };

    /// A file being opened: granted at once, whatever gallery imports hold.
    Ticket open();

    /// A gallery import of `workers` workers, if they fit under the cap now and no file is being
    /// opened; otherwise an empty ticket. Asking for more than the cap asks for the cap.
    Ticket tryEnter(u32 workers);

    /// Called, on the thread that released it, whenever a ticket is returned or an open begins, so
    /// whoever is waiting to enter can try again, or cancel what a file being opened needs. The
    /// returned id removes it.
    u64 listen(std::function<void()> changed);
    void unlisten(u64 id);

    u32 cap() const { return cap_; }
    u32 held() const;
    u32 opens() const;

private:
    void returned(u32 workers, bool open);
    void notify();

    const u32 cap_;
    mutable std::mutex mutex_;
    u32 held_ = 0;
    u32 opens_ = 0;
    u64 nextListener_ = 1;
    std::vector<std::pair<u64, std::function<void()>>> listeners_;
};

/// The number of performance cores, or of all cores where they are not told apart.
u32 performanceCores();

} // namespace geo::library
