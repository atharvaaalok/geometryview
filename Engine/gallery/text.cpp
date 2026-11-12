#include "gallery/text.h"

#include <cmath>
#include <format>

namespace geo::gallery {

usize characters(std::string_view utf8) {
    usize n = 0;
    for (const char c : utf8) {
        n += (u8(c) & 0xC0) != 0x80 ? 1 : 0; // continuation bytes are not characters
    }
    return n;
}

float textWidth(std::string_view utf8, const TextStyle& style) {
    return float(characters(utf8)) * (kAdvance + style.tracking) * style.size;
}

std::string setText(std::string_view utf8, const TextStyle& style, float width) {
    std::string text(utf8);
    if (style.upper) {
        for (char& c : text) {
            if (c >= 'a' && c <= 'z') {
                c = char(c - 'a' + 'A');
            }
        }
    }
    const float per = (kAdvance + style.tracking) * style.size;
    const usize n = characters(text);
    if (width <= 0.0f || float(n) * per <= width + 0.01f) {
        return text;
    }
    // As many characters as fit with the ellipsis after them.
    const usize keep = usize(std::max(0.0f, std::floor((width + 0.01f) / per) - 1.0f));
    std::string out;
    usize kept = 0;
    for (usize i = 0; i < text.size() && kept < keep; ++i) {
        out += text[i];
        if (i + 1 >= text.size() || (u8(text[i + 1]) & 0xC0) != 0x80) {
            ++kept;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out + "\xE2\x80\xA6"; // …
}

std::string number(double value) {
    const double a = std::fabs(value);
    std::string text;
    if (a >= 100.0) {
        text = std::format("{}", i64(std::llround(value)));
    } else if (a >= 10.0) {
        text = std::format("{}", std::round(value * 10.0) / 10.0);
    } else {
        text = std::format("{}", std::round(value * 100.0) / 100.0);
    }
    return text;
}

std::string millimetres(double value) {
    return value >= 1000.0 ? number(value / 1000.0) + " m" : number(value) + " mm";
}

std::string grouped(u64 n) {
    std::string digits = std::to_string(n);
    std::string out;
    for (usize i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return out;
}

std::string count(u64 n, std::string_view one, std::string_view many) {
    const std::string plural = many.empty() ? std::string(one) + "s" : std::string(many);
    return grouped(n) + " " + (n == 1 ? std::string(one) : plural);
}

std::string dimensions(const double (&sides)[3], bool compact) {
    if (compact) {
        return number(sides[0]) + "\xC3\x97" + number(sides[1]) + "\xC3\x97" + number(sides[2]);
    }
    return number(sides[0]) + " \xC3\x97 " + number(sides[1]) + " \xC3\x97 " + number(sides[2]) +
           " mm";
}

} // namespace geo::gallery
