#include "gallery/draw.h"

#include <algorithm>
#include <cmath>

namespace geo::gallery {
namespace {

bool hasDigit(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

std::optional<Drawn> drawnOf(const File& file) {
    if (file.failed()) {
        return std::nullopt;
    }
    const std::optional<std::array<double, 2>> size = file.drawnSize();
    if (!size || (*size)[0] <= 0.0 || (*size)[1] <= 0.0) {
        return std::nullopt;
    }
    Drawn d{(*size)[0], (*size)[1]};
    if (file.picture && file.picture->mmPerPixel > 0.0) {
        const Picture& p = *file.picture;
        const double mm = p.mmPerPixel;
        d.below = std::max(0.0, double(p.ink[1] + p.ink[3] - (p.part[1] + p.part[3])) * mm);
        d.above = std::max(0.0, double(p.part[1] - p.ink[1]) * mm);
        d.left = std::max(0.0, double(p.part[0] - p.ink[0]) * mm);
        d.right = std::max(0.0, double(p.ink[0] + p.ink[2] - (p.part[0] + p.part[2])) * mm);
    }
    return d;
}

std::vector<u32> drawable(const Tree& tree, const std::vector<u32>& files) {
    std::vector<u32> out;
    for (const u32 f : files) {
        if (drawnOf(tree.file(f))) {
            out.push_back(f);
        }
    }
    return out;
}

double largest(const Drawn& d) {
    return std::max(d.w, d.h);
}

std::vector<u32> bySize(const Tree& tree, std::vector<u32> files) {
    std::stable_sort(files.begin(), files.end(), [&](u32 a, u32 b) {
        const double sa = tree.file(a).size(), sb = tree.file(b).size();
        if (sa != sb) {
            return sa < sb;
        }
        return compareNatural(tree.file(a).names.label, tree.file(b).names.label) < 0;
    });
    return files;
}

// Whether two names read as one series: both numbers, or the same letters before a number.
static bool alike(std::string_view a, std::string_view b) {
    const auto lead = [](std::string_view s) {
        usize n = 0;
        while (n < s.size() && ((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= 'A' && s[n] <= 'Z'))) {
            ++n;
        }
        return n;
    };
    const bool digitA = !a.empty() && a[0] >= '0' && a[0] <= '9';
    const bool digitB = !b.empty() && b[0] >= '0' && b[0] <= '9';
    if (digitA && digitB) {
        return true;
    }
    if (a.empty() || b.empty() || (a[0] | 0x20) != (b[0] | 0x20)) {
        return false;
    }
    const usize la = lead(a), lb = lead(b);
    const bool seriesA = la > 0 && la < a.size() && a[la] >= '0' && a[la] <= '9';
    const bool seriesB = lb > 0 && lb < b.size() && b[lb] >= '0' && b[lb] <= '9';
    return seriesA == seriesB;
}

// "M1–M36" when the names read as one series, else the span of sizes.
std::string rangeOf(const Tree& tree, const std::vector<u32>& files) {
    std::vector<double> sizes;
    for (const u32 f : files) {
        if (tree.file(f).size() > 0) {
            sizes.push_back(tree.file(f).size());
        }
    }
    if (sizes.empty()) {
        return {};
    }
    std::sort(sizes.begin(), sizes.end());
    const double q3 = sizes[std::min(sizes.size() - 1, sizes.size() * 3 / 4)];
    std::vector<std::string> named;
    std::vector<double> kept;
    for (const u32 f : files) {
        const File& file = tree.file(f);
        if (file.size() <= 0 || file.size() > 3 * q3) {
            continue;
        }
        kept.push_back(file.size());
        if (hasDigit(file.names.label) && !file.names.label.starts_with('#')) {
            named.push_back(file.names.label);
        }
    }
    if (named.size() >= 2) {
        std::sort(named.begin(), named.end(), [](const std::string& a, const std::string& b) {
            return compareNatural(a, b) < 0;
        });
        const std::string& a = named.front();
        const std::string& b = named.back();
        if (a != b && std::all_of(named.begin(), named.end(),
                                  [&](const std::string& n) { return alike(n, a); })) {
            return a + "\xE2\x80\x93" + b;
        }
    }
    std::sort(kept.begin(), kept.end());
    if (kept.empty()) {
        return {};
    }
    return millimetres(kept.front()) + "\xE2\x80\x93" + millimetres(kept.back());
}

std::array<float, 2> Draw::part(u32 file, float x, float bottom, double mmPerPoint, u32 group,
                                bool marks) {
    const File& f = tree.file(file);
    const std::optional<Drawn> d = drawnOf(f);
    const float w = float(d->w / mmPerPoint), h = float(d->h / mmPerPoint);
    out.group(group);
    if (f.picture && f.picture->mmPerPixel > 0.0) {
        const Picture& p = *f.picture;
        const float k = float(p.mmPerPixel / mmPerPoint); // points a pixel
        out.picture(file, x - p.part[0] * k, bottom - (p.part[1] + p.part[3]) * k,
                    float(p.width) * k, float(p.height) * k);
        if (k > kDetailAbove) {
            out.last().flags |= GeoItemDetail; // drawn large: its detail picture, once it comes
        }
        if (f.stale) {
            out.last().flags |= GeoItemStale; // drawn again: faded until the new picture comes
        }
    } else {
        // Not drawn yet: its measured box, in construction lines.
        out.rect(x, bottom - h, w, h, 0, palette.faint, 1.0f, 0.0f, GeoItemDashed);
    }
    lastLine(bottom);
    out.group(0);
    if (marks && f.reading >= 0.0f && !f.picture) {
        // Being read: a mustard bar, filling as it reads, on the box's foot.
        const float barW = std::min(64.0f, std::max(24.0f, w - 8.0f));
        const float bx = x + (w - barW) / 2.0f, by = bottom - 9.0f;
        out.rect(bx, by, barW, 5.0f, hex(0xF7F5EE), hex(0x231E16), 1.5f, 2.5f);
        const float filled = (barW - 3.0f) * f.reading;
        if (filled > 0.5f) {
            out.rect(bx + 1.5f, by + 1.5f, filled, 2.0f, palette.folders[0], 0, 0.0f, 1.0f);
        }
    }
    if (marks && f.changed) {
        // Changed on disk and not seen since: a Δ at the part's top right.
        constexpr TextStyle kDelta{9.5f, 600, 0.0f, false};
        const float cx = x + w - 4.0f, cy = bottom - h - 14.0f;
        out.rect(cx, cy, 16.0f, 14.0f, hex(0xF7F5EE), hex(0x231E16), 1.25f, 7.0f);
        out.text("\xCE\x94", kDelta, hex(0x231E16), cx,
                 cy + (14.0f - lineHeightOf(kDelta)) / 2.0f + ascentOf(kDelta), 16.0f, 1);
    }
    return {w, h};
}

float Draw::scaleBar(double mmPerPoint, float x, float baseline, u32 colour, u32 group,
                     bool small) {
    const double length = scaleBarLength(mmPerPoint);
    const float bar = float(length / mmPerPoint);
    const TextStyle& style = small ? kBarNumberSmall : kBarNumber;
    const std::string label = millimetres(length);
    const float textW = textWidth(label, style);
    const float w = std::max(bar, textW);
    out.group(group);
    out.text(label, style, colour, x, baseline, w, 1);
    out.group(0);
    const float top = baseline + kDescent * style.size + 2.0f;
    const float left = x + (w - bar) / 2.0f;
    out.line(left, top, left, top + 6.0f, colour, 1.5f);
    out.line(left, top + 6.0f, left + bar, top + 6.0f, colour, 1.5f);
    out.line(left + bar, top, left + bar, top + 6.0f, colour, 1.5f);
    return w;
}
float Draw::scaleBarWidth(double mmPerPoint, bool small) {
    const double length = scaleBarLength(mmPerPoint);
    return std::max(float(length / mmPerPoint),
                    textWidth(millimetres(length), small ? kBarNumberSmall : kBarNumber));
}

namespace {

constexpr TextStyle kPosterTitle{44.0f, 600, -0.02f, false};
constexpr TextStyle kPosterSmall{13.0f, 400, 0.0f, false};
constexpr TextStyle kPosterEyebrow{10.5f, 600, 0.16f, true};
constexpr TextStyle kPosterFoot{10.5f, 400, 0.12f, true};
constexpr const char* kStyleNames[] = {"Illustration", "Metal",     "Clay",
                                       "Blueprint",    "Technical", "Studio"};

const char* styleName(u32 style) {
    return kStyleNames[std::min<u32>(style, 5)];
}

} // namespace

float Draw::posterHead(std::string_view summary, std::string_view eyebrow, float width, float left,
                       float top) {
    const float baseline = top + ascentOf(kPosterTitle);
    const u32 g = newGroup();
    out.group(g);
    const std::string title = tree.titleOf(in.folder);
    const float titleW = out.text(title, kPosterTitle, palette.on, left, baseline, width * 0.6f);
    out.last().line = baseline;
    const std::string right = std::string(eyebrow) + " \xC2\xB7 " + styleName(in.style);
    const float rightW = textWidth(right, kPosterEyebrow) + kPosterEyebrow.size;
    const float smallX = left + titleW + 18.0f;
    const float room = left + width - rightW - 18.0f - smallX;
    if (room > 40.0f) {
        out.text(summary, kPosterSmall, palette.muted, smallX, baseline, room);
        out.last().line = baseline;
    }
    out.text(right, kPosterEyebrow, palette.muted, left, baseline, width, 2);
    out.last().line = baseline;
    out.group(0);
    return top + 64.0f;
}

float Draw::posterFoot(float y, float width) {
    out.line(kContentLeft, y, kContentLeft + width, y, palette.on, 1.5f);
    const float baseline = y + 8.0f + ascentOf(kPosterFoot);
    std::string path = tree.title();
    if (const std::string& inside = tree.folder(in.folder).path; !inside.empty()) {
        path += "/" + inside;
    }
    std::string middle = std::string("GeometryView \xC2\xB7 ") + styleName(in.style);
    if (in.style == 0) {
        middle += " \xC2\xB7 one colour per folder";
    }
    const u32 g = newGroup();
    out.group(g);
    const float third = width / 3.0f;
    out.text(path, kPosterFoot, palette.muted, kContentLeft, baseline, third - 12.0f);
    out.last().line = baseline;
    out.text(middle, kPosterFoot, palette.muted, kContentLeft + third, baseline, third, 1);
    out.last().line = baseline;
    out.text(in.posterDate, kPosterFoot, palette.muted, kContentLeft + 2.0f * third + 12.0f,
             baseline, third - 12.0f, 2);
    out.last().line = baseline;
    out.group(0);
    return baseline + kDescent * kPosterFoot.size + 4.0f;
}

} // namespace geo::gallery
