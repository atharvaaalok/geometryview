// What every part of the renderer shares: the device, the queue, residency, and buffers waiting
// for the frames that read them to finish.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>

#include "base/types.h"

#include <vector>

namespace geo::gpu {

struct GpuContext {
    id<MTLDevice> device;
    id<MTL4CommandQueue> queue;
    id<MTLResidencySet> residency; // shared by everything not mapped from a cache file
    id<MTLSharedEvent> frameFinished;
    u64 frameCount = 0; // the frame being encoded, or the last one

    id<MTLBuffer> makeShared(usize length) const {
        return [device newBufferWithLength:std::max<usize>(length, 16)
                                   options:MTLResourceStorageModeShared |
                                           MTLResourceHazardTrackingModeUntracked];
    }
};

/// Buffers and residency sets that a frame in flight may still read. Each leaves residency, and
/// memory, once the frame that last used it has finished.
class Retirement {
public:
    void retire(u64 frame, std::vector<id<MTLBuffer>> buffers,
                std::vector<id<MTLResidencySet>> sets = {}) {
        if (!buffers.empty() || !sets.empty()) {
            entries_.push_back({frame, std::move(buffers), std::move(sets)});
        }
    }

    void releaseFinished(const GpuContext& context) {
        if (entries_.empty()) {
            return;
        }
        const u64 finished = context.frameFinished.signaledValue;
        usize kept = 0;
        for (Entry& entry : entries_) {
            if (entry.frame > finished) {
                entries_[kept++] = std::move(entry);
                continue;
            }
            for (id<MTLBuffer> buffer : entry.buffers) {
                [context.residency removeAllocation:buffer];
            }
            for (id<MTLResidencySet> set : entry.sets) {
                [context.queue removeResidencySet:set];
            }
        }
        if (kept != entries_.size()) {
            entries_.resize(kept);
            [context.residency commit];
        }
    }

private:
    struct Entry {
        u64 frame = 0;
        std::vector<id<MTLBuffer>> buffers;    // in the shared residency set
        std::vector<id<MTLResidencySet>> sets; // a segment's own set
    };
    std::vector<Entry> entries_;
};

} // namespace geo::gpu
#endif
