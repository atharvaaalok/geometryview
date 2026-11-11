// Sheet: every file once, folder by folder, like a contact sheet. Headings per subfolder stay at
// the top of the view while their files scroll under them; the files stand in cells of one size, on
// the cells' baseline, at the scale Sheet's control says. Docs/design/gallery-spec.md, "Views".
#include "gallery/draw.h"
#include "gallery/plates.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace geo::gallery {
namespace {

constexpr TextStyle kHeading{11.5f, 600, 0.16f, true};
constexpr TextStyle kHeadingCount{11.0f, 500, 0.04f, false};
constexpr TextStyle kFamilyTitle{13.5f, 600, 0.0f, false};
constexpr TextStyle kFamilyMeta{11.5f, 400, 0.0f, false};
constexpr TextStyle kStamp{9.0f, 600, 0.08f, true};
constexpr usize kBlockShown = 240; // cells in a block before "show all"
constexpr float kGap = 6.0f, kRowGap = 2.0f;

struct Block {
    std::string title;
    std::vector<u32> files;
    u32 folder = kNone; // where clicking the title goes, if anywhere
    i32 colour = -1;
    std::string key;
};

struct Heading {
    std::string title;
    u64 count = 0;
    u32 folder = kNone;
    std::vector<Block> blocks;
};

// Size bands for a folder of files alone.
struct SizeBand {
    const char* title;
    double lo, hi;
};
constexpr SizeBand kSizeBands[] = {{"Under 20 mm", 0, 20},
                                   {"20 to 100 mm", 20, 100},
                                   {"100 to 500 mm", 100, 500},
                                   {"Over 500 mm", 500, 1e300}};

class Sheet : public Draw {
public:
    using Draw::Draw;

    std::vector<Heading> headingsOf(u32 folder) {
        const Folder& f = tree.folder(folder);
        std::vector<Heading> out;
        if (!f.files.empty() && f.folders.empty()) {
            // A folder of files alone: size bands.
            Heading h{"By size, since this folder has no subfolders", f.files.size(), kNone, {}};
            std::vector<u32> unmeasured = f.files;
            for (const SizeBand& band : kSizeBands) {
                Block b{band.title, {}, kNone, -1, f.path + "|" + band.title};
                for (const u32 file : f.files) {
                    const double s = tree.file(file).size();
                    if (s > 0 && s >= band.lo && s < band.hi) {
                        b.files.push_back(file);
                    }
                }
                if (!b.files.empty()) {
                    h.blocks.push_back(std::move(b));
                }
            }
            std::erase_if(unmeasured, [&](u32 file) { return tree.file(file).size() > 0; });
            if (!unmeasured.empty()) {
                h.blocks.push_back(
                    {"Not measured yet", unmeasured, kNone, -1, f.path + "|reading"});
            }
            out.push_back(std::move(h));
            return out;
        }
        if (!f.files.empty()) {
            out.push_back({"In this folder",
                           f.files.size(),
                           kNone,
                           {{tree.titleOf(folder), f.files, kNone, i32(f.colour), f.path}}});
        }
        for (const u32 sub : f.folders) {
            const Folder& s = tree.folder(sub);
            if (s.count == 0) {
                continue;
            }
            Heading h{tree.titleOf(sub), s.count, sub, {}};
            for (const u32 family : tree.familiesUnder(sub)) {
                const Folder& fam = tree.folder(family);
                std::string title = tree.titleOf(sub);
                if (family != sub) {
                    // Its path below the heading's folder, each name a title.
                    title.clear();
                    std::string_view rest = std::string_view(fam.path).substr(s.path.size() + 1);
                    while (!rest.empty()) {
                        const usize slash = rest.find('/');
                        if (!title.empty()) {
                            title += " / ";
                        }
                        title += folderTitle(rest.substr(0, slash));
                        rest = slash == std::string_view::npos ? std::string_view()
                                                               : rest.substr(slash + 1);
                    }
                }
                h.blocks.push_back({title, fam.files, family, i32(fam.colour), fam.path});
            }
            out.push_back(std::move(h));
        }
        return out;
    }

