#include "step/part21.h"

#include <doctest/doctest.h>
#include <format>
#include <string>
#include <string_view>
#include <vector>

using namespace geo;
using namespace geo::import::step;

namespace {

std::span<const u8> bytes(std::string_view text) {
    return {reinterpret_cast<const u8*>(text.data()), text.size()};
}

constexpr std::string_view kHeader = R"(ISO-10303-21;
HEADER;
FILE_DESCRIPTION((''),'2;1');
FILE_NAME('fake DATA; name','',(''),(''),'','','');
FILE_SCHEMA(('AUTOMOTIVE_DESIGN'));
ENDSEC;
DATA;
)";

Part21 index(const std::string& file, std::vector<u8>& scratch) {
    Part21 result;
    std::string error;
    REQUIRE(indexPart21(bytes(file), result, scratch, error));
    return result;
}

std::string document(std::string_view data) {
    return std::string(kHeader) + std::string(data) + "ENDSEC;\nEND-ISO-10303-21;\n";
}

} // namespace

TEST_CASE("records carry their ids, types and resolved references") {
    const std::string file = document(R"(#10 = CARTESIAN_POINT('',(0.,0.,0.));
#20 = VERTEX_POINT('',#10);
#30=SOMETHING_ELSE('text with #99 and ; inside',#20,#10);
)");
    std::vector<u8> scratch;
    const Part21 p = index(file, scratch);
    REQUIRE(p.recordCount() == 3);
    CHECK(p.ids[0] == 10);
    CHECK(p.kinds[0] == Kind::CARTESIAN_POINT);
    CHECK(p.kinds[1] == Kind::VERTEX_POINT);
    CHECK(p.kinds[2] == Kind::Other);
    REQUIRE(p.refsOf(1).size() == 1);
    CHECK(p.refsOf(1)[0] == 0);
    REQUIRE(p.refsOf(2).size() == 2);
    CHECK(p.refsOf(2)[0] == 1);
    CHECK(p.refsOf(2)[1] == 0);
    CHECK(p.text(1) == "#20 = VERTEX_POINT('',#10);");
    CHECK(p.header.ends_with("DATA;\n"));
    CHECK_FALSE(p.needsWholeFile);
}

TEST_CASE("a doubled quote stays inside its string") {
    const std::string file = document("#1 = PRODUCT('it''s; #5 fine',#2);\n#2 = PRODUCT('b');\n");
    std::vector<u8> scratch;
    const Part21 p = index(file, scratch);
    REQUIRE(p.recordCount() == 2);
    REQUIRE(p.refsOf(0).size() == 1);
    CHECK(p.refsOf(0)[0] == 1);
}

TEST_CASE("a dangling reference is kept as missing") {
    const std::string file = document("#1 = VERTEX_POINT('',#2);\n");
    std::vector<u8> scratch;
    const Part21 p = index(file, scratch);
    REQUIRE(p.refsOf(0).size() == 1);
    CHECK(p.refsOf(0)[0] == kNoRecord);
}

TEST_CASE("forward references and sparse ids resolve") {
    const std::string file =
        document("#4000000000 = VERTEX_POINT('',#7);\n#7 = CARTESIAN_POINT('',(1.,2.,3.));\n");
    std::vector<u8> scratch;
    const Part21 p = index(file, scratch);
    REQUIRE(p.recordCount() == 2);
    CHECK(p.refsOf(0)[0] == 1);
}

TEST_CASE("complex entities are classified by their most telling type") {
    const std::string file = document(R"(#1 = ( REPRESENTATION_RELATIONSHIP('','',#2,#3)
REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION(#4)
SHAPE_REPRESENTATION_RELATIONSHIP() );
#2 = ( GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNIT_ASSIGNED_CONTEXT((#3)) REPRESENTATION_CONTEXT('','') );
#3 = ( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) );
#4 = ( BOUNDED_CURVE() CURVE() );
)");
    std::vector<u8> scratch;
    const Part21 p = index(file, scratch);
    REQUIRE(p.recordCount() == 4);
    CHECK(p.kinds[0] == Kind::REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION);
    CHECK(p.kinds[1] == Kind::GEOMETRIC_REPRESENTATION_CONTEXT);
    CHECK(p.kinds[2] == Kind::SI_UNIT);
    CHECK(p.kinds[3] == Kind::Complex);
    REQUIRE(p.refsOf(0).size() == 3);
    CHECK(p.refsOf(0)[2] == 3);
}

TEST_CASE("comments are blanked, even when they hold quotes and semicolons") {
    const std::string file =
        document("/* it's; #9 */\n#1 = VERTEX_POINT('', /* ; */ #2);\n#2 = PRODUCT('/* no */');\n");
    std::vector<u8> scratch;
    const Part21 p = index(file, scratch);
    REQUIRE(p.recordCount() == 2);
    CHECK(p.ids[0] == 1);
    REQUIRE(p.refsOf(0).size() == 1);
    CHECK(p.refsOf(0)[0] == 1);
    ParamCursor cursor(p.text(1));
    std::string value;
    CHECK(cursor.readString(value));
    CHECK(value == "/* no */");
}

TEST_CASE("external scopes need the full reader") {
    const std::string file = document("#1 = &SCOPE #2 = PRODUCT('a'); ENDSCOPE;\n");
    std::vector<u8> scratch;
    const Part21 p = index(file, scratch);
    CHECK(p.needsWholeFile);
}

