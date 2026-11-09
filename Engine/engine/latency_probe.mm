#include "engine/latency_probe.h"

#include "base/types.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace geo::engine {

LatencyProbe::LatencyProbe() : enabled_(std::getenv("GEOMETRYVIEW_LATENCY") != nullptr) {}

void LatencyProbe::report() {
    std::lock_guard lock(mutex_);
    if (samples_.empty()) {
        return;
    }
    const auto median = [&](auto&& value) {
        std::vector<double> values;
        values.reserve(samples_.size());
        for (const Sample& sample : samples_) {
            values.push_back(value(sample) * 1e3);
        }
        std::sort(values.begin(), values.end());
        return values[values.size() / 2];
    };
    std::vector<double> total;
    total.reserve(samples_.size());
    for (const Sample& sample : samples_) {
        total.push_back((sample.presented - sample.inputTime) * 1e3);
    }
    std::sort(total.begin(), total.end());
    std::fprintf(stderr,
                 "latency: %zu frames, input to present: median %.1f ms, p95 %.1f ms, "
                 "max %.1f ms\n",
                 total.size(), total[total.size() / 2], total[total.size() * 95 / 100],
                 total.back());
    std::fprintf(stderr,
                 "latency: input to frame start %.1f, start to commit deadline %.1f, "
                 "deadline to intended present %.1f, intended to actual %.1f ms\n",
                 median([](const Sample& s) { return s.callback - s.inputTime; }),
                 median([](const Sample& s) { return s.commitBy - s.callback; }),
                 median([](const Sample& s) { return s.showAt - s.commitBy; }),
                 median([](const Sample& s) { return s.presented - s.showAt; }));
}

void LatencyProbe::track(id<MTLDrawable> drawable, double inputTime, double callback,
                         double commitBy, double showAt) {
    [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        // A dropped drawable reports a presented time of zero.
        if (presented.presentedTime > 0.0) {
            std::lock_guard lock(mutex_);
            samples_.push_back({inputTime, callback, commitBy, showAt, presented.presentedTime});
        }
    }];
}

} // namespace geo::engine
