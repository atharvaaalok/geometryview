// Text as the gallery sets it: IBM Plex Mono, whose every glyph has the same advance, so a line's
// width, where it is cut short and where its baseline falls are computed here, the same for the
// Mac's layers and the browser's DOM, with no font engine.
#pragma once

#include "base/types.h"

#include <string>
#include <string_view>

namespace geo::gallery {

/// IBM Plex Mono's metrics, in ems: every glyph's advance, and the ascent and descent the line
/// box is built from (hhea, which both Core Text and browsers use).
inline constexpr float kAdvance = 0.6f;
inline constexpr float kAscent = 1.025f;
inline constexpr float kDescent = 0.275f;

struct TextStyle {
    float size = 11.0f;    // points
    u16 weight = 400;      // 400 regular, 500 medium, 600 semibold
    float tracking = 0.0f; // extra space after every character, in ems
    bool upper = false;    // set in capitals

    bool operator==(const TextStyle&) const = default;
};

/// Characters, counted as Unicode code points.
usize characters(std::string_view utf8);

/// A line's width in points: each character's advance and its tracking.
float textWidth(std::string_view utf8, const TextStyle& style);

/// The text as set: in capitals when the style asks, then cut with an ellipsis to fit `width`.
std::string setText(std::string_view utf8, const TextStyle& style, float width);

/// Where a line's box starts, above its baseline, and the box's height: as a browser lays a line
/// whose height is its font's ascent and descent.
inline float ascentOf(const TextStyle& style) {
    return kAscent * style.size;
}
inline float lineHeightOf(const TextStyle& style) {
    return (kAscent + kDescent) * style.size;
}

// ---- Numbers, as the gallery writes them --------------------------------------------------------

/// A measurement to three significant figures at most: 605, 84.7, 4.25, 0.3.
std::string number(double value);
/// A length: "84.7 mm", or "1.2 m" from a metre up.
std::string millimetres(double value);
/// "1 file", "1,855 files".
std::string count(u64 n, std::string_view one, std::string_view many = {});
/// A box's sides: "40 × 30 × 26 mm", or "40×30×26" compact.
std::string dimensions(const double (&sides)[3], bool compact = false);
/// A whole number with thousands separated: "1,855".
std::string grouped(u64 n);

} // namespace geo::gallery