TEST_CASE("a file with no data section is rejected") {
    Part21 result;
    std::vector<u8> scratch;
    std::string error;
    CHECK_FALSE(indexPart21(bytes("this is not a step file"), result, scratch, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("large files split across chunks without losing records or string state") {
    std::string data;
    constexpr u32 kRecords = 400000;
    for (u32 i = 1; i <= kRecords; ++i) {
        data += std::format("#{} = PRODUCT('name; ''{}'' #1',#{},\n  $);\n", i, i,
                            i == kRecords ? 1 : i + 1);
    }
    const std::string file = document(data);
    REQUIRE(file.size() > (8 << 20) * 2);
    std::vector<u8> scratch;
    const Part21 p = index(file, scratch);
    REQUIRE(p.recordCount() == kRecords);
    for (u32 r = 0; r < kRecords; r += 9973) {
        CHECK(p.ids[r] == r + 1);
        REQUIRE(p.refsOf(r).size() == 1);
        CHECK(p.refsOf(r)[0] == (r + 1) % kRecords);
    }
    CHECK(p.text(kRecords - 1).ends_with(";"));
}

TEST_CASE("a record is kept wherever a chunk boundary falls around it") {
    constexpr usize kChunk = 8 << 20;
    for (const int shift : {-2, -1, 0, 1, 2}) {
        // Fills up to the boundary with one long record, then places the next record around it.
        const usize boundary = kHeader.size() + kChunk;
        std::string file(kHeader);
        file += "#1 = PRODUCT('";
        const usize fill = boundary + usize(i64(shift)) - file.size() - std::string("');\n").size();
        file.append(fill, 'x');
        file += "');\n#2 = PRODUCT('two',#3);\n#3 = PRODUCT('three',#2);\nENDSEC;\n";
        std::vector<u8> scratch;
        const Part21 p = index(file, scratch);
        REQUIRE(p.recordCount() == 3);
        CHECK(p.ids[1] == 2);
        CHECK(p.refsOf(1)[0] == 2);
        CHECK(p.refsOf(2)[0] == 1);
    }
}

TEST_CASE("the cursor reads every parameter form") {
    const std::string_view record =
        R"(#1 = THING('a''b',1.5,-2,.T.,#42,(1.,2.E1,3.),*,$,LENGTH_MEASURE(25.4));)";
    ParamCursor cursor(record);
    REQUIRE(cursor.valid());
    std::string text;
    REQUIRE(cursor.readString(text));
    CHECK(text == "a'b");
    double value = 0;
    REQUIRE(cursor.readReal(value));
    CHECK(value == 1.5);
    REQUIRE(cursor.readReal(value));
    CHECK(value == -2.0);
    std::string_view word;
    REQUIRE(cursor.readEnum(word));
    CHECK(word == "T");
    u32 id = 0;
    REQUIRE(cursor.readId(id));
    CHECK(id == 42);
    REQUIRE(cursor.enterList());
    double sum = 0;
    while (cursor.inList()) {
        REQUIRE(cursor.readReal(value));
        sum += value;
    }
    CHECK(sum == 24.0);
    REQUIRE(cursor.leaveList());
    CHECK(cursor.skip());
    CHECK(cursor.skip());
    REQUIRE(cursor.readReal(value));
    CHECK(value == 25.4);
    CHECK(cursor.atEnd());
    CHECK_FALSE(cursor.skip());
}

TEST_CASE("the cursor tolerates spaces inside typed values") {
    ParamCursor cursor("#1 = LENGTH_MEASURE_WITH_UNIT( LENGTH_MEASURE( 1.5 ), #2 );");
    double value = 0;
    REQUIRE(cursor.readReal(value));
    CHECK(value == 1.5);
    u32 id = 0;
    REQUIRE(cursor.readId(id));
    CHECK(id == 2);
}

TEST_CASE("the cursor finds a sub-entity in a complex record") {
    const std::string_view record =
        "#1 = ( A(1.) CONVERSION_BASED_UNIT('INCH',#7) LENGTH_UNIT() );";
    ParamCursor cursor(record, "CONVERSION_BASED_UNIT");
    REQUIRE(cursor.valid());
    std::string name;
    REQUIRE(cursor.readString(name));
    CHECK(name == "INCH");
    u32 id = 0;
    REQUIRE(cursor.readId(id));
    CHECK(id == 7);
    CHECK_FALSE(ParamCursor(record, "MISSING").valid());
}

TEST_CASE("strings decode STEP escapes to UTF-8") {
    const std::string_view record = R"(#1 = P('caf\X\E9 \X2\03B103B2\X0\!');)";
    ParamCursor cursor(record);
    std::string text;
    REQUIRE(cursor.readString(text));
    CHECK(text == "caf\xC3\xA9 \xCE\xB1\xCE\xB2!");
}

TEST_CASE("a wrong parameter type fails without moving") {
    ParamCursor cursor("#1 = P(#7,'x');");
    double value = 0;
    std::string text;
    CHECK_FALSE(cursor.readReal(value));
    CHECK_FALSE(cursor.readString(text));
    u32 id = 0;
    CHECK(cursor.readId(id));
    CHECK(id == 7);
}