    float heading(const Heading& h, float y, float width, u32 leftGroup, usize& firstItem) {
        firstItem = out.items().size();
        const float height = 8.0f + lineHeightOf(kHeading) + 8.0f;
        const float baseline = y + 8.0f + ascentOf(kHeading);
        // The band behind it, so what scrolls under it is hidden.
        out.rect(0.0f, y, kContentLeft + width + kContentRight, height, palette.band);
        const u32 g = newGroup();
        out.target(h.folder == kNone ? GeoTargetNone : GeoTargetFolder, h.folder);
        out.group(g);
        const std::string count = grouped(h.count);
        const float countW = textWidth(count, kHeadingCount);
        const float used =
            out.text(h.title, kHeading, palette.on, kContentLeft, baseline, width - countW - 40.0f);
        out.column(leftGroup);
        out.target(GeoTargetNone, 0);
        out.text(count, kHeadingCount, palette.muted, kContentLeft + width - countW, baseline,
                 countW, 2);
        out.group(0);
        const float ruleLeft = kContentLeft + used + 12.0f;
        const float ruleRight = kContentLeft + width - countW - 12.0f;
        if (ruleRight > ruleLeft) {
            out.line(ruleLeft, baseline - 4.0f, ruleRight, baseline - 4.0f, palette.on, 1.5f);
        }
        return y + height;
    }

    float block(const Block& b, float y, float width, u32 leftGroup, u32 columns, float cw,
                float picH, const std::vector<u32>& columnGroups, double trueMm) {
        y += 4.0f;
        const float baseline = y + ascentOf(kFamilyTitle);
        const u32 g = newGroup();
        float x = kContentLeft;
        std::vector<u32> files = b.files;
        if (in.sort == GeoSortSize) {
            files = bySize(tree, files);
        }
        const std::vector<u32> ok = drawable(tree, files);
        const FamilyScale family = familyScale(tree, ok, cw - 14.0f, picH - 8.0f, 1.0, 2.0);
        std::optional<double> mm;
        if (in.sheetScale == GeoScaleFolder) {
            mm = family.mmPerPoint;
        } else if (in.sheetScale == GeoScaleTrue) {
            mm = trueMm;
        }
        // A swatch of the folder's colour, its title, what it holds, and its scale.
        if (b.colour >= 0) {
            out.rect(x, baseline - 9.5f, 10.0f, 10.0f, palette.folders[usize(b.colour) % 4],
                     palette.on, 1.25f, 3.0f);
            out.column(leftGroup);
            x += 10.0f + 12.0f;
        }
        out.target(b.folder == kNone ? GeoTargetNone : GeoTargetFolder, b.folder);
        out.group(g);
        const float titleW = out.text(b.title, kFamilyTitle, palette.on, x, baseline,
                                      width - (x - kContentLeft) - 160.0f);
        if (b.colour < 0) {
            out.column(leftGroup);
        }
        out.target(GeoTargetNone, 0);
        x += titleW + 12.0f;
        std::string meta = count(b.files.size(), "file");
        if (const std::string range = rangeOf(tree, b.files); !range.empty()) {
            meta += " \xC2\xB7 " + range;
        }
        float right = kContentLeft + width;
        if (mm) {
            const float barW = scaleBarWidth(*mm);
            right -= barW;
            scaleBar(*mm, right, baseline, palette.on, g);
            right -= 12.0f;
        }
        out.group(g);
        out.text(meta, kFamilyMeta, palette.muted, x, baseline, std::max(0.0f, right - x));
        out.group(0);
        y = baseline + kDescent * kFamilyTitle.size + (mm ? 10.0f : 0.0f) + 8.0f;

        const bool all = in.expanded && in.expanded->contains(b.key);
        const usize shown = all ? files.size() : std::min(files.size(), kBlockShown);
        const float ch = picH + 32.0f;
        for (usize start = 0; start < shown; start += columns) {
            const u32 partGroup = newGroup(), line1 = newGroup(), line2 = newGroup();
            for (usize c = 0; c < columns && start + c < shown; ++c) {
                const u32 file = files[start + c];
                const float cx = kContentLeft + float(c) * (cw + kGap);
                cell(file, cx, y, cw, picH, ch, mm, partGroup, line1, line2, columnGroups[c]);
            }
            y += ch + kRowGap;
        }
        if (shown < files.size()) {
            out.target(GeoTargetMore, u32(result.bands.size()));
            result.bands.push_back(b.key);
            out.text(grouped(files.size() - shown) + " more \xC2\xB7 show all", kMore, palette.on,
                     kContentLeft, y + 4.0f + ascentOf(kMore), width);
            out.target(GeoTargetNone, 0);
            y += lineHeightOf(kMore) + 16.0f;
        }
        return y + 20.0f;
    }

