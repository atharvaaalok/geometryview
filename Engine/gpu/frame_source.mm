#include "gpu/frame_source.h"

#include "base/time.h"

namespace {

// Time reserved between starting a frame and its commit deadline. Encoding takes under 0.4 ms on
// real scenes, and this leaves room for a slow frame or a late timer.
constexpr double kStartBudgetSeconds = 0.004;
constexpr CFTimeInterval kFarFuture = 1e9;

} // namespace

@interface GeoFrameDelegate : NSObject <CAMetalDisplayLinkDelegate>
@property (nonatomic) geo::gpu::DisplayLinkFrameSource::Callback callback;
@end

@implementation GeoFrameDelegate {
@public
    CFRunLoopTimerRef _timer;
    id<CAMetalDrawable> _pendingDrawable;
    double _pendingCommitDeadline;
    double _pendingPresentationTime;
}

- (void)deliverPending {
    id<CAMetalDrawable> drawable = _pendingDrawable;
    _pendingDrawable = nil;
    if (drawable && _callback) {
        _callback({drawable, _pendingCommitDeadline, _pendingPresentationTime});
    }
}

- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update {
    (void)link;
    _pendingDrawable = update.drawable;
    _pendingCommitDeadline = update.targetTimestamp;
    _pendingPresentationTime = update.targetPresentationTimestamp;

    const double delay = update.targetTimestamp - kStartBudgetSeconds - geo::nowSeconds();
    if (delay <= 0.0) {
        [self deliverPending];
        return;
    }
    // One timer, re-armed each frame, so pacing allocates nothing. It is a timer rather than a
    // sleep so the run loop keeps draining input until the frame starts.
    CFRunLoopTimerSetNextFireDate(_timer, CFAbsoluteTimeGetCurrent() + delay);
}

@end

namespace geo::gpu {

struct DisplayLinkFrameSource::State {
    CAMetalDisplayLink* link = nil;
    GeoFrameDelegate* delegate = nil;

    void stop() {
        [link invalidate];
        link = nil;
        if (delegate != nil && delegate->_timer != nullptr) {
            CFRunLoopTimerInvalidate(delegate->_timer);
            CFRelease(delegate->_timer);
            delegate->_timer = nullptr;
        }
    }
};

DisplayLinkFrameSource::DisplayLinkFrameSource() : state_(new State) {}

DisplayLinkFrameSource::~DisplayLinkFrameSource() {
    state_->stop();
    delete state_;
}

void DisplayLinkFrameSource::attach(CAMetalLayer* layer, Callback callback) {
    state_->stop();
    GeoFrameDelegate* delegate = [GeoFrameDelegate new];
    delegate.callback = std::move(callback);
    state_->delegate = delegate;

    __weak GeoFrameDelegate* weakDelegate = delegate;
    delegate->_timer = CFRunLoopTimerCreateWithHandler(kCFAllocatorDefault,
                                                       CFAbsoluteTimeGetCurrent() + kFarFuture,
                                                       kFarFuture, 0, 0, ^(CFRunLoopTimerRef) {
                                                           [weakDelegate deliverPending];
                                                       });
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), delegate->_timer, kCFRunLoopCommonModes);

    state_->link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    state_->link.delegate = delegate;
    state_->link.preferredFrameLatency = 1.0f;
    state_->link.preferredFrameRateRange = CAFrameRateRangeMake(30.0f, 120.0f, 120.0f);
    state_->link.paused = YES;
    [state_->link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
}

void DisplayLinkFrameSource::setRunning(bool running) { state_->link.paused = running ? NO : YES; }

bool DisplayLinkFrameSource::isRunning() const { return state_->link && !state_->link.isPaused; }

} // namespace geo::gpu
