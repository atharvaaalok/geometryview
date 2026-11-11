#include "gallery/plates.h"

#include "gallery/draw.h"

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
constexpr TextStyle kStripMore{10.0f, 400, 0.0f, false};

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

// A part drawn smaller than this, in points, beside one at least 48 points, gets a detail bubble.
constexpr float kTiny = 6.0f;
constexpr float kBubble = 84.0f; // the bubble's diameter
constexpr float kRing = 18.0f;   // the ring's on the shelf

class Painter : public Draw {
public:
    using Draw::Draw;

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
            // "ONE SCALE" where the line has room for it, as on a phone it may not.
            const float oneW = textWidth("ONE SCALE", kOneScale);
            if (rightStart - 14.0f - oneW - 14.0f >= x) {
                rightStart -= 14.0f + oneW;
                out.group(g);
                out.text("ONE SCALE", kOneScale, palette.muted, rightStart, baseline, oneW);
                out.group(0);
            }
        }
        // What the section holds, cut to the room left, or left out where that is too little to
        // read.
        if (const float room = rightStart - 14.0f - x;
            !meta.empty() && room >= textWidth("000000", kSectionMeta)) {
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
            int detail = 0; // the part again at detail:1, in a bubble beside it; 0 for none
            char letter = 0;
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
        // Tiny beside huge: a part too small to see at the shelf's scale gets a detail bubble, the
        // drawing office's way: a ring and a letter on it, and the part again, larger, at a stated
        // scale, in a circle beside it.
        float tallest = 0.0f;
        for (const Item& it : items) {
            tallest = std::max(tallest, std::max(it.w, it.h));
        }
        char letter = 'A';
        for (Item& it : items) {
            if (failedBoxes || std::max(it.w, it.h) >= kTiny || tallest < 48.0f || letter > 'Z') {
                continue;
            }
            for (const int n : {2, 5, 10, 20, 50, 100, 200, 500}) {
                it.detail = n;
                if (std::max(it.w, it.h) * float(n) >= kBubble * 0.45f) {
                    break;
                }
            }
            it.letter = letter++;
            it.slot = std::max(options.minSlot, kRing + 18.0f + kBubble);
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
                above = std::max(above, it.detail ? kBubble : it.h);
                below = std::max(below, it.below);
            }
            const float baseline = y + above;
            const float labelTop = baseline + below + 4.0f;
            const u32 partGroup = newGroup(), line1 = newGroup(), line2 = newGroup(),
                      line3 = newGroup();
            float cx = kContentLeft;
            for (const Item& it : r) {
                const File& file = tree.file(it.file);
                // A detailed part stands in its ring at the slot's left, its bubble to the right.
                const float left =
                    it.detail ? cx + (kRing - it.w) / 2.0f : cx + (it.slot - it.w) / 2.0f;
                const float centre = it.detail ? cx + it.slot / 2.0f : left + it.w / 2.0f;
                const float boxLeft = it.detail ? cx : left, boxW = it.detail ? it.slot : it.w;
                const float boxTop = baseline - (it.detail ? kBubble : it.h);
                const bool hovered = in.hoverKind == GeoTargetFile && in.hover == it.file;
                const bool selected = in.selected == it.file;
                const bool faded = in.matches && !in.matches->contains(it.file);
                const usize firstItem = out.items().size();
                out.target(GeoTargetFile, it.file);
                out.key(keyOf("part", it.file));
                if (hovered && !selected) {
                    out.offset(-2.0f, -3.0f);
                }
                const float labelBottom = labelTop + options.labelH - 6.0f;
                if (selected || hovered) {
                    out.rect(boxLeft - 10.0f, boxTop - 8.0f, boxW + 20.0f,
                             labelBottom - (boxTop - 8.0f), selected ? palette.wash : palette.lift,
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
                if (it.detail) {
                    // The ring on the shelf, its letter above, a leader to the bubble, and the
                    // part again inside it.
                    constexpr TextStyle kLetter{9.5f, 600, 0.0f, false};
                    const float rcx = cx + kRing / 2.0f, rcy = baseline - it.h / 2.0f;
                    const float ring = std::max(kRing, std::max(it.w, it.h) + 8.0f) / 2.0f;
                    out.rect(rcx - ring, rcy - ring, 2.0f * ring, 2.0f * ring, 0, palette.on, 1.25f,
                             ring);
                    out.text(std::string(1, it.letter), kLetter, palette.on, rcx - 8.0f,
                             rcy - ring - 3.0f, 16.0f, 1);
                    const float bx = cx + it.slot - kBubble, by = baseline - kBubble;
                    out.line(rcx + ring, rcy, bx, by + kBubble / 2.0f, palette.on, 1.0f,
                             GeoItemDashed);
                    out.rect(bx, by, kBubble, kBubble, 0, palette.on, 1.5f, kBubble / 2.0f);
                    const double mm = it.mm / double(it.detail);
                    const float dw = it.w * float(it.detail), dh = it.h * float(it.detail);
                    part(it.file, bx + (kBubble - dw) / 2.0f, by + (kBubble + dh) / 2.0f, mm, 0,
                         /*marks=*/false);
                }
                const u32 text = selected && !palette.dark ? hex(0x231E16) : palette.on;
                const u32 textMuted = selected && !palette.dark ? hex(0x231E16) : palette.muted;
                const float labelW = it.slot + options.gap - 6.0f;
                const float labelX = centre - labelW / 2.0f;
                const bool grouped = !(hovered && !selected);
                // Its name, then what tells it apart, then its size.
                std::string sub = file.failed() ? "couldn't read" : file.names.sub;
                std::string dims;
                if (file.facts && file.facts->hasBox && !failedBoxes) {
                    const double sides[3] = {file.facts->size[0], file.facts->size[1],
                                             file.facts->size[2]};
                    dims = dimensions(sides, true);
                }
                if (it.detail) {
                    dims = std::format("DETAIL {} {}:1 \xC2\xB7 ", it.letter, it.detail) + dims;
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
                if (faded) {
                    for (usize i = firstItem; i < out.items().size(); ++i) {
                        out.item(i).flags |= GeoItemFading;
                    }
                }
                out.offset(0.0f, 0.0f);
                out.target(GeoTargetNone, 0);
                out.key(0);
                result.parts.push_back({GeoTargetFile, it.file, boxLeft, boxTop, boxW,
                                        baseline - boxTop + below + options.labelH});
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
        // A search marks the plates holding what it found, and fades the rest.
        u64 found = 0;
        if (in.matches) {
            for (const u32 file : tree.filesUnder(folder)) {
                found += in.matches->contains(file) ? 1 : 0;
            }
        }
        const bool marked = in.matches && found > 0;
        const bool quiet = in.matches && found == 0;
        const bool lifted = (hovered || focused) && !marked;
        const usize firstItem = out.items().size();
        out.target(GeoTargetFolder, folder);
        out.key(keyOf("plate", folder));
        if (marked) {
            out.rect(x, y, w, h, palette.wash, palette.washLine, 1.5f, 12.0f, GeoItemSticker,
                     palette.washLine);
        } else if (lifted) {
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
                const auto size = part(all[i], sx, stripY + 15.0f, mmTiny, g, /*marks=*/false);
                sx += size[0] + 2.0f;
            }
            out.group(0);
            const std::string more = grouped(f.count) + " in all";
            out.text(more, kStripMore, palette.faint, x + 14.0f, stripY + 11.0f, w - 28.0f, 2);
        }
        if (marked) {
            // How many it found, top right.
            constexpr TextStyle kBadge{10.5f, 500, 0.0f, false};
            const std::string badge = grouped(found) + " match";
            const float bw = textWidth(badge, kBadge) + 14.0f;
            out.rect(x + w - 12.0f - bw, y + 5.5f, bw, 19.0f, hex(0xF7F5EE), hex(0x231E16), 1.5f,
                     9.5f, GeoItemSticker, hex(0x231E16));
            out.text(badge, kBadge, hex(0x231E16), x + w - 12.0f - bw, y + 5.5f + 13.0f, bw, 1);
        }
        if (quiet) {
            for (usize i = firstItem; i < out.items().size(); ++i) {
                out.item(i).flags |= GeoItemFading;
            }
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
        // On a phone's width, two abreast, as the design has it.
        const bool narrow = width < 480.0f && n >= 2;
        for (usize c = narrow ? 2 : 1; c <= (narrow ? 2 : n); ++c) {
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
};

} // namespace

FamilyScale familyScale(const Tree& tree, const std::vector<u32>& files, float w, float h,
                        double fraction, double over, bool shadows) {
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
            need.push_back(std::max(d->w / w, (d->h + (shadows ? d->below : 0.0)) / h));
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
    if (input.poster && folder.count > 0) {
        // Show it off: every folder holding files, as plates on one printed sheet.
        std::vector<u32> leaves;
        for (const u32 f : tree.familiesUnder(input.folder)) {
            if (f != input.folder || folder.folders.empty()) {
                leaves.push_back(f);
            }
        }
        if (leaves.empty()) {
            leaves.push_back(input.folder);
        }
        y = painter.posterHead(count(folder.count, "file") + " in " +
                                   count(leaves.size(), "folder") + ", each at its own scale",
                               "Plate 1", width);
        const float footRoom = 34.0f;
        y = painter.plates(leaves, y, width, std::max(168.0f, input.height - y - footRoom - 12.0f),
                           leftGroup);
        y = painter.posterFoot(y + 12.0f, width);
        result.contentHeight = y + kContentBottom;
        return result;
    }
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
