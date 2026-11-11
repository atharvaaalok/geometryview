// The gallery's import queue, run through the cases it shares with the browser's.
#include "library/queue.h"

#include "base/json.h"

#include <doctest/doctest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace geo;
using library::Queue;

namespace {

json::Value sharedCases() {
    std::ifstream file(GEO_TEST_DATA_DIR "/gallery-queue.json");
    std::stringstream text;
    text << file.rdbuf();
    json::Value value;
    std::string error;
    REQUIRE_MESSAGE(json::parse(text.str(), value, error), error);
    return value;
}

std::vector<std::string> strings(const json::Value& value) {
    std::vector<std::string> out;
    for (const json::Value& item : value.items()) {
        out.push_back(item.asString());
    }
    return out;
}

void runStep(Queue& queue, const json::Value& step) {
    const auto& [op, value] = step.members().front();
    CAPTURE(op);
    CAPTURE(value.write());
    if (op == "want") {
        std::vector<library::Want> wants;
        for (const json::Value& job : value.items()) {
            const std::optional<library::Band> band = library::bandNamed(job[1].asString());
            REQUIRE(band.has_value());
            wants.push_back({job[0].asString(), *band, job[2].asNumber(), u64(job[3].asNumber())});
        }
        queue.want(wants);
    } else if (op == "next") {
        const std::optional<std::string> started = queue.next();
        if (value.isNull()) {
            CHECK_FALSE(started.has_value());
        } else {
            REQUIRE(started.has_value());
            CHECK(*started == value.asString());
        }
    } else if (op == "finish") {
        queue.finish(value.asString());
    } else if (op == "stopped") {
        queue.stopped(value.asString());
    } else if (op == "cancel") {
        CHECK(queue.toCancel() == strings(value));
    } else if (op == "waiting") {
        CHECK(queue.waiting() == strings(value));
    } else if (op == "running") {
        CHECK(queue.running() == strings(value));
    } else if (op == "slots") {
        queue.setSlots(u32(value.asNumber()));
    } else if (op == "prefetch") {
        queue.setPrefetch(value.asBool());
    } else {
        FAIL("unknown step ", op);
    }
}

} // namespace

TEST_CASE("the import queue runs the cases it shares with the browser") {
    const json::Value cases = sharedCases()["cases"];
    REQUIRE(cases.size() > 0);
    for (const json::Value& c : cases.items()) {
        CAPTURE(c["name"].asString());
        Queue queue(u32(c["slots"].asNumber(1)), c["prefetch"].asBool(true));
        for (const json::Value& step : c["steps"].items()) {
            runStep(queue, step);
        }
    }
}

TEST_CASE("the import queue starts thousands of wanted files in order, quickly") {
    Queue queue(4);
    std::vector<library::Want> wants;
    for (u32 i = 0; i < 20000; ++i) {
        wants.push_back({std::to_string(i),
                         i < 40 ? library::Band::Visible : library::Band::Prefetch, double(i)});
    }
    for (int scroll = 0; scroll < 50; ++scroll) {
        queue.want(wants);
    }
    std::string last;
    u32 started = 0;
    while (const std::optional<std::string> id = queue.next()) {
        CHECK(std::stoul(*id) == started);
        queue.finish(*id);
        ++started;
    }
    CHECK(started == 20000);
    CHECK(queue.idle());
}
