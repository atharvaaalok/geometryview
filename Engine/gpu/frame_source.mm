#include "gpu/frame_source.h"

@interface GeoDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic) geo::gpu::DisplayLinkFrameSource::Callback callback;
@end

@implementation GeoDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update {
    (void)link;
    if (_callback) {
        _callback({update.drawable, update.targetPresentationTimestamp});
    }
}
@end

namespace geo::gpu {

struct DisplayLinkFrameSource::State {
    CAMetalDisplayLink* link = nil;
    GeoDisplayLinkDelegate* delegate = nil;
};

DisplayLinkFrameSource::DisplayLinkFrameSource() : state_(new State) {}

DisplayLinkFrameSource::~DisplayLinkFrameSource() {
    [state_->link invalidate];
    delete state_;
}

void DisplayLinkFrameSource::attach(CAMetalLayer* layer, Callback callback) {
    [state_->link invalidate];
    state_->delegate = [GeoDisplayLinkDelegate new];
    state_->delegate.callback = std::move(callback);
    state_->link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    state_->link.delegate = state_->delegate;
    state_->link.preferredFrameLatency = 1.0f;
    state_->link.preferredFrameRateRange = CAFrameRateRangeMake(30.0f, 120.0f, 120.0f);
    state_->link.paused = YES;
    [state_->link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
}

void DisplayLinkFrameSource::setRunning(bool running) {
    state_->link.paused = running ? NO : YES;
}

bool DisplayLinkFrameSource::isRunning() const {
    return state_->link && !state_->link.isPaused;
}

} // namespace geo::gpu
