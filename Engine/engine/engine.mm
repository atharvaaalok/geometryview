// Implements the C API and runs the engine thread.
//
// The main thread only ever enqueues. The engine thread drains the queue, updates view state and
// renders, which makes it the single writer of that state and removes most locking.
#include "engine.h"

#include "base/assert.h"
#include "base/spsc_ring.h"
#include "base/time.h"
#include "gpu/frame_source.h"
#include "gpu/renderer.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <pthread.h>

namespace {

using namespace geo;

// ProMotion drops its refresh rate when presents stop, so frames keep flowing briefly after the
// last input, which keeps the next interaction at full rate.
constexpr double kLingerSeconds = 0.25;
constexpr usize kCommandCapacity = 4096;

struct ResizeCommand {
    u32 width, height;
    float scale;
};

struct Command {
    enum class Kind : u8 { AttachLayer, Resize, Pointer, RequestFrame, Stop };
    Kind kind;
    union {
        void* layer;
        ResizeCommand resize;
        GeoPointerEvent pointer;
    };
};

} // namespace

struct GeoEngine {
    SpscRing<Command, kCommandCapacity> commands;
    CFRunLoopRef runLoop = nullptr;
    CFRunLoopSourceRef source = nullptr;
    pthread_t thread{};
    std::atomic<bool> ready{false};

    id<MTLDevice> device = nil;
    std::unique_ptr<gpu::Renderer> renderer;
    gpu::DisplayLinkFrameSource frames;

    bool needsFrame = false;
    bool stopping = false;
    double lastActivity = 0.0;

    std::atomic<u64> framesRendered{0};
    std::atomic<double> lastFrameSeconds{0.0};

    void post(const Command& command) {
        if (commands.tryPush(command)) {
            CFRunLoopSourceSignal(source);
            CFRunLoopWakeUp(runLoop);
        }
    }

    void requestFrame() {
        needsFrame = true;
        lastActivity = nowSeconds();
        frames.setRunning(true);
    }

    void drainCommands() {
        while (auto command = commands.tryPop()) {
            switch (command->kind) {
            case Command::Kind::AttachLayer:
                attachLayer((__bridge_transfer CAMetalLayer*)command->layer);
                break;
            case Command::Kind::Resize:
            case Command::Kind::RequestFrame:
                requestFrame();
                break;
            case Command::Kind::Pointer:
                lastActivity = nowSeconds();
                break;
            case Command::Kind::Stop:
                stopping = true;
                CFRunLoopStop(runLoop);
                break;
            }
        }
    }

    void attachLayer(CAMetalLayer* layer) {
        renderer->attachLayer(layer);
        frames.attach(layer, [this](const gpu::FrameTick& tick) { onFrame(tick); });
        requestFrame();
    }

    void onFrame(const gpu::FrameTick& tick) {
        if (needsFrame) {
            needsFrame = false;
            const double start = nowSeconds();
            renderer->render(tick.drawable);
            lastFrameSeconds.store(nowSeconds() - start, std::memory_order_relaxed);
            framesRendered.fetch_add(1, std::memory_order_relaxed);
        } else if (nowSeconds() - lastActivity > kLingerSeconds) {
            frames.setRunning(false);
        }
    }

    void run() {
        pthread_setname_np("geometry.engine");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        @autoreleasepool {
            runLoop = CFRunLoopGetCurrent();
            CFRunLoopSourceContext context{};
            context.info = this;
            context.perform = [](void* info) {
                @autoreleasepool {
                    static_cast<GeoEngine*>(info)->drainCommands();
                }
            };
            source = CFRunLoopSourceCreate(nullptr, 0, &context);
            CFRunLoopAddSource(runLoop, source, kCFRunLoopCommonModes);
            ready.store(true, std::memory_order_release);
            ready.notify_one();
            while (!stopping) {
                CFRunLoopRun();
            }
            CFRunLoopRemoveSource(runLoop, source, kCFRunLoopCommonModes);
            CFRelease(source);
        }
    }
};

const char* geo_engine_version(void) {
    return "0.1.0";
}

GeoEngine* geo_engine_create(void) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    std::unique_ptr<gpu::Renderer> renderer = device ? gpu::Renderer::create(device) : nullptr;
    if (!renderer) {
        return nullptr;
    }
    auto* engine = new GeoEngine;
    engine->device = device;
    engine->renderer = std::move(renderer);

    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    const int result = pthread_create(
        &engine->thread, &attributes,
        [](void* argument) -> void* {
            static_cast<GeoEngine*>(argument)->run();
            return nullptr;
        },
        engine);
    pthread_attr_destroy(&attributes);
    GEO_ASSERT(result == 0);
    engine->ready.wait(false, std::memory_order_acquire);
    return engine;
}

void geo_engine_destroy(GeoEngine* engine) {
    if (!engine) {
        return;
    }
    Command command{};
    command.kind = Command::Kind::Stop;
    engine->post(command);
    pthread_join(engine->thread, nullptr);
    delete engine;
}

void geo_engine_attach_layer(GeoEngine* engine, void* metal_layer) {
    Command command{};
    command.kind = Command::Kind::AttachLayer;
    command.layer = (__bridge_retained void*)(__bridge CAMetalLayer*)metal_layer;
    engine->post(command);
}

void geo_engine_resize(GeoEngine* engine, uint32_t width, uint32_t height, float scale) {
    Command command{};
    command.kind = Command::Kind::Resize;
    command.resize = {width, height, scale};
    engine->post(command);
}

void geo_engine_post_pointer(GeoEngine* engine, const GeoPointerEvent* event) {
    Command command{};
    command.kind = Command::Kind::Pointer;
    command.pointer = *event;
    engine->post(command);
}

void geo_engine_request_frame(GeoEngine* engine) {
    Command command{};
    command.kind = Command::Kind::RequestFrame;
    engine->post(command);
}

GeoEngineStats geo_engine_stats(const GeoEngine* engine) {
    return {engine->framesRendered.load(std::memory_order_relaxed),
            engine->lastFrameSeconds.load(std::memory_order_relaxed)};
}
