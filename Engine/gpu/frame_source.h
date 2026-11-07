// Decides when frames happen and supplies the drawable to render into.
//
// Rendering is on demand. The source stops completely when nothing needs drawing, so an idle window
// costs no CPU or GPU time, and it can be restarted from any point in the engine thread's loop.
//
// Frames start just before the commit deadline rather than as soon as the display link fires.
// Encoding takes well under a millisecond, so starting at once would leave a finished frame waiting
// while newer input arrives. ADR 0005 has the measurements.
#pragma once

#ifdef __OBJC__
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>

#include <functional>

namespace geo::gpu {

struct FrameTick {
    id<CAMetalDrawable> drawable;
    double commitDeadline;   // work must be committed by now to make `presentationTime`
    double presentationTime; // when the frame is intended to reach the display
};

class DisplayLinkFrameSource {
public:
    using Callback = std::function<void(const FrameTick&)>;

    DisplayLinkFrameSource();
    ~DisplayLinkFrameSource();
    DisplayLinkFrameSource(const DisplayLinkFrameSource&) = delete;
    DisplayLinkFrameSource& operator=(const DisplayLinkFrameSource&) = delete;

    /// Binds to `layer` on the calling thread, whose run loop must be running.
    void attach(CAMetalLayer* layer, Callback callback);
    void setRunning(bool running);
    bool isRunning() const;

private:
    struct State;
    State* state_;
};

} // namespace geo::gpu
#endif
