// Fuzzes the STEP tokenizer and parameter cursor: any bytes must index or fail cleanly, and every
// record must read without leaving its text.
#include "step/part21.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace geo;
using namespace geo::import::step;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Part21 index;
    std::vector<u8> scratch;
    std::string error;
    if (!indexPart21({data, size}, index, scratch, error)) {
        return 0;
    }
    for (u32 record = 0; record < index.recordCount() && record < 4096; ++record) {
        for (const u32 ref : index.refsOf(record)) {
            if (ref != kNoRecord && ref >= index.recordCount()) {
                __builtin_trap(); // a reference must name a record or be dangling
            }
        }
        ParamCursor cursor(index.text(record));
        std::string text;
        double real = 0.0;
        std::string_view name;
        u32 id = 0;
        for (int step = 0; step < 64 && cursor.valid() && !cursor.atEnd(); ++step) {
            if (!(cursor.readString(text) || cursor.readReal(real) || cursor.readEnum(name) ||
                  cursor.readId(id) || cursor.enterList() || cursor.leaveList() || cursor.skip())) {
                break;
            }
        }
    }
    return 0;
}
