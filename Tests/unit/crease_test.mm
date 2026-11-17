// A crease is inked wherever it can be seen, from any side and at any angle: crease_fixture.h. The
// matching case in webgpu_test.mm checks the WebGPU renderer the same way.

#if defined(GEO_TEST_WORKER_PATH)

#include "crease_fixture.h"
#include "gpu/renderer.h"

#import <Metal/Metal.h>

#include <doctest/doctest.h>
#include <unistd.h>

using namespace geo;

// Before, close in perspective a long straight crease reaching behind the camera was left out
// whole, and seen steeply a face rising beside a crease covered its line: of 792 views, 206 creases
// were short of ink, some of all of it.
TEST_CASE("every crease of a grooved block is inked wherever it shows, from every side") {
    @autoreleasepool {
        const std::string blob = test::importGrooveBlock(GEO_TEST_WORKER_PATH);
        std::unique_ptr<gpu::Renderer> renderer =
            gpu::Renderer::create(MTLCreateSystemDefaultDevice(), GEO_TEST_SHADER_LIBRARY);
        REQUIRE(renderer != nullptr);
        const test::CreaseSweep sweep = test::sweepCreases(*renderer, blob);
        unlink(blob.c_str());
        CAPTURE(sweep.views);
        CAPTURE(sweep.worstView);
        CHECK(sweep.views > 300);
        CHECK(sweep.short_ == 0);
    }
}

#endif
