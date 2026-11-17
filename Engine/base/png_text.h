// Text in a PNG: tEXt chunks, which image viewers and file inspectors show as the picture's
// metadata (its Software, a Comment). Written straight into the encoded bytes, so any encoder's
// output can carry them.
#pragma once

#include "base/types.h"

#include <span>
#include <string_view>
#include <vector>

namespace geo {

struct PngText {
    std::string_view keyword; // 1 to 79 Latin-1 characters, such as "Software"
    std::string_view text;    // Latin-1
};

/// `png` with `texts` as tEXt chunks just after its header, replacing any tEXt chunk with the same
/// keyword, so tagging again changes nothing. Empty if `png` is not a whole PNG.
std::vector<u8> withPngText(std::span<const u8> png, std::span<const PngText> texts);

/// The CRC-32 a PNG chunk ends with, over its type and data.
u32 pngCrc(std::span<const u8> bytes);

} // namespace geo