    void cell(u32 file, float x, float y, float cw, float picH, float ch, std::optional<double> mm,
              u32 partGroup, u32 line1, u32 line2, u32 column) {
        const File& f = tree.file(file);
        const bool hovered = in.hoverKind == GeoTargetFile && in.hover == file;
        const bool selected = in.selected == file;
        const bool found = in.matches && in.matches->contains(file);
        const bool faded = in.matches && !found;
        out.target(GeoTargetFile, file);
        out.key(keyOf("part", file));
        const bool lifted = hovered && !selected && !found;
        if (lifted) {
            out.offset(-1.0f, -2.0f);
        }
        const auto grouped = [&](u32 g) { return lifted ? 0u : g; };
        if (selected || found) {
            out.rect(x, y, cw, ch, palette.wash, palette.washLine, 1.5f, 10.0f, GeoItemSticker,
                     palette.washLine);
        } else if (hovered) {
            out.rect(x, y, cw, ch, palette.lift, palette.liftLine, 1.5f, 10.0f, GeoItemSticker,
                     palette.liftLine);
        } else {
            // An empty cell, at rest: it keeps its column.
            out.rect(x, y, cw, ch, 0);
        }
        if (!lifted) {
            out.column(column);
        }
        const u32 fade = faded ? u32(GeoItemFading) : 0u;
        const usize first = out.items().size();
        const float baseline = y + 4.0f + picH - 4.0f;
        std::string stamp;
        if (const std::optional<Drawn> d = drawnOf(f)) {
            // The scale that fits the cell, no finer than the picture's own pixels allow.
            double fit = std::max(d->w / double(cw - 10.0f), d->h / double(picH - 4.0f));
            if (f.picture && f.picture->mmPerPixel > 0) {
                fit = std::max(fit, f.picture->mmPerPixel / kMaxPointsPerPixel);
            }
            double scale = mm.value_or(fit);
            if (mm && scale < fit) {
                scale = fit;
                stamp = "SMALLER";
            }
            if (in.sheetScale == GeoScaleTrue && mm && stamp.empty() &&
                largest(*d) / scale < 10.0) {
                // Too small to see at true scale: drawn larger, the drawing office's way.
                for (const int n : {2, 5, 10, 20, 50, 100}) {
                    if (largest(*d) / scale * n >= 18.0 || n == 100) {
                        scale = std::max(fit, scale / n);
                        stamp = std::format("DETAIL {}:1", n);
                        break;
                    }
                }
            }
            const float w = float(d->w / scale);
            part(file, x + 4.0f + (cw - 8.0f - w) / 2.0f, baseline, scale, grouped(partGroup));
        } else {
            out.rect(x + cw / 2.0f - 26.0f, y + 4.0f + picH / 2.0f - 22.0f, 52.0f, 40.0f, 0,
                     palette.on, 1.5f, 4.0f, f.failed() ? u32(GeoItemHatched) : u32(GeoItemDashed));
        }
        const u32 text = (selected || found) && !palette.dark ? hex(0x231E16) : palette.on;
        const u32 muted = (selected || found) && !palette.dark ? hex(0x231E16) : palette.muted;
        std::string sub = f.failed() ? "couldn't read" : f.names.sub;
        if (sub.empty() && f.facts && f.facts->hasBox) {
            const double sides[3] = {f.facts->size[0], f.facts->size[1], f.facts->size[2]};
            sub = dimensions(sides);
        }
        out.group(grouped(line1));
        out.text(f.names.label, kLabel, text, x + 6.0f, y + picH + 14.0f, cw - 12.0f, 1);
        out.group(grouped(line2));
        if (!sub.empty()) {
            out.text(sub, kLabelSub, muted, x + 6.0f, y + picH + 26.0f, cw - 12.0f, 1);
        }
        out.group(0);
        if (!stamp.empty()) {
            const float w = textWidth(stamp, kStamp) + 10.0f;
            out.rect(x + cw - 6.0f - w, y + 6.0f, w, 13.0f, hex(0xF7F5EE), hex(0x231E16), 1.25f,
                     6.5f);
            out.text(stamp, kStamp, hex(0x231E16), x + cw - 6.0f - w, y + 6.0f + 9.5f, w, 1);
        }
        for (usize i = first; i < out.items().size(); ++i) {
            out.item(i).flags |= fade;
        }
        out.offset(0.0f, 0.0f);
        out.target(GeoTargetNone, 0);
        out.key(0);
        result.parts.push_back({GeoTargetFile, file, x, y, cw, ch});
    }
};

} // namespace

