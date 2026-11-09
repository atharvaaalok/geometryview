// Measures the time from an input event to the frame showing it being presented.
//
// This starts at the event's timestamp and ends at the time the display reports for the present, so
// it excludes the mouse's own latency and the panel's response. Enable with GEOMETRYVIEW_LATENCY=1.
#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>

#include <mutex>
#include <vector>

namespace geo::engine {

class LatencyProbe {
public:
    LatencyProbe();

    bool enabled() const { return enabled_; }
    /// `callback` is when the frame started, and `commitBy` and `showAt` are the display link's
    /// deadline and intended presentation time.
    void track(id<MTLDrawable> drawable, double inputTime, double callback, double commitBy,
               double showAt);

    /// Prints the median, 95th percentile and maximum so far. Safe from any thread.
    void report();

private:
    bool enabled_;
    std::mutex mutex_; // presented handlers run on a Metal thread
    struct Sample {
        double inputTime, callback, commitBy, showAt, presented;
    };
    std::vector<Sample> samples_;
};

} // namespace geo::engine
#endif
