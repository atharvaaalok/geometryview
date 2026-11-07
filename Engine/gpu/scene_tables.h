// The GPU's view of a scene: definitions, placed occurrences and the batches that draw them.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>

#include "gpu/gpu_context.h"
#include "scene/scene.h"

#include <unordered_map>

namespace geo::gpu {

/// Draw lists of batches, each batch one object threadgroup.
struct BatchRange {
    usize first = 0, last = 0;
    bool empty() const { return first == last; }
};

class SceneTables {
public:
    /// Rebuilds the tables for `scene`. Never waits for the GPU: tables a frame may still read are
    /// retired, and mapped geometry stays alive with them.
    void sync(GpuContext& context, Retirement& retirement, const scene::Scene& scene);

    bool empty() const { return surfaceBatchCount_ == 0; }

    id<MTLBuffer> definitions() const { return definitions_; }
    id<MTLBuffer> occurrences() const { return occurrences_; }
    id<MTLBuffer> batches() const { return batches_; }
    id<MTLBuffer> meshletRefs() const { return meshletRefs_; }
    id<MTLBuffer> drawBases() const { return drawBases_; }

    /// Closed surfaces draw with back faces culled, and open ones show both sides.
    BatchRange closedBatches() const { return {0, openBatchStart_}; }
    BatchRange openBatches() const { return {openBatchStart_, surfaceBatchCount_}; }

private:
    struct SegmentResidency {
        id<MTLBuffer> buffer;
        id<MTLResidencySet> set;
    };
    std::unordered_map<const scene::MappedSegment*, SegmentResidency> segments_;
    id<MTLBuffer> definitions_, occurrences_, batches_, drawBases_, meshletRefs_;
    usize surfaceBatchCount_ = 0;
    usize openBatchStart_ = 0;
};

} // namespace geo::gpu
#endif