LayoutResult layoutSheet(const LayoutInput& input, Display& display) {
    LayoutResult result;
    Sheet sheet(input, display, result);
    const Tree& tree = *input.tree;
    const Folder& folder = tree.folder(input.folder);
    const float width = input.width - kContentLeft - kContentRight;
    const float cellW = folder.count > 400 ? std::min(input.sheetCell, 104.0f) : input.sheetCell;
    const u32 columns = std::max(3u, u32(std::floor((width + kGap) / (cellW + kGap))));
    const float cw = (width - kGap * float(columns - 1)) / float(columns);
    const float picH = cw * 0.72f;
    // True scale: one scale for everything, so the 90th percentile of parts fills nine tenths of a
    // cell.
    std::vector<double> sizes;
    for (const u32 f : drawable(tree, tree.filesUnder(input.folder))) {
        sizes.push_back(largest(*drawnOf(tree.file(f))));
    }
    std::sort(sizes.begin(), sizes.end());
    const double trueMm =
        sizes.empty() ? 1.0 : sizes[std::min(sizes.size() - 1, sizes.size() * 9 / 10)] / (cw * 0.9);

    const u32 leftGroup = sheet.newGroup();
    std::vector<u32> columnGroups(columns);
    for (u32& g : columnGroups) {
        g = sheet.newGroup();
    }
    columnGroups[0] = leftGroup;
    const std::vector<Heading> headings = sheet.headingsOf(input.folder);
    float y = kContentTop;
    struct Stuck {
        usize first, last;
        float top, height;
    };
    std::vector<Stuck> stuck;
    for (const Heading& h : headings) {
        usize first = 0;
        const float top = y;
        y = sheet.heading(h, y, width, leftGroup, first);
        stuck.push_back({first, display.items().size(), top, y - top});
        for (const Block& b : h.blocks) {
            y = sheet.block(b, y, width, leftGroup, columns, cw, picH, columnGroups, trueMm);
        }
    }
    // Each heading stays at the top until the next one reaches it.
    for (usize i = 0; i < stuck.size(); ++i) {
        const float next = i + 1 < stuck.size() ? stuck[i + 1].top : y;
        const float until = std::max(0.0f, next - stuck[i].top - stuck[i].height);
        for (usize item = stuck[i].first; item < stuck[i].last; ++item) {
            GeoGalleryItem& it = display.item(item);
            it.sticky_top = stuck[i].top;
            it.sticky_until = until;
        }
    }
    if (headings.empty()) {
        // Nothing to draw: as Plates says it.
        return layoutPlates(input, display);
    }
    result.contentHeight = y + kContentBottom;
    return result;
}

} // namespace geo::gallery
