#include "base/parallel.h"

#include <atomic>
#include <thread>

#if !defined(__APPLE__) && GEO_HAS_THREADS
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

// Workers that sleep until a batch is posted, then claim indices until the batch is done. The
// posting thread claims indices too, so a batch never waits on an idle core. A claim is a ticket
// holding the batch's generation in its high half and the next index in its low half, so a worker
// that wakes late can never claim an index of a later batch with an earlier batch's function.
class Pool {
public:
    Pool() {
        const usize helpers = parallelWidth() - 1;
        for (usize i = 0; i < helpers; ++i) {
            threads_.emplace_back([this] { serve(); });
        }
    }

    void run(usize count, void* context, void (*function)(void*, usize)) {
        std::lock_guard batchLock(batchMutex_); // one batch at a time
        u32 generation;
        {
            std::lock_guard lock(mutex_);
            generation = ++generation_;
            batch_ = {count, context, function, generation};
            remaining_ = count;
            ticket_.store(u64(generation) << 32);
        }
        wake_.notify_all();
        work(batch_);
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return remaining_ == 0; });
    }

private:
    struct Batch {
        usize count = 0;
        void* context = nullptr;
        void (*function)(void*, usize) = nullptr;
        u32 generation = 0;
    };

    void serve() {
        u32 seen = 0;
        for (;;) {
            Batch batch;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return generation_ != seen; });
                seen = generation_;
                batch = batch_;
            }
            work(batch);
        }
    }

    // Claims and runs indices of `batch` until none are left or a later batch has begun.
    void work(const Batch& batch) {
        usize finished = 0;
        u64 ticket = ticket_.load();
        for (;;) {
            if (u32(ticket >> 32) != batch.generation || usize(u32(ticket)) >= batch.count) {
                break;
            }
            if (!ticket_.compare_exchange_weak(ticket, ticket + 1)) {
                continue;
            }
            batch.function(batch.context, usize(u32(ticket)));
            ++finished;
            ticket = ticket_.load();
        }
        if (finished > 0) {
            std::lock_guard lock(mutex_);
            remaining_ -= finished;
            if (remaining_ == 0) {
                done_.notify_all();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex batchMutex_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    Batch batch_;
    std::atomic<u64> ticket_{0};
    usize remaining_ = 0;
    u32 generation_ = 0;
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
