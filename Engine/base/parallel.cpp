#include "base/parallel.h"

#include <atomic>
#include <thread>

#if !defined(__APPLE__) && GEO_HAS_THREADS
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>
#endif

namespace geo {

usize parallelWidth() {
#if GEO_HAS_THREADS
    static const usize width = std::max<usize>(1, std::thread::hardware_concurrency());
    return width;
#else
    return 1;
#endif
}

#if !defined(__APPLE__) && GEO_HAS_THREADS

namespace {

// Workers that sleep until a batch is posted, then claim its indices until none are left. The
// posting thread claims indices too, so a batch never waits on an idle core. Several threads may
// post at once, as the packer does while it finds edges alongside, and the workers share
// themselves among every batch in hand, as Apple's pool does. A batch lives on its poster's stack
// and is let go only once no worker holds it.
class Pool {
public:
    Pool() {
        const usize helpers = parallelWidth() - 1;
        for (usize i = 0; i < helpers; ++i) {
            threads_.emplace_back([this] { serve(); });
        }
    }

    void run(usize count, void* context, void (*function)(void*, usize)) {
        Batch batch{count, context, function};
        {
            std::lock_guard lock(mutex_);
            active_.push_back(&batch);
        }
        wake_.notify_all();
        const usize ran = claim(batch);
        std::unique_lock lock(mutex_);
        batch.finished += ran;
        done_.wait(lock, [&] { return batch.finished == count && batch.holders == 0; });
        active_.erase(std::find(active_.begin(), active_.end(), &batch));
    }

private:
    struct Batch {
        usize count = 0;
        void* context = nullptr;
        void (*function)(void*, usize) = nullptr;
        std::atomic<usize> next{0};
        usize finished = 0; // indices run, under the mutex
        usize holders = 0;  // workers that may still claim from it, under the mutex
    };

    // The batch with indices left to claim and the fewest workers on it, or null, so concurrent
    // batches advance together rather than one after the other. Under the mutex.
    Batch* open() const {
        Batch* best = nullptr;
        for (Batch* batch : active_) {
            if (batch->next.load(std::memory_order_relaxed) < batch->count &&
                (best == nullptr || batch->holders < best->holders)) {
                best = batch;
            }
        }
        return best;
    }

    void serve() {
        std::unique_lock lock(mutex_);
        for (;;) {
            Batch* batch = nullptr;
            wake_.wait(lock, [&] { return (batch = open()) != nullptr; });
            ++batch->holders;
            lock.unlock();
            const usize ran = claim(*batch);
            lock.lock();
            batch->finished += ran;
            --batch->holders;
            if (batch->finished == batch->count && batch->holders == 0) {
                done_.notify_all();
            }
        }
    }

    // Runs indices of `batch` until none are left, and returns how many it ran.
    static usize claim(Batch& batch) {
        usize ran = 0;
        for (usize index; (index = batch.next.fetch_add(1, std::memory_order_relaxed)) < batch.count;
             ++ran) {
            batch.function(batch.context, index);
        }
        return ran;
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::vector<Batch*> active_;
};

thread_local bool insidePool = false;

} // namespace

void detail::poolFor(usize count, void* context, void (*function)(void*, usize)) {
    if (count <= 1 || insidePool) {
        for (usize index = 0; index < count; ++index) {
            function(context, index);
        }
        return;
    }
    static Pool* pool = new Pool; // never destroyed, so exit does not wait on its threads
    insidePool = true;
    pool->run(count, context, function);
    insidePool = false;
}

#endif

void Concurrent::start(void* context, void (*function)(void*)) {
    context_ = context;
    function_ = function;
#if defined(__APPLE__)
    group_ = dispatch_group_create();
    dispatch_group_async_f(group_, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), context,
                           function);
#elif GEO_HAS_THREADS
    thread_ = new std::thread(function, context);
#endif
}

void Concurrent::wait() {
    if (function_ == nullptr) {
        return;
    }
#if defined(__APPLE__)
    dispatch_group_wait(group_, DISPATCH_TIME_FOREVER);
    dispatch_release(group_);
#elif GEO_HAS_THREADS
    auto* thread = static_cast<std::thread*>(thread_);
    thread->join();
    delete thread;
#else
    function_(context_);
#endif
    function_ = nullptr;
}

} // namespace geo
