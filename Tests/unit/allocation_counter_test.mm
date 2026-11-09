// The frame's allocation count tells our code's allocations, which a frame must not make, from
// those Metal makes inside its own calls.
#include "engine/allocation_counter.h"

#include <Metal/Metal.h>
#include <doctest/doctest.h>
#include <thread>

using namespace geo;

// Sanitizers bring allocators of their own, which the system's malloc logger never sees.
#if !GEO_SANITIZED
namespace {

// Keeps an allocation from being optimized away.
void* volatile sink = nullptr;

} // namespace

TEST_CASE("allocation counting tells our code's allocations from Metal's") {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const bool haveQueue = queue != nil;
    REQUIRE(haveQueue);

    u64 ours = 0, oursOwn = 0, metal = 0, metalOwn = 0;
    std::thread([&] {
        engine::startCountingAllocations();
        u64 all = engine::allocationCount(), own = engine::ownAllocationCount();
        auto* numbers = new int[64];
        sink = numbers;
        ours = engine::allocationCount() - all;
        oursOwn = engine::ownAllocationCount() - own;
        delete[] numbers;

        @autoreleasepool {
            all = engine::allocationCount();
            own = engine::ownAllocationCount();
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            sink = (__bridge void*)commands;
            metal = engine::allocationCount() - all;
            metalOwn = engine::ownAllocationCount() - own;
        }
        engine::stopCountingAllocations();
    }).join();

    CHECK(ours == 1);
    CHECK(oursOwn == 1);
    CHECK(metal > 0);
    CHECK(metalOwn == 0);
}
#endif
