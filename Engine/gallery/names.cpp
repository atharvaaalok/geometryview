#include "gallery/names.h"

#include "base/types.h"

#include <algorithm>
#include <array>
#include <vector>

namespace geo::gallery {
namespace {

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}
bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
char lower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}
char upper(char c) {
    return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = lower(c);
    }
    return out;
}

// Words split at underscores, spaces, dashes and dots.
std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> out;
    usize start = 0;
    for (usize i = 0; i <= text.size(); ++i) {
        const bool cut = i == text.size() || text[i] == '_' || text[i] == ' ' || text[i] == '-' ||
                         text[i] == '.';
        if (cut) {
            if (i > start) {
                out.push_back(text.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return out;
}

// A word that tells nothing about the part: a long number, a hash, or a file-format word.
bool isIdWord(std::string_view word) {
    static constexpr std::array<std::string_view, 9> kNoise = {
        "step", "stp", "stl", "part", "model", "file", "output", "export", "copy"};
    const std::string w = lowered(word);
    if (std::find(kNoise.begin(), kNoise.end(), w) != kNoise.end()) {
        return true;
    }
    const usize digits = usize(std::count_if(w.begin(), w.end(), isDigit));
    if (digits == w.size() && w.size() >= 3) {
        return true; // 00840788, 000
    }
    const bool hex = std::all_of(w.begin(), w.end(),
                                 [](char c) { return isDigit(c) || (c >= 'a' && c <= 'f'); });
    return hex && w.size() >= 12 && digits >= 4; // 8bd96a8cce47d02d56a633b9
}

constexpr std::array<std::string_view, 7> kCodes = {"iso",  "din", "en", "ansi",
                                                    "asme", "jis", "gb"};

} // namespace

std::string folderTitle(std::string_view name) {
    std::string out;
    for (std::string_view word : words(name)) {
        if (!out.empty()) {
            out += ' ';
        }
        const std::string w = lowered(word);
        // ISO 4032: a standard's code and its number.
        bool coded = false;
        for (std::string_view code : kCodes) {
            if (w.size() > code.size() && w.starts_with(code) && isDigit(w[code.size()])) {
                for (const char c : code) {
                    out += upper(c);
                }
                out += ' ';
                out += word.substr(code.size());
                coded = true;
                break;
            }
        }
        if (coded) {
            continue;
        }
        const bool letters = std::any_of(w.begin(), w.end(), isAlpha);
        const bool vowels = std::any_of(w.begin(), w.end(), [](char c) {
            return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' || c == 'y';
        });
        if (letters && !vowels) {
            for (const char c : word) {
                out += upper(c);
            }
        } else {
            out += upper(word[0]);
            out += word.substr(1);
        }
    }
    return out;
}

std::string stemOf(std::string_view fileName) {
    std::string_view stem = fileName;
    if (const usize slash = stem.rfind('/'); slash != std::string_view::npos) {
        stem.remove_prefix(slash + 1);
    }
    if (const usize dot = stem.rfind('.'); dot != std::string_view::npos && dot > 0) {
        stem = stem.substr(0, dot);
    }
    // A zero-padded ordering prefix: "01_", "007 - ", "00."; the rest must still name something.
    if (stem.size() > 2 && stem[0] == '0' && isDigit(stem[1])) {
        usize at = 2;
        while (at < stem.size() && isDigit(stem[at])) {
            ++at;
        }
        usize rest = at;
        while (rest < stem.size() &&
               (stem[rest] == '_' || stem[rest] == ' ' || stem[rest] == '-' || stem[rest] == '.')) {
            ++rest;
        }
        if (rest > at && rest < stem.size()) {
            stem.remove_prefix(rest);
        }
    }
    return std::string(stem);
}

FileNames fileNames(std::string_view fileName, std::string_view product) {
    const std::string stem = stemOf(fileName);
    // Whether it is only an id is read before any ordering prefix is dropped: in an id, leading
    // zeros are part of it.
    std::string_view raw = fileName;
    if (const usize slash = raw.rfind('/'); slash != std::string_view::npos) {
        raw.remove_prefix(slash + 1);
    }
    if (const usize dot = raw.rfind('.'); dot != std::string_view::npos && dot > 0) {
        raw = raw.substr(0, dot);
    }
    const std::vector<std::string_view> parts = words(raw);
    const bool idOnly = !parts.empty() && std::all_of(parts.begin(), parts.end(), isIdWord);
    if (!idOnly) {
        return {stem, {}, true};
    }
    // The id: the first word with a digit in it, kept short.
    std::string id;
    for (std::string_view word : parts) {
        if (std::any_of(word.begin(), word.end(), isDigit)) {
            id = std::string(word.substr(0, 8));
            break;
        }
    }
    if (id.empty()) {
        id = std::string(parts.front().substr(0, 8));
    }
    const std::vector<std::string_view> productWords = words(product);
    const bool productNamed =
        !productWords.empty() && !std::all_of(productWords.begin(), productWords.end(), isIdWord);
    if (productNamed) {
        return {std::string(product), "#" + id, true};
    }
    return {"#" + id, "unnamed", false};
}

int compareNatural(std::string_view a, std::string_view b) {
    usize i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isDigit(a[i]) && isDigit(b[j])) {
            usize ei = i, ej = j;
            while (ei < a.size() && isDigit(a[ei])) {
                ++ei;
            }
            while (ej < b.size() && isDigit(b[ej])) {
                ++ej;
            }
            // Compare as numbers: skip leading zeros, then the longer run is larger.
            usize zi = i, zj = j;
            while (zi + 1 < ei && a[zi] == '0') {
                ++zi;
            }
            while (zj + 1 < ej && b[zj] == '0') {
                ++zj;
            }
            if (ei - zi != ej - zj) {
                return ei - zi < ej - zj ? -1 : 1;
            }
            const int c = a.substr(zi, ei - zi).compare(b.substr(zj, ej - zj));
            if (c != 0) {
                return c < 0 ? -1 : 1;
            }
            i = ei;
            j = ej;
            continue;
        }
        const char ca = lower(a[i]), cb = lower(b[j]);
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < a.size()) {
        return 1;
    }
    if (j < b.size()) {
        return -1;
    }
    return a.compare(b) < 0 ? -1 : a.compare(b) > 0 ? 1 : 0;
}

} // namespace geo::gallery
