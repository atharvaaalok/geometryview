#include "base/json.h"

#include <doctest/doctest.h>
#include <string>

using geo::json::Value;

TEST_CASE("json reads what it writes") {
    Value facts = Value::object();
    facts.set("name", "Drive \"A\"\n");
    facts.set("parts", 3);
    facts.set("estimated", false);
    Value size = Value::array();
    size.push(40.5);
    size.push(-2);
    size.push(1e-7);
    facts.set("size", std::move(size));
    facts.set("nothing", Value());

    const std::string text = facts.write();
    CHECK(text == R"({"name":"Drive \"A\"\n","parts":3,"estimated":false,"size":[40.5,-2,1e-07],)"
                  R"("nothing":null})");

    Value back;
    std::string error;
    REQUIRE(geo::json::parse(text, back, error));
    CHECK(back["name"].asString() == "Drive \"A\"\n");
    CHECK(back["parts"].asNumber() == 3);
    CHECK_FALSE(back["estimated"].asBool(true));
    CHECK(back["size"].size() == 3);
    CHECK(back["size"][0].asNumber() == 40.5);
    CHECK(back["size"][2].asNumber() == doctest::Approx(1e-7));
    CHECK(back["nothing"].isNull());
    CHECK(back["missing"].isNull());
    CHECK(back["size"][7].isNull());
    CHECK(back.write() == text);
}

TEST_CASE("json setting a member again replaces it, in place") {
    Value object = Value::object();
    object.set("a", 1);
    object.set("b", 2);
    object.set("a", "one");
    CHECK(object.write() == R"({"a":"one","b":2})");
    // A copy is its own.
    Value copy = object;
    copy.set("b", 3);
    CHECK(object["b"].asNumber() == 2);
}

TEST_CASE("json reads escapes, surrogate pairs and whitespace") {
    Value value;
    std::string error;
    REQUIRE(geo::json::parse(" [ \"\\u00e9\\ud83d\\ude00\\/\" , true , -0.5e1 ] ", value, error));
    CHECK(value[0].asString() == "\xC3\xA9\xF0\x9F\x98\x80/");
    CHECK(value[1].asBool());
    CHECK(value[2].asNumber() == -5.0);
}

TEST_CASE("json refuses what is not one value") {
    Value value;
    std::string error;
    for (const char* text : {"", "{", "[1,]", "{\"a\" 1}", "tru", "01x", "\"\\q\"", "1 2",
                             "\"unterminated", "{\"a\":1,}", "-", "1.", "\"a\nb\""}) {
        CAPTURE(text);
        CHECK_FALSE(geo::json::parse(text, value, error));
        CHECK_FALSE(error.empty());
    }
    // Deep nesting is refused, not recursed into.
    const std::string deep(200, '[');
    CHECK_FALSE(geo::json::parse(deep, value, error));
    CHECK(error.find("deeply") != std::string::npos);
}
