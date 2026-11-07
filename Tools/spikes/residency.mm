// Spike 4: what happens to memory when a file-mapped Metal buffer is bound and released?
//
//   spike_residency <file> [cycles]
//
// Reports system wired memory and this process's footprint at each step, so a wired-memory leak
// shows up as a value that does not return to the baseline.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <fcntl.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <print>

namespace {

struct Memory {
    double wiredMb;
    double footprintMb;
};

Memory sample() {
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                      &count);
    task_vm_info_data_t task{};
    mach_msg_type_number_t taskCount = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&task), &taskCount);
    return {double(vm.wire_count) * double(vm_page_size) / 1e6, double(task.phys_footprint) / 1e6};
}

// Prints a wired-memory time series, because other processes move this system-wide counter.
void series(const char* label, const Memory& base, int samples, int intervalMs) {
    std::print("  {:<26}", label);
    double lowest = 1e12, highest = -1e12;
    for (int i = 0; i < samples; ++i) {
        usleep(useconds_t(intervalMs) * 1000);
        double delta = sample().wiredMb - base.wiredMb;
        lowest = std::min(lowest, delta);
        highest = std::max(highest, delta);
        std::print(" {:+7.0f}", delta);
    }
    std::println("   [{:+.0f}..{:+.0f}] footprint {:+.1f} MB", lowest, highest,
                 sample().footprintMb - base.footprintMb);
}

// Forces a real GPU read of one word, so the mapping is actually used.
void touch(id<MTLCommandQueue> queue, id<MTLBuffer> source, id<MTLBuffer> scratch) {
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:source sourceOffset:0 toBuffer:scratch destinationOffset:0 size:4];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
}

} // namespace

int main(int argc, const char** argv) {
    @autoreleasepool {
        if (argc < 2) {
            std::println(stderr, "usage: spike_residency <file> [cycles]");
            return 2;
        }
        int cycles = argc > 2 ? std::atoi(argv[2]) : 3;

        int fd = open(argv[1], O_RDONLY);
        struct stat info{};
        if (fd < 0 || fstat(fd, &info) != 0) {
            std::println(stderr, "could not open {}", argv[1]);
            return 1;
        }
        size_t page = size_t(getpagesize());
        size_t length = size_t(info.st_size) & ~(page - 1);

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> scratch = [device newBufferWithLength:16
                                                    options:MTLResourceStorageModeShared];
        std::println("file {:.0f} MB, wired deltas in MB, sampled every 500 ms",
                     double(length) / 1e6);

        Memory base = sample();
        series("control, nothing mapped", base, 12, 500);

        for (int cycle = 0; cycle < cycles; ++cycle) {
            std::println("cycle {}:", cycle + 1);
            void* mapping = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
            @autoreleasepool {
                id<MTLBuffer> buffer =
                    [device newBufferWithBytesNoCopy:mapping
                                              length:length
                                             options:MTLResourceStorageModeShared
                                                     | MTLResourceHazardTrackingModeUntracked
                                         deallocator:^(void* pointer, NSUInteger size) {
                                             munmap(pointer, size);
                                         }];
                NSError* error = nil;
                MTLResidencySetDescriptor* d = [MTLResidencySetDescriptor new];
                id<MTLResidencySet> set = [device newResidencySetWithDescriptor:d error:&error];
                [set addAllocation:buffer];
                [set commit];
                NSDate* start = [NSDate date];
                [set requestResidency];
                touch(queue, buffer, scratch);
                std::println("  residency and first GPU read: {:.1f} ms",
                             -start.timeIntervalSinceNow * 1e3);
                series("held, in use", base, 8, 500);
                // Rendering is on demand, so measure the first GPU read after going idle. This is
                // the cost a user would feel on the first hover after a pause. The two controls
                // separate the GPU waking up from the cost of a large or file-backed allocation.
                id<MTLBuffer> tiny = [device newBufferWithLength:16384
                                                         options:MTLResourceStorageModeShared];
                id<MTLBuffer> large = [device newBufferWithLength:length
                                                          options:MTLResourceStorageModeShared];
                memset(large.contents, 1, length);
                struct Case {
                    const char* name;
                    id<MTLBuffer> buffer;
                };
                for (const Case& c : {Case{"16 KB anonymous", tiny}, Case{"500 MB anonymous", large},
                                      Case{"500 MB file-backed", buffer}}) {
                    for (int idleSeconds : {1, 4}) {
                        for (int repeat = 0; repeat < 2; ++repeat) {
                            usleep(useconds_t(idleSeconds) * 1'000'000);
                            NSDate* resume = [NSDate date];
                            touch(queue, c.buffer, scratch);
                            double first = -resume.timeIntervalSinceNow * 1e3;
                            resume = [NSDate date];
                            touch(queue, c.buffer, scratch);
                            double second = -resume.timeIntervalSinceNow * 1e3;
                            std::println("  {:<20} {} s idle: first read {:6.2f} ms, next {:5.2f} ms",
                                         c.name, idleSeconds, first, second);
                        }
                    }
                }
                [set endResidency];
                [set removeAllocation:buffer];
                [set commit];
            }
            series("after release", base, 16, 500);
        }
        close(fd);
    }
    return 0;
}
