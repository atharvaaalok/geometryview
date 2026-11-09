#include "engine/allocation_counter.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <unistd.h>

// Declared by the system's stack logging, and not in a public header.
extern "C" {
using MallocLogger = void(uint32_t type, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3,
                          uintptr_t result, uint32_t skip);
extern MallocLogger* malloc_logger;
}

namespace geo::engine {
namespace {

constexpr uint32_t kLogTypeAllocate = 2;
// Not thread_local: touching a thread-local for the first time allocates, which would re-enter this
// hook forever.
pthread_t countedThread = nullptr;
u64 allocations = 0;
u64 ownAllocations = 0;

// Where code lies: this program's, and that of the GPU driver and the display's frameworks, whose
// own allocations no frame can avoid (Metal's encoders, Core Animation's drawables).
struct CodeRange {
    uintptr_t begin = 0, end = 0;
    bool holds(uintptr_t pc) const { return pc >= begin && pc < end; }
};
constexpr int kSystemRangesKept = 16;
CodeRange ownCode;
CodeRange systemCode[kSystemRangesKept];
int systemRanges = 0;
bool rangesFound = false;

CodeRange textOf(uint32_t image) {
    unsigned long size = 0;
    const auto* header =
        reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(image));
    const uint8_t* text = header ? getsegmentdata(header, "__TEXT", &size) : nullptr;
    const auto begin = reinterpret_cast<uintptr_t>(text);
    return {begin, begin + size};
}

// Images load as the program runs (the driver's with the first device), so this is called once a
// frame is about to be counted.
void findCodeRanges() {
    if (rangesFound) {
        return;
    }
    rangesFound = true;
    ownCode = textOf(0); // the main executable, which the engine is linked into
    constexpr const char* kSystemImages[] = {"/Metal.framework/", "/AGXMetal", "/IOGPU.framework/",
                                             "/QuartzCore.framework/", "/IOSurface.framework/"};
    for (uint32_t i = 1; i < _dyld_image_count() && systemRanges < kSystemRangesKept; ++i) {
        const char* name = _dyld_get_image_name(i);
        for (const char* system : kSystemImages) {
            if (name != nullptr && std::strstr(name, system) != nullptr) {
                systemCode[systemRanges++] = textOf(i);
                break;
            }
        }
    }
}

// An allocation is the system's when, between it and this program's code, the stack passes through
// the driver's or the display's frameworks.
bool madeBySystem(void* const* frames, int depth) {
    for (int i = 1; i < depth; ++i) {
        const auto pc = reinterpret_cast<uintptr_t>(frames[i]);
        if (ownCode.holds(pc)) {
            return false;
        }
        for (int r = 0; r < systemRanges; ++r) {
            if (systemCode[r].holds(pc)) {
                return true;
            }
        }
    }
    return false;
}

// Call stacks kept in place, since keeping them must not allocate either.
constexpr int kStackDepth = 32;
constexpr int kStacksKept = 256;
void* stacks[kStacksKept][kStackDepth];
int stackDepths[kStacksKept];
uintptr_t stackSizes[kStacksKept];
int stacksKept = 0;
bool keepingStacks = false;

void logAllocation(uint32_t type, uintptr_t, uintptr_t size, uintptr_t, uintptr_t, uint32_t) {
    if ((type & kLogTypeAllocate) == 0 || pthread_equal(pthread_self(), countedThread) == 0) {
        return;
    }
    ++allocations;
    void* frames[kStackDepth];
    const int depth = backtrace(frames, kStackDepth);
    if (!madeBySystem(frames, depth)) {
        ++ownAllocations;
    }
    if (keepingStacks && stacksKept < kStacksKept) {
        stackSizes[stacksKept] = size;
        stackDepths[stacksKept] = depth;
        std::memcpy(stacks[stacksKept], frames, sizeof frames);
        ++stacksKept;
    }
}

bool stacksWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("GEOMETRY_ALLOCS");
        return value != nullptr && std::strcmp(value, "stacks") == 0;
    }();
    return wanted;
}

} // namespace

bool allocationCountingEnabled() {
    static const bool enabled = std::getenv("GEOMETRY_ALLOCS") != nullptr;
    return enabled;
}

void startCountingAllocations() {
    countedThread = pthread_self();
    malloc_logger = logAllocation;
}

void stopCountingAllocations() {
    malloc_logger = nullptr;
    countedThread = nullptr;
}

u64 allocationCount() {
    if (countedThread != nullptr) {
        findCodeRanges();
    }
    return allocations;
}

u64 ownAllocationCount() {
    if (countedThread != nullptr) {
        findCodeRanges();
    }
    return ownAllocations;
}

void keepAllocationStacks() {
    stacksKept = 0;
    keepingStacks = stacksWanted();
}

void printAllocationStacks(int fd) {
    keepingStacks = false;
    char line[96];
    for (int i = 0; i < stacksKept; ++i) {
        const int length =
            std::snprintf(line, sizeof line, "allocation %d of %d, %lu bytes, %s:\n", i + 1,
                          stacksKept, static_cast<unsigned long>(stackSizes[i]),
                          madeBySystem(stacks[i], stackDepths[i]) ? "the system's" : "our code's");
        (void)write(fd, line, static_cast<size_t>(length));
        backtrace_symbols_fd(stacks[i], stackDepths[i], fd);
    }
}

} // namespace geo::engine
