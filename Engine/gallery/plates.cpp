#include "gallery/plates.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <optional>

namespace geo::gallery {
namespace {

// ---- Type -----------------------------------------------------------------------------------

constexpr TextStyle kEyebrow{10.0f, 600, 0.14f, true};
constexpr TextStyle kTitle{13.5f, 600, 0.0f, false};
constexpr TextStyle kMeta{11.0f, 400, 0.0f, false};
constexpr TextStyle kSectionTitle{10.5f, 600, 0.14f, true};
constexpr TextStyle kSectionMeta{11.5f, 400, 0.0f, false};
constexpr TextStyle kOneScale{10.0f, 600, 0.12f, true};
constexpr TextStyle kBarNumber{10.0f, 600, 0.04f, false};
constexpr TextStyle kBarNumberSmall{9.0f, 600, 0.04f, false};
constexpr TextStyle kLabel{10.5f, 400, 0.0f, false};
constexpr TextStyle kLabelSub{9.5f, 400, 0.0f, false};
constexpr TextStyle kStripMore{10.0f, 400, 0.0f, false};
constexpr TextStyle kMore{11.5f, 400, 0.0f, false};

// Size bands for a folder of many files, each drawn at its own scale.
struct Band {
    const char* title;
    double lo, hi;
};
constexpr Band kBands[] = {{"Under 1 mm", 0, 1},        {"1 to 5 mm", 1, 5},
                           {"5 to 20 mm", 5, 20},       {"20 to 100 mm", 20, 100},
                           {"100 to 500 mm", 100, 500}, {"Over 500 mm", 500, 1e300}};
constexpr usize kFamilyMax = 80; // a folder of more files is shown in size bands
constexpr usize kBandShown = 48; // files in a band before "show all"

// ---- Files ----------------------------------------------------------------------------------

struct Drawn {
    double w = 0, h = 0;                   // the part, in millimetres
    double below = 0;                      // its ink below the part: its shadow
    double left = 0, right = 0, above = 0; // ink past the part on the other sides
};

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

bool hasDigit(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Whether two names read as one series: both numbers, or the same letters before a number.
bool alike(std::string_view a, std::string_view b) {
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

std::string metaOf(const Tree& tree, u32 folder) {
    const Folder& f = tree.folder(folder);
    if (!f.folders.empty()) {
        return count(f.count, "file") + " \xC2\xB7 " + count(f.folders.size(), "folder");
    }
    const std::string range = rangeOf(tree, f.files);
    return count(f.count, "file") + (range.empty() ? "" : " \xC2\xB7 " + range);
}

std::string eyebrowOf(const Tree& tree, u32 folder) {
    std::string out;
    for (const u32 child : tree.folder(folder).folders) {
        if (!out.empty()) {
            out += " \xC2\xB7 ";
        }
        out += folderTitle(tree.folder(child).name);
    }
    return out;
}

// ---- Drawing --------------------------------------------------------------------------------

class Painter {
public:
    Painter(const LayoutInput& input, Display& display, LayoutResult& result)
        : in(input), tree(*input.tree), out(display), result(result),
          palette(paletteOf(input.style)) {}

    u32 newGroup() { return ++result.groups; }

    // A file's part, standing with its bottom at `bottom` and its left edge at `x`, at
    // `mmPerPoint`. Returns the part's size in points.
    std::array<float, 2> part(u32 file, float x, float bottom, double mmPerPoint, u32 group) {
        const File& f = tree.file(file);
        const std::optional<Drawn> d = drawnOf(f);
        const float w = float(d->w / mmPerPoint), h = float(d->h / mmPerPoint);
        out.group(group);
        if (f.picture && f.picture->mmPerPixel > 0.0) {
            const Picture& p = *f.picture;
            const float k = float(p.mmPerPixel / mmPerPoint); // points a pixel
            out.picture(file, x - p.part[0] * k, bottom - (p.part[1] + p.part[3]) * k,
                        float(p.width) * k, float(p.height) * k);
        } else {
            // Not drawn yet: its measured box, in construction lines.
            out.rect(x, bottom - h, w, h, 0, palette.faint, 1.0f, 0.0f, GeoItemDashed);
        }
        lastLine(bottom);
        out.group(0);
        return {w, h};
    }

    void lastLine(float line) { out.last().line = line; }

    // A scale bar: its length above an ink bracket. Returns its width; the number stands on
    // `baseline`, centred in the width.
    float scaleBar(double mmPerPoint, float x, float baseline, u32 colour, u32 group,
                   bool small = false) {
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
    static float scaleBarWidth(double mmPerPoint, bool small = false) {
        const double length = scaleBarLength(mmPerPoint);
        return std::max(float(length / mmPerPoint),
                        textWidth(millimetres(length), small ? kBarNumberSmall : kBarNumber));
    }

    // A section heading: its title and meta on the left, a rule, and "ONE SCALE" with a scale bar
    // on the right when it has a scale. Returns the y below it.
    float section(float y, float width, const std::string& title, const std::string& meta,
                  std::optional<double> mmPerPoint, u32 leftGroup) {
        const float baseline = y + ascentOf(kSectionMeta);
        const u32 g = newGroup();
        float x = kContentLeft;
        out.group(g);
        out.text(title, kSectionTitle, palette.muted, x, baseline, 0.0f);
        out.column(leftGroup); // the title starts the view's left edge
        out.group(0);
        const float titleW = textWidth(setText(title, kSectionTitle, 0), kSectionTitle);
        x += titleW + 14.0f;
        float right = kContentLeft + width;
        float rightStart = right;
        if (mmPerPoint) {
            const float barW = scaleBarWidth(*mmPerPoint);
            rightStart = right - barW;
            scaleBar(*mmPerPoint, rightStart, baseline, palette.on, g);
            const float oneW = textWidth("ONE SCALE", kOneScale);
            rightStart -= 14.0f + oneW;
            out.group(g);
            out.text("ONE SCALE", kOneScale, palette.muted, rightStart, baseline, oneW);
            out.group(0);
        }
        if (!meta.empty()) {
            const float room = rightStart - 14.0f - x - 40.0f;
            out.group(g);
            const float used = out.text(meta, kSectionMeta, palette.muted, x, baseline, room);
            out.group(0);
            x += used + 14.0f;
        }
        if (rightStart - 14.0f > x) {
            out.line(x, baseline - 4.0f, rightStart - 14.0f, baseline - 4.0f, palette.rule, 1.0f);
        }
        const float below = mmPerPoint ? kDescent * kBarNumber.size + 2.0f + 6.0f + 1.5f
                                       : kDescent * kSectionMeta.size;
        return baseline + below + 16.0f;
    }

    struct ShelfOptions {
        float gap = 22.0f;
        float labelH = 44.0f;
        float minSlot = 64.0f;
        float rowGap = 14.0f;
    };

    // Files standing on baselines at one scale each, wrapping into rows. Returns the y below.
    float shelf(const std::vector<u32>& files, float y, float width,
                const std::function<double(u32)>& mmPerPointOf, const ShelfOptions& options,
                bool failedBoxes = false) {
        struct Item {
            u32 file;
            float w, h, below, left, slot;
            double mm;
        };
        std::vector<Item> items;
        for (const u32 f : files) {
            Item it{};
            it.file = f;
            if (failedBoxes) {
                it.w = 52.0f;
                it.h = 40.0f;
            } else {
                const Drawn d = *drawnOf(tree.file(f));
                it.mm = mmPerPointOf(f);
                it.w = float(d.w / it.mm);
                it.h = float(d.h / it.mm);
                it.below = float(d.below / it.mm);
            }
            it.slot = std::max(it.w, options.minSlot);
            items.push_back(it);
        }
        // Every label of a shelf has the same lines, so names, subtitles and sizes share rows.
        const bool withSub = std::any_of(files.begin(), files.end(), [&](u32 f) {
            return !tree.file(f).names.sub.empty() && !tree.file(f).failed();
        });
        std::vector<std::vector<Item>> rows;
        std::vector<Item> row;
        float x = 0.0f;
        for (const Item& it : items) {
            if (!row.empty() && x + it.slot > width) {
                rows.push_back(std::move(row));
                row.clear();
                x = 0.0f;
            }
            row.push_back(it);
            x += it.slot + options.gap;
        }
        if (!row.empty()) {
            rows.push_back(std::move(row));
        }
        for (const std::vector<Item>& r : rows) {
            float above = 0.0f, below = 0.0f;
            for (const Item& it : r) {
                above = std::max(above, it.h);
                below = std::max(below, it.below);
            }
            const float baseline = y + above;
            const float labelTop = baseline + below + 4.0f;
            const u32 partGroup = newGroup(), line1 = newGroup(), line2 = newGroup(),
                      line3 = newGroup();
            float cx = kContentLeft;
            for (const Item& it : r) {
                const File& file = tree.file(it.file);
                const float left = cx + (it.slot - it.w) / 2.0f;
                const bool hovered = in.hoverKind == GeoTargetFile && in.hover == it.file;
                const bool selected = in.selected == it.file;
                out.target(GeoTargetFile, it.file);
                out.key(keyOf("part", it.file));
                if (hovered && !selected) {
                    out.offset(-2.0f, -3.0f);
                }
                const float labelBottom = labelTop + options.labelH - 6.0f;
                if (selected || hovered) {
                    out.rect(left - 10.0f, baseline - it.h - 8.0f, it.w + 20.0f,
                             labelBottom - (baseline - it.h - 8.0f),
                             selected ? palette.wash : palette.lift,
                             selected ? palette.washLine : palette.liftLine, 1.5f, 10.0f,
                             GeoItemSticker, selected ? palette.washLine : palette.liftLine);
                }
                if (failedBoxes) {
                    out.group(partGroup);
                    out.rect(left, baseline - it.h, it.w, it.h, 0, palette.on, 1.5f, 4.0f,
                             GeoItemHatched);
                    out.group(0);
                } else {
                    part(it.file, left, baseline, it.mm, (hovered && !selected) ? 0 : partGroup);
                }
                const u32 text = selected && !palette.dark ? hex(0x231E16) : palette.on;
                const u32 textMuted = selected && !palette.dark ? hex(0x231E16) : palette.muted;
                const float labelW = it.slot + options.gap - 6.0f;
                const float labelX = left + it.w / 2.0f - labelW / 2.0f;
                const bool grouped = !(hovered && !selected);
                // Its name, then what tells it apart, then its size.
                std::string sub = file.failed() ? "couldn't read" : file.names.sub;
                std::string dims;
                if (file.facts && file.facts->hasBox && !failedBoxes) {
                    const double sides[3] = {file.facts->size[0], file.facts->size[1],
                                             file.facts->size[2]};
                    dims = dimensions(sides, true);
                }
                std::vector<std::pair<std::string, bool>> lines = {{file.names.label, false}};
                if (failedBoxes) {
                    lines.push_back({sub, true});
                } else if (withSub) {
                    lines.push_back({sub.empty() ? std::string("\xC2\xA0") : sub, true});
                    lines.push_back({dims, true});
                } else {
                    lines.push_back({dims, true});
                }
                float lineBaseline = labelTop + 11.0f;
                const u32 groupsOfLines[3] = {line1, line2, line3};
                for (usize i = 0; i < lines.size() && i < 3; ++i) {
                    out.group(grouped ? groupsOfLines[i] : 0);
                    if (!lines[i].first.empty()) {
                        out.text(lines[i].first, i == 0 ? kLabel : kLabelSub,
                                 i == 0 ? text : textMuted, labelX, lineBaseline, labelW, 1);
                    }
                    out.group(0);
                    lineBaseline += 12.0f;
                }
                out.offset(0.0f, 0.0f);
                out.target(GeoTargetNone, 0);
                out.key(0);
                result.parts.push_back({GeoTargetFile, it.file, left, baseline - it.h, it.w,
                                        it.h + below + options.labelH});
                cx += it.slot + options.gap;
            }
            y += above + below + options.labelH + options.rowGap;
        }
        return y;
    }

    // ---- Plates ----

    struct Rhythm {
        float top = 14.0f;
        float bottomText = 46.0f;
        float stripTop = 48.0f;
    };

    Rhythm rhythmOf(const std::vector<u32>& folders, float minWidth, bool eyebrows) {
        Rhythm r;
        const bool strip = std::any_of(folders.begin(), folders.end(),
                                       [&](u32 f) { return tree.folder(f).count > 40; });
        const bool bar =
            minWidth > 200.0f && std::any_of(folders.begin(), folders.end(),
                                             [&](u32 f) { return tree.folder(f).folders.empty(); });
        r.stripTop = bar ? 56.0f : 48.0f;
        r.top = eyebrows ? 30.0f : 14.0f;
        r.bottomText = 46.0f + (strip ? r.stripTop - 48.0f + 19.0f : 0.0f);
        return r;
    }

    // The parts a cover draws from: those under the folder, without those far larger than the
    // rest, smallest first.
    std::vector<u32> coverPool(u32 folder) {
        const std::vector<u32> files = drawable(tree, tree.filesUnder(folder));
        const FamilyScale family = familyScale(tree, files, 1, 1, 0.8, 3.0);
        return bySize(tree, family.normal);
    }

    // n parts spread from smallest to largest.
    static std::vector<u32> samples(const std::vector<u32>& sorted, usize n) {
        if (sorted.size() <= n) {
            return sorted;
        }
        std::vector<u32> out;
        for (usize i = 0; i < n; ++i) {
            const u32 pick = sorted[std::min(sorted.size() - 1, i * sorted.size() / n)];
            if (std::find(out.begin(), out.end(), pick) == out.end()) {
                out.push_back(pick);
            }
        }
        return out;
    }

    std::vector<u32> coverOf(u32 folder, usize n) {
        const Folder& f = tree.folder(folder);
        if (f.folders.empty()) {
            return samples(coverPool(folder), n);
        }
        // A folder of folders shows each subfolder's middle sizes side by side, so no one giant
        // part stands for the lot.
        std::vector<std::vector<u32>> groups;
        for (const u32 family : tree.familiesUnder(folder)) {
            const std::vector<u32> files = drawable(tree, tree.folder(family).files);
            if (!files.empty()) {
                groups.push_back(files);
            }
        }
        std::vector<u32> pool;
        for (const auto& g : groups) {
            pool.insert(pool.end(), g.begin(), g.end());
        }
        if (groups.size() <= 1) {
            return samples(bySize(tree, familyScale(tree, pool, 1, 1).normal), n);
        }
        const FamilyScale family = familyScale(tree, pool, 1, 1, 0.8, 3.0);
        std::vector<double> sizes;
        for (const u32 file : family.normal) {
            sizes.push_back(largest(*drawnOf(tree.file(file))));
        }
        std::sort(sizes.begin(), sizes.end());
        const double mid = sizes.empty() ? 1.0 : std::max(sizes[sizes.size() / 2], 1e-9);
        const usize per = std::max<usize>(1, n / groups.size());
        std::vector<u32> picked;
        for (usize gi = 0; gi < groups.size() && gi < n; ++gi) {
            std::vector<u32> near;
            for (const u32 file : groups[gi]) {
                if (std::find(family.normal.begin(), family.normal.end(), file) !=
                    family.normal.end()) {
                    near.push_back(file);
                }
            }
            std::stable_sort(near.begin(), near.end(), [&](u32 a, u32 b) {
                return std::fabs(std::log(std::max(largest(*drawnOf(tree.file(a))), 1e-9) / mid)) <
                       std::fabs(std::log(std::max(largest(*drawnOf(tree.file(b))), 1e-9) / mid));
            });
            for (usize i = 0; i < per && i < near.size(); ++i) {
                picked.push_back(near[i]);
            }
        }
        return bySize(tree, picked);
    }

    // Parts at one scale on one to three baselines in the art box, whichever draws them largest.
    // Returns the scale, in millimetres a point.
    std::optional<double> cover(const std::vector<u32>& parts, float x, float y, float W, float H) {
        if (parts.empty() || W < 20.0f || H < 20.0f) {
            return std::nullopt;
        }
        constexpr float gapX = 8.0f, gapY = 10.0f;
        const auto padded = [&](u32 f) {
            const Drawn d = *drawnOf(tree.file(f));
            const double pad = 0.04 * largest(d);
            return std::array<double, 2>{d.w + pad, d.h + pad};
        };
        struct Best {
            double mm = 0;
            std::vector<std::vector<u32>> rows;
        };
        std::optional<Best> best;
        for (usize r = 1; r <= std::min<usize>(3, parts.size()); ++r) {
            const usize per = (parts.size() + r - 1) / r;
            std::vector<std::vector<u32>> rows;
            for (usize i = 0; i < r; ++i) {
                std::vector<u32> rowFiles;
                for (usize j = i * per; j < std::min(parts.size(), (i + 1) * per); ++j) {
                    rowFiles.push_back(parts[j]);
                }
                if (!rowFiles.empty()) {
                    rows.push_back(rowFiles);
                }
            }
            const double rowH = (H - gapY * float(rows.size() - 1)) / float(rows.size());
            double mm = 0;
            for (const auto& rowFiles : rows) {
                double widths = 0;
                for (const u32 f : rowFiles) {
                    mm = std::max(mm, padded(f)[1] / rowH);
                    widths += padded(f)[0];
                }
                mm = std::max(mm, widths / (W - gapX * float(rowFiles.size() - 1)));
            }
            // Never past 1.25 times a picture's own pixels, or it blurs.
            for (const u32 f : parts) {
                if (const auto& p = tree.file(f).picture; p && p->mmPerPixel > 0) {
                    mm = std::max(mm, p->mmPerPixel / kMaxPointsPerPixel);
                }
            }
            if (!best || mm < best->mm * 0.97) {
                best = Best{mm, rows};
            }
        }
        std::vector<float> used;
        float total = 0;
        for (const auto& rowFiles : best->rows) {
            float h = 0;
            for (const u32 f : rowFiles) {
                h = std::max(h, float(padded(f)[1] / best->mm));
            }
            used.push_back(h);
            total += h;
        }
        total += gapY * float(best->rows.size() - 1);
        float rowY = y + H - total;
        for (usize i = 0; i < best->rows.size(); ++i) {
            const auto& rowFiles = best->rows[i];
            const float base = rowY + used[i];
            float widths = 0;
            for (const u32 f : rowFiles) {
                widths += float(drawnOf(tree.file(f))->w / best->mm);
            }
            widths += gapX * float(rowFiles.size() - 1);
            float px = x + (W - widths) / 2.0f;
            const u32 g = newGroup();
            for (const u32 f : rowFiles) {
                const auto size = part(f, px, base - 3.0f, best->mm, g);
                result.covers.push_back(
                    {GeoTargetFile, f, px, base - 3.0f - size[1], size[0], size[1]});
                px += size[0] + gapX;
            }
            rowY = base + gapY;
        }
        return best->mm;
    }

    struct PlateGroups {
        u32 eyebrow, art, base, title, meta, strip;
    };

    void plate(u32 folder, float x, float y, float w, float h, const Rhythm& rhythm,
               const PlateGroups& groups, bool eyebrows) {
        const Folder& f = tree.folder(folder);
        const bool hovered = in.hoverKind == GeoTargetFolder && in.hover == folder;
        const bool focused = in.focused == folder;
        const bool lifted = hovered || focused;
        out.target(GeoTargetFolder, folder);
        out.key(keyOf("plate", folder));
        if (lifted) {
            out.offset(-1.0f, -2.0f);
            out.rect(x, y, w, h, palette.lift, palette.liftLine, 1.5f, 12.0f, GeoItemSticker,
                     palette.liftLine);
        } else {
            // Crop marks at the corners, at rest.
            const float m = 12.0f, o = -0.75f;
            const u32 c = palette.on;
            out.line(x + o, y + o, x + o + m, y + o, c, 1.5f);
            out.line(x + o, y + o, x + o, y + o + m, c, 1.5f);
            out.line(x + w - o - m, y + o, x + w - o, y + o, c, 1.5f);
            out.line(x + w - o, y + o, x + w - o, y + o + m, c, 1.5f);
            out.line(x + o, y + h - o, x + o + m, y + h - o, c, 1.5f);
            out.line(x + o, y + h - o - m, x + o, y + h - o, c, 1.5f);
            out.line(x + w - o - m, y + h - o, x + w - o, y + h - o, c, 1.5f);
            out.line(x + w - o, y + h - o - m, x + w - o, y + h - o, c, 1.5f);
        }
        const auto inGroup = [&](u32 g) { return lifted ? 0u : g; };
        const std::string eyebrow = eyebrowOf(tree, folder);
        if (eyebrows && !eyebrow.empty()) {
            out.group(inGroup(groups.eyebrow));
            out.text(eyebrow, kEyebrow, palette.muted, x + 14.0f, y + 11.0f + ascentOf(kEyebrow),
                     w - 28.0f);
            out.group(0);
        }
        const float top = rhythm.top;
        const float artW = w - 24.0f;
        const float artH = std::max(30.0f, h - top - rhythm.bottomText - 8.0f);
        const usize n = usize(
            std::max(3.0f, std::min(f.folders.empty() ? 8.0f : 9.0f, std::floor(artW / 38.0f))));
        const std::optional<double> mm = cover(coverOf(folder, n), x + 12.0f, y + top, artW, artH);
        out.group(inGroup(groups.base));
        out.line(x + 14.0f, y + top + artH + 3.0f, x + w - 14.0f, y + top + artH + 3.0f,
                 palette.rule, 1.0f);
        out.group(inGroup(groups.title));
        out.text(tree.titleOf(folder), kTitle, palette.on, x + 14.0f,
                 y + top + artH + 11.0f + ascentOf(kTitle), w - 28.0f);
        const float metaBaseline = y + top + artH + 30.0f + ascentOf(kMeta);
        float metaRoom = w - 28.0f;
        const bool bar = mm && f.folders.empty() && w > 200.0f;
        if (bar) {
            const float barW = scaleBarWidth(*mm, true);
            metaRoom -= barW + 10.0f;
        }
        out.group(inGroup(groups.meta));
        const float metaUsed =
            out.text(metaOf(tree, folder), kMeta, palette.muted, x + 14.0f, metaBaseline, metaRoom);
        if (bar) {
            scaleBar(*mm, x + 14.0f + metaUsed + 10.0f, metaBaseline, palette.muted,
                     inGroup(groups.meta), true);
        }
        out.group(0);
        if (f.count > 40) {
            // A strip of what is inside, and how many in all.
            const float stripY = y + top + artH + rhythm.stripTop;
            const std::vector<u32> all = drawable(tree, tree.filesUnder(folder));
            const usize fit = usize(std::max(4.0f, std::floor((w - 28.0f - 74.0f) / 15.0f)));
            const usize step = std::max<usize>(1, all.size() / fit);
            float sx = x + 14.0f;
            usize shown = 0;
            const u32 g = inGroup(groups.strip);
            for (usize i = 0; i < all.size() && shown < fit; i += step, ++shown) {
                const Drawn d = *drawnOf(tree.file(all[i]));
                const double mmTiny = std::max(d.w / 13.0, d.h / 13.0);
                const auto size = part(all[i], sx, stripY + 15.0f, mmTiny, g);
                sx += size[0] + 2.0f;
            }
            out.group(0);
            const std::string more = grouped(f.count) + " in all";
            out.text(more, kStripMore, palette.faint, x + 14.0f, stripY + 11.0f, w - 28.0f, 2);
        }
        out.offset(0.0f, 0.0f);
        out.target(GeoTargetNone, 0);
        out.key(0);
        result.plates.push_back({GeoTargetFolder, folder, x, y, w, h});
    }

    float plates(const std::vector<u32>& folders, float y, float width, float height,
                 u32 leftGroup) {
        // Columns that draw the plates largest with the fewest empty cells.
        const usize n = folders.size();
        constexpr float gap = 16.0f, aspect = 1.25f, minH = 168.0f;
        usize bestC = 1, bestR = n;
        float bestW = width, bestH = minH, bestScore = -1.0f;
        for (usize c = 1; c <= n; ++c) {
            const usize r = (n + c - 1) / c;
            const float w = (width - gap * float(c - 1)) / float(c);
            const float h = std::max(minH, (height - gap * float(r - 1)) / float(r));
            const bool fits = h * float(r) + gap * float(r - 1) <= height + 1.0f;
            const float score =
                std::min(w / aspect, h) * std::sqrt(float(n) / float(r * c)) * (fits ? 1.0f : 0.6f);
            if (score > bestScore) {
                bestScore = score;
                bestC = c;
                bestR = r;
                bestW = w;
                bestH = h;
            }
        }
        const bool eyebrows = std::any_of(folders.begin(), folders.end(),
                                          [&](u32 f) { return !tree.folder(f).folders.empty(); });
        const Rhythm rhythm = rhythmOf(folders, bestW, eyebrows);
        std::vector<u32> columns(bestC);
        for (u32& g : columns) {
            g = newGroup();
        }
        for (usize row = 0; row < bestR; ++row) {
            // Every plate of a row shares its internal lines.
            const PlateGroups groups{newGroup(), newGroup(), newGroup(),
                                     newGroup(), newGroup(), newGroup()};
            for (usize col = 0; col < bestC; ++col) {
                const usize i = row * bestC + col;
                if (i >= n) {
                    break;
                }
                const float x = kContentLeft + float(col) * (bestW + gap);
                const float py = y + float(row) * (bestH + gap);
                const usize before = out.items().size();
                plate(folders[i], x, py, bestW, bestH, rhythm, groups, eyebrows);
                // Plates align by column, and the first column on the view's left edge.
                const bool lifted = (in.hoverKind == GeoTargetFolder && in.hover == folders[i]) ||
                                    in.focused == folders[i];
                if (!lifted && out.items().size() > before) {
                    GeoGalleryItem& first = out.item(before); // its first crop mark
                    first.column = col == 0 ? leftGroup : columns[col];
                    first.column_line = x; // the plate's own edge, inside its crop marks
                }
            }
        }
        return y + float(bestR) * bestH + gap * float(bestR - 1);
    }

    // A folder's own files: one shelf at one scale, a row of larger ones apart, or size bands for
    // a big mixed folder; then those that could not be read.
    float files(u32 folder, float y, float width, u32 leftGroup) {
        const std::vector<u32>& own = tree.folder(folder).files;
        std::vector<u32> failed, good, waiting;
        for (const u32 f : own) {
            if (tree.file(f).failed()) {
                failed.push_back(f);
            } else if (drawnOf(tree.file(f))) {
                good.push_back(f);
            } else {
                waiting.push_back(f);
            }
        }
        if (good.size() <= kFamilyMax) {
            std::vector<double> sizes;
            for (const u32 f : good) {
                sizes.push_back(largest(*drawnOf(tree.file(f))));
            }
            std::sort(sizes.begin(), sizes.end());
            const double q3 =
                sizes.empty() ? 0.0 : sizes[std::min(sizes.size() - 1, sizes.size() * 3 / 4)];
            std::vector<u32> normal, oversize;
            for (const u32 f : good) {
                (largest(*drawnOf(tree.file(f))) <= 3.0 * q3 ? normal : oversize).push_back(f);
            }
            if (!normal.empty()) {
                FamilyScale family = familyScale(tree, normal, 150.0f, 132.0f, 0.8, 1e9);
                for (const u32 f : normal) {
                    if (const auto& p = tree.file(f).picture; p && p->mmPerPixel > 0) {
                        family.mmPerPoint = std::max(family.mmPerPoint,
                                                     p->mmPerPixel / (kMaxPointsPerPixel * 1.04));
                    }
                }
                const std::string range = rangeOf(tree, good);
                std::string meta = count(good.size(), "file");
                if (!range.empty()) {
                    meta += " \xC2\xB7 " + range;
                }
                meta += " \xC2\xB7 sizes in mm";
                y = section(y, width, "Smallest to largest", meta, family.mmPerPoint, leftGroup);
                const double mm = family.mmPerPoint;
                y = shelf(bySize(tree, normal), y, width, [mm](u32) { return mm; }, {});
            }
            if (!oversize.empty()) {
                const FamilyScale big = familyScale(tree, oversize, 440.0f, 150.0f, 1.0, 1e9);
                const double ratio =
                    normal.empty()
                        ? 1.0
                        : big.mmPerPoint /
                              familyScale(tree, normal, 150.0f, 132.0f, 0.8, 1e9).mmPerPoint;
                y += 18.0f;
                y = section(
                    y, width,
                    std::format("Larger files \xC2\xB7 drawn {}\xC3\x97 smaller", number(ratio)),
                    {}, big.mmPerPoint, leftGroup);
                const double mm = big.mmPerPoint;
                y = shelf(bySize(tree, oversize), y, width, [mm](u32) { return mm; }, {});
            }
        } else {
            // A big mixed folder: size bands, each at its own scale, smallest first.
            for (const Band& band : kBands) {
                std::vector<u32> inBand;
                for (const u32 f : good) {
                    const double s = tree.file(f).size() > 0 ? tree.file(f).size()
                                                             : largest(*drawnOf(tree.file(f)));
                    if (s >= band.lo && s < band.hi) {
                        inBand.push_back(f);
                    }
                }
                if (inBand.empty()) {
                    continue;
                }
                inBand = bySize(tree, inBand);
                const std::string key = tree.folder(folder).path + "|" + band.title;
                const FamilyScale family = familyScale(tree, inBand, 96.0f, 84.0f, 0.9, 1e9);
                y = section(y, width, band.title, count(inBand.size(), "file"), family.mmPerPoint,
                            leftGroup);
                const bool all = in.expanded && in.expanded->contains(key);
                std::vector<u32> shown = inBand;
                if (!all && shown.size() > kBandShown) {
                    shown.resize(kBandShown);
                }
                const Tree& t = tree;
                const double mm = family.mmPerPoint;
                y = shelf(shown, y, width,
                          [&t, mm](u32 f) {
                              const Drawn d = *drawnOf(t.file(f));
                              // At most 190 × 150 points a part.
                              return std::max(mm, std::max(d.w / 190.0, d.h / 150.0));
                          },
                          {16.0f, 32.0f, 56.0f, 14.0f});
                if (shown.size() < inBand.size()) {
                    out.target(GeoTargetMore, u32(result.bands.size()));
                    result.bands.push_back(key);
                    out.text(grouped(inBand.size() - shown.size()) +
                                 " more in this band \xC2\xB7 show all",
                             kMore, palette.on, kContentLeft, y + ascentOf(kMore), width);
                    out.target(GeoTargetNone, 0);
                    y += lineHeightOf(kMore) + 12.0f;
                }
                y += 26.0f;
            }
        }
        if (!waiting.empty()) {
            y += 18.0f;
            y = section(y, width, "Reading", count(waiting.size(), "file"), std::nullopt,
                        leftGroup);
            for (usize i = 0; i < waiting.size(); ++i) {
                const File& file = tree.file(waiting[i]);
                out.target(GeoTargetFile, waiting[i]);
                out.text(file.names.label, kLabel, palette.muted, kContentLeft,
                         y + ascentOf(kLabel), width);
                out.target(GeoTargetNone, 0);
                y += 16.0f;
                if (i == 47 && waiting.size() > 48) {
                    out.text(grouped(waiting.size() - 48) + " more", kLabel, palette.muted,
                             kContentLeft, y + ascentOf(kLabel), width);
                    y += 16.0f;
                    break;
                }
            }
        }
        if (!failed.empty()) {
            y += 18.0f;
            y = section(y, width, "Couldn't read", count(failed.size(), "file"), std::nullopt,
                        leftGroup);
            y = shelf(failed, y, width, [](u32) { return 1.0; }, {}, true);
        }
        return y;
    }

    // A folder with nothing to draw says so, in the Library's own words.
    float empty(u32 folder, float y, float width, float height) {
        const float h = std::max(160.0f, height * 0.5f);
        out.rect(kContentLeft + 22.0f, y + 22.0f, width - 44.0f, h - 44.0f, 0, palette.rule, 1.5f,
                 10.0f, GeoItemDashed);
        constexpr TextStyle kHeading{12.5f, 600, 0.06f, true};
        const float middle = y + h / 2.0f;
        out.text(tree.titleOf(folder) + " \xC2\xB7 0 files", kHeading, palette.on, kContentLeft,
                 middle - 2.0f, width, 1);
        out.text("No STEP or STL files here.", kSectionMeta, palette.muted, kContentLeft,
                 middle + 16.0f, width, 1);
        return y + h;
    }

    const LayoutInput& in;
    const Tree& tree;
    Display& out;
    LayoutResult& result;
    Palette palette;
};

} // namespace

FamilyScale familyScale(const Tree& tree, const std::vector<u32>& files, float w, float h,
                        double fraction, double over) {
    FamilyScale out;
    std::vector<double> sizes;
    for (const u32 f : files) {
        if (const auto d = drawnOf(tree.file(f))) {
            sizes.push_back(largest(*d));
        }
    }
    if (sizes.empty()) {
        out.normal = files;
        return out;
    }
    std::vector<double> sorted = sizes;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2];
    std::vector<double> need;
    for (const u32 f : files) {
        const auto d = drawnOf(tree.file(f));
        if (d && largest(*d) > over * median) {
            out.oversize.push_back(f);
            continue;
        }
        out.normal.push_back(f);
        if (d) {
            need.push_back(std::max(d->w / w, d->h / h));
        }
    }
    std::sort(need.begin(), need.end());
    if (!need.empty()) {
        const usize at = std::min(need.size() - 1, usize(double(need.size()) * fraction));
        out.mmPerPoint = need[at] > 0 ? need[at] : 1.0;
    }
    return out;
}

double scaleBarLength(double mmPerPoint) {
    for (int e = -3; e < 7; ++e) {
        for (const double s : {1.0, 2.0, 5.0}) {
            const double v = s * std::pow(10.0, e);
            if (v / mmPerPoint >= 40.0) {
                return v;
            }
        }
    }
    return 1e7;
}

LayoutResult layoutPlates(const LayoutInput& input, Display& display) {
    LayoutResult result;
    Painter painter(input, display, result);
    const Tree& tree = *input.tree;
    const Folder& folder = tree.folder(input.folder);
    const float width = input.width - kContentLeft - kContentRight;
    float y = kContentTop;
    const u32 leftGroup = painter.newGroup();
    // Folders with nothing to draw get no plate; entered, they say so.
    std::vector<u32> plated;
    for (const u32 f : folder.folders) {
        if (tree.folder(f).count > 0) {
            plated.push_back(f);
        }
    }
    if (!plated.empty()) {
        const float gridHeight =
            folder.files.empty() ? input.height - 34.0f : std::min(input.height * 0.48f, 360.0f);
        y = painter.plates(plated, y, width, gridHeight, leftGroup);
        if (!folder.files.empty()) {
            y += 26.0f;
            y = painter.files(input.folder, y, width, leftGroup);
        }
    } else if (!folder.files.empty()) {
        y = painter.files(input.folder, y, width, leftGroup);
    } else {
        y = painter.empty(input.folder, y, width, input.height);
    }
    result.contentHeight = y + kContentBottom;
    return result;
}

} // namespace geo::gallery
