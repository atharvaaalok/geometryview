// Atlas: the folder as a map you zoom into (Docs/design/gallery-spec.md, "Views"). Subfolders are
// regions sized by the square root of how many files they hold, and their subfolders districts
// inside them, three deep. Far out each region shows a few landmark parts; closer in, districts and
// theirs; closest, every part in its cell with its name. The map is laid out in map units and drawn
// through a camera into the view's points; only what shows is drawn, and text keeps its size on
// screen.
#include "gallery/atlas.h"

#include "gallery/draw.h"
#include "gallery/text.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace geo::gallery {
namespace {

constexpr float kMapW = 2400.0f;
constexpr int kMaxDepth = 3;

constexpr TextStyle kRegionName{15.0f, 600, 0.32f, true};
constexpr TextStyle kRegionCount{11.5f, 500, 0.04f, false};
constexpr TextStyle kDistrictName{11.5f, 600, 0.0f, false};
constexpr TextStyle kDistrictCount{10.5f, 500, 0.04f, false};
constexpr TextStyle kCellName{11.0f, 400, 0.0f, false};
constexpr TextStyle kCellSub{9.5f, 400, 0.0f, false};
constexpr TextStyle kCard{10.5f, 400, 0.0f, false};
constexpr TextStyle kCardSmall{7.5f, 600, 0.1f, true};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};

Rect inset(const Rect& r, float d, float top) {
    return {r.x + d, r.y + top, std::max(12.0f, r.w - 2.0f * d), std::max(12.0f, r.h - d - top)};
}

/// A squarified treemap: rectangles for weights inside `rect`, as near square as they come.
std::vector<Rect> squarify(const std::vector<double>& weights, const Rect& rect) {
    double total = 0;
    for (const double w : weights) {
        total += w;
    }
    total = total > 0 ? total : 1;
    struct Item {
        usize i;
        double a;
    };
    std::vector<Item> items;
    for (usize i = 0; i < weights.size(); ++i) {
        items.push_back({i, weights[i] / total * double(rect.w) * double(rect.h)});
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Item& a, const Item& b) { return a.a > b.a; });
    std::vector<Rect> out(weights.size());
    Rect r = rect;
    const auto worst = [](const std::vector<Item>& row, double side) {
        double s = 0, mx = 0, mn = 1e300;
        for (const Item& x : row) {
            s += x.a;
            mx = std::max(mx, x.a);
            mn = std::min(mn, x.a);
        }
        return std::max(side * side * mx / (s * s), s * s / (side * side * std::max(mn, 1e-9)));
    };
    const auto lay = [&](const std::vector<Item>& row) {
        double s = 0;
        for (const Item& x : row) {
            s += x.a;
        }
        if (r.w >= r.h) {
            const float w = float(s / double(r.h));
            float y = r.y;
            for (const Item& x : row) {
                const float h = float(x.a / double(w));
                out[x.i] = {r.x, y, w, h};
                y += h;
            }
            r = {r.x + w, r.y, r.w - w, r.h};
        } else {
            const float h = float(s / double(r.w));
            float x0 = r.x;
            for (const Item& x : row) {
                const float w = float(x.a / double(h));
                out[x.i] = {x0, r.y, w, h};
                x0 += w;
            }
            r = {r.x, r.y + h, r.w, r.h - h};
        }
    };
    std::vector<Item> row;
    for (const Item& it : items) {
        const double side = std::min(r.w, r.h);
        std::vector<Item> with = row;
        with.push_back(it);
        if (row.empty() || worst(with, side) <= worst(row, side)) {
            row = std::move(with);
        } else {
            lay(row);
            row = {it};
        }
    }
    if (!row.empty()) {
        lay(row);
    }
    return out;
}

struct Place {
    std::string name;
    u32 folder = kNone; // kNone for loose files or a size band
    u32 count = 0;
    u32 colour = 0;
    std::vector<Place> children;
    std::vector<u32> files; // a leaf's, every file under it
    Rect rect, inner;
    int depth = 0;
    // A leaf's cells: the grid that draws them largest.
    u32 cols = 1, rows = 1;
};

} // namespace

class Atlas : public Draw {
public:
    Atlas(const LayoutInput& input, Display& display, LayoutResult& result, const AtlasCamera& cam)
        : Draw(input, display, result), cam_(cam) {}

    Place placeOf(u32 folder, int depth) const {
        const Folder& f = tree.folder(folder);
        Place p;
        p.name = tree.titleOf(folder);
        p.folder = folder;
        p.count = f.count;
        p.colour = f.colour;
        p.depth = depth;
        if (depth >= kMaxDepth || f.folders.empty()) {
            p.files = tree.filesUnder(folder);
            return p;
        }
        if (!f.files.empty()) {
            Place loose;
            loose.name = "loose files";
            loose.count = u32(f.files.size());
            loose.colour = f.colour;
            loose.files = f.files;
            loose.depth = depth + 1;
            p.children.push_back(std::move(loose));
        }
        for (const u32 sub : f.folders) {
            if (tree.folder(sub).count > 0) {
                p.children.push_back(placeOf(sub, depth + 1));
            }
        }
        return p;
    }

    std::vector<Place> regionsOf(u32 folder) const {
        const Folder& f = tree.folder(folder);
        std::vector<Place> regions;
        if (f.folders.empty()) {
            Place p;
            p.name = tree.titleOf(folder);
            p.folder = folder;
            p.count = f.count;
            p.colour = f.colour;
            p.files = bySize(tree, f.files);
            regions.push_back(std::move(p));
            return regions;
        }
        for (const u32 sub : f.folders) {
            if (tree.folder(sub).count > 0) {
                regions.push_back(placeOf(sub, 0));
            }
        }
        if (!f.files.empty()) {
            Place loose;
            loose.name = "loose files";
            loose.count = u32(f.files.size());
            loose.colour = f.colour;
            loose.files = bySize(tree, f.files);
            regions.push_back(std::move(loose));
        }
        return regions;
    }

    /// Lays places into the map: regions, districts inside them, and each leaf's grid of cells.
    void lay(Place& p, const Rect& rect) {
        p.rect = rect;
        const float head = p.depth == 0 ? 70.0f : 38.0f;
        if (!p.children.empty()) {
            p.inner = inset(rect, p.depth == 0 ? 20.0f : 12.0f, head);
            if (p.children.size() == 1) {
                lay(p.children[0], p.inner);
                return;
            }
            std::vector<double> weights;
            for (const Place& c : p.children) {
                weights.push_back(std::pow(double(c.count), 0.8));
            }
            const std::vector<Rect> rects = squarify(weights, p.inner);
            for (usize i = 0; i < p.children.size(); ++i) {
                lay(p.children[i], inset(rects[i], 6.0f, 6.0f));
            }
            return;
        }
        p.inner = inset(rect, p.depth == 0 ? 22.0f : 12.0f, head);
        const usize n = std::max<usize>(1, p.files.size());
        float best = -1.0f;
        for (usize cols = 1; cols <= n; ++cols) {
            const usize rows = (n + cols - 1) / cols;
            const float w = p.inner.w / float(cols), h = p.inner.h / float(rows);
            const float s = std::min(w - 8.0f, h - 32.0f);
            if (s > best) {
                best = s;
                p.cols = u32(cols);
                p.rows = u32(rows);
            }
            if (rows == 1) {
                break;
            }
        }
    }

    float mapH() const { return kMapW * in.height / std::max(1.0f, in.width); }

    // The camera: view points from map units.
    float sx(float x) const { return x * cam_.k + cam_.x; }
    float sy(float y) const { return y * cam_.k + cam_.y; }
    Rect screen(const Rect& r) const { return {sx(r.x), sy(r.y), r.w * cam_.k, r.h * cam_.k}; }
    bool shows(const Rect& s) const {
        return s.x + s.w >= 0 && s.y + s.h >= 0 && s.x <= in.width && s.y <= in.height;
    }

    /// Up to `n` typical parts spread across a set's sizes.
    std::vector<u32> landmarks(const std::vector<u32>& files, usize n) const {
        const std::vector<u32> sorted = bySize(tree, drawable(tree, files));
        if (sorted.size() <= n) {
            return sorted;
        }
        std::vector<u32> out;
        for (usize i = 0; i < n; ++i) {
            out.push_back(sorted[(i * (sorted.size() - 1)) / std::max<usize>(1, n - 1)]);
        }
        return out;
    }

    /// Parts standing side by side, at one scale that fits them in `area` (view points).
    void standing(const std::vector<u32>& files, const Rect& area) {
        if (files.empty() || area.w < 12.0f || area.h < 12.0f) {
            return;
        }
        double totalW = 0, maxH = 0;
        for (const u32 f : files) {
            const Drawn d = *drawnOf(tree.file(f));
            totalW += d.w;
            maxH = std::max(maxH, d.h + d.below);
        }
        const float gap = 10.0f;
        const double mm =
            std::max(totalW / std::max(1.0, double(area.w - gap * float(files.size() - 1))),
                     maxH / double(area.h));
        float x = area.x + (area.w - float(totalW / mm) - gap * float(files.size() - 1)) / 2.0f;
        const float bottom = area.y + area.h / 2.0f + float(maxH / mm) / 2.0f;
        const u32 g = newGroup();
        for (const u32 f : files) {
            const Drawn d = *drawnOf(tree.file(f));
            out.target(GeoTargetFile, f);
            out.key(keyOf("part", f));
            part(f, x, bottom - float(d.below / mm), mm, g);
            lastLine(bottom); // its ink on the shared line, whatever its shadow's depth
            result.parts.push_back({GeoTargetFile, f, x, bottom - float((d.h + d.below) / mm),
                                    float(d.w / mm), float((d.h + d.below) / mm)});
            out.target(GeoTargetNone, 0);
            out.key(0);
            x += float(d.w / mm) + gap;
        }
    }

    void drawPlace(const Place& p, int level) {
        const Rect s = screen(p.rect);
        if (!shows(s)) {
            return;
        }
        const bool region = p.depth == 0;
        if (!region && level == 0) {
            return; // districts show from the middle level in
        }
        const usize first = out.items().size();
        out.key(keyOf("place", p.folder == kNone ? u32(p.count) : p.folder));
        if (region) {
            // Hatched ground inside an ink line.
            out.rect(s.x, s.y, s.w, s.h, 0, palette.rule, 1.2f, 28.0f, GeoItemHatched);
            out.rect(s.x, s.y, s.w, s.h, 0, palette.on, 1.5f, 28.0f);
        } else {
            out.rect(s.x, s.y, s.w, s.h, palette.band, palette.faint, 1.0f, 16.0f,
                     p.depth >= 2 ? u32(GeoItemDashed) : 0u);
        }
        // Its name and how many files, on one baseline, in the place's top left.
        const TextStyle& nameStyle = region ? kRegionName : kDistrictName;
        const TextStyle& countStyle = region ? kRegionCount : kDistrictCount;
        const float nx = s.x + (region ? 26.0f : 14.0f);
        const float baseline = s.y + (region ? 18.0f : 10.0f) + ascentOf(nameStyle);
        const float room = s.w - (nx - s.x) - 16.0f;
        if (room > 40.0f) {
            out.target(p.folder == kNone ? GeoTargetNone : GeoTargetFolder, p.folder);
            const u32 g = newGroup();
            out.group(g);
            const float used = out.text(p.name, nameStyle, palette.on, nx, baseline, room - 40.0f);
            out.text(grouped(p.count), countStyle, palette.muted, nx + used + 7.0f, baseline,
                     std::max(0.0f, room - used - 7.0f));
            out.group(0);
            out.target(GeoTargetNone, 0);
        }
        out.key(0);
        (void)first;
        if (p.children.empty()) {
            leaf(p, level);
        } else {
            if (region && level == 0) {
                // Far out: a few landmarks stand for the region.
                standing(landmarks(filesOf(p), 5), inset(s, 26.0f, 54.0f));
            }
            for (const Place& c : p.children) {
                drawPlace(c, level);
            }
        }
    }

    static void collect(const Place& p, std::vector<u32>& out) {
        out.insert(out.end(), p.files.begin(), p.files.end());
        for (const Place& c : p.children) {
            collect(c, out);
        }
    }
    static std::vector<u32> filesOf(const Place& p) {
        std::vector<u32> out;
        collect(p, out);
        return out;
    }

    void leaf(const Place& p, int level) {
        const Rect area = screen(p.inner);
        if (level < 2) {
            // Landmarks, far out; a district's covers in the middle.
            standing(landmarks(p.files, level == 0 ? 5 : 7), area);
            pins(p);
            return;
        }
        // Closest: every part in its cell, fitted to it, its name under it.
        const float cw = area.w / float(p.cols), ch = area.h / float(p.rows);
        const float labRoom = lineHeightOf(kCellName) + lineHeightOf(kCellSub) + 4.0f;
        // Names and subtitles align along each row of the grid.
        std::vector<u32> rowGroups;
        for (u32 r = 0; r < p.rows * 2; ++r) {
            rowGroups.push_back(newGroup());
        }
        for (usize i = 0; i < p.files.size(); ++i) {
            const u32 f = p.files[i];
            const Rect cell{area.x + float(i % p.cols) * cw, area.y + float(i / p.cols) * ch, cw,
                            ch};
            if (!shows(cell)) {
                continue;
            }
            const File& file = tree.file(f);
            const bool selected = in.selected == f;
            const bool hovered = in.hoverKind == GeoTargetFile && in.hover == f;
            const bool faded = in.matches && !in.matches->contains(f);
            const usize first = out.items().size();
            out.target(GeoTargetFile, f);
            out.key(keyOf("part", f));
            if (selected || hovered) {
                out.rect(cell.x + 2.0f, cell.y + 2.0f, cell.w - 4.0f, cell.h - 4.0f,
                         selected ? palette.wash : palette.lift,
                         selected ? palette.washLine : palette.liftLine, 1.5f, 6.0f, GeoItemSticker,
                         selected ? palette.washLine : palette.liftLine);
            }
            const float picH = cell.h - labRoom - 6.0f;
            if (const std::optional<Drawn> d = drawnOf(file); d && picH > 6.0f) {
                const double mm =
                    std::max(d->w / double(cell.w - 8.0f), (d->h + d->below) / double(picH));
                const float w = float(d->w / mm);
                part(f, cell.x + (cell.w - w) / 2.0f, cell.y + 3.0f + picH - float(d->below / mm),
                     mm, 0);
            }
            const float nameBase = cell.y + cell.h - labRoom + ascentOf(kCellName);
            const u32 row = u32(i / p.cols);
            out.group(rowGroups[row * 2]);
            out.text(file.names.label, kCellName, palette.on, cell.x + 2.0f, nameBase,
                     cell.w - 4.0f, 1);
            out.group(rowGroups[row * 2 + 1]);
            if (!file.names.sub.empty()) {
                out.text(file.names.sub, kCellSub, palette.muted, cell.x + 2.0f,
                         nameBase + lineHeightOf(kCellName), cell.w - 4.0f, 1);
            }
            out.group(0);
            if (faded) {
                for (usize j = first; j < out.items().size(); ++j) {
                    out.item(j).flags |= GeoItemFading;
                }
            }
            out.target(GeoTargetNone, 0);
            out.key(0);
            result.parts.push_back({GeoTargetFile, f, cell.x, cell.y, cell.w, cell.h});
        }
    }

    /// A search's finds, as pins where their cells are, when the parts themselves are not drawn.
    void pins(const Place& p) {
        if (!in.matches) {
            return;
        }
        const Rect area = screen(p.inner);
        const float cw = area.w / float(p.cols), ch = area.h / float(p.rows);
        for (usize i = 0; i < p.files.size(); ++i) {
            if (!in.matches->contains(p.files[i])) {
                continue;
            }
            const float cx = area.x + (float(i % p.cols) + 0.5f) * cw;
            const float cy = area.y + (float(i / p.cols) + 0.5f) * ch;
            out.target(GeoTargetFile, p.files[i]);
            out.line(cx, cy, cx, cy - 12.0f, palette.on, 1.5f);
            out.rect(cx - 5.0f, cy - 22.0f, 10.0f, 10.0f, palette.folders[2], palette.on, 1.25f,
                     5.0f);
            out.target(GeoTargetNone, 0);
        }
    }

    /// The map card, in the view's bottom left: the regions small, the view's frame, the level.
    void card(const std::vector<Place>& regions, int level) {
        const float w = 200.0f, mh = w * mapH() / kMapW;
        const float h = mh + 12.0f + 22.0f;
        const float x = 10.0f, y = in.height - h - 10.0f;
        const usize first = out.items().size();
        const u32 ink = hex(0x231E16), muted = hex(0x6F685C), paper = hex(0xF7F5EE),
                  faint = hex(0x9A9284);
        out.target(GeoTargetPeek, 0); // it takes clicks, so nothing beneath is chosen through it
        out.rect(x, y, w + 12.0f, h, paper, ink, 1.5f, 10.0f, GeoItemSticker, ink);
        const float k = w / kMapW, ox = x + 6.0f, oy = y + 6.0f;
        for (const Place& r : regions) {
            out.rect(ox + r.rect.x * k, oy + r.rect.y * k, r.rect.w * k, r.rect.h * k, 0, ink, 1.0f,
                     4.0f);
            for (const Place& c : r.children) {
                out.rect(ox + c.rect.x * k, oy + c.rect.y * k, c.rect.w * k, c.rect.h * k, 0, faint,
                         0.75f, 2.0f);
            }
        }
        // The view's frame on the map.
        const float vx = (0.0f - cam_.x) / cam_.k, vy = (0.0f - cam_.y) / cam_.k;
        const float vw = in.width / cam_.k, vh = in.height / cam_.k;
        const float fx = std::clamp(ox + vx * k, ox, ox + w),
                    fy = std::clamp(oy + vy * k, oy, oy + mh);
        const float fr = std::clamp(ox + (vx + vw) * k, ox, ox + w);
        const float fb = std::clamp(oy + (vy + vh) * k, oy, oy + mh);
        out.rect(fx, fy, std::max(2.0f, fr - fx), std::max(2.0f, fb - fy), hex(0xFFF4D6, 0.45f),
                 ink, 1.5f, 2.0f);
        static const char* levels[] = {"Regions", "Districts", "Parts"};
        const std::string text = std::string(levels[level]) + " \xC2\xB7 " +
                                 grouped(u64(std::lround(cam_.k / cam_.fit * 100.0f))) + "%";
        out.text(text, kCard, muted, x + 8.0f, y + mh + 12.0f + ascentOf(kCard), w - 4.0f);
        out.target(GeoTargetNone, 0);
        for (usize i = first; i < out.items().size(); ++i) {
            out.item(i).flags |= GeoItemFixed;
        }
        (void)kCardSmall;
    }

private:
    const AtlasCamera& cam_;
};

int atlasLevel(const AtlasCamera& cam) {
    return cam.k < cam.fit * 1.55f ? 0 : cam.k < cam.fit * 3.3f ? 1 : 2;
}

AtlasCamera atlasFit(float width, float height) {
    AtlasCamera cam;
    const float mapH = kMapW * height / std::max(1.0f, width);
    cam.fit = std::min(width / kMapW, height / mapH);
    cam.k = cam.fit;
    cam.x = (width - kMapW * cam.k) / 2.0f;
    cam.y = (height - mapH * cam.k) / 2.0f;
    return cam;
}

LayoutResult layoutAtlas(const LayoutInput& input, Display& display, const AtlasCamera& camera) {
    LayoutResult result;
    Atlas atlas(input, display, result, camera);
    const Tree& tree = *input.tree;
    if (tree.folder(input.folder).count == 0) {
        return layoutPlates(input, display); // nothing to draw: as Plates says it
    }
    std::vector<Place> regions = atlas.regionsOf(input.folder);
    std::vector<double> weights;
    for (const Place& r : regions) {
        weights.push_back(std::sqrt(double(r.count)));
    }
    const std::vector<Rect> rects = squarify(weights, {0.0f, 0.0f, kMapW, atlas.mapH()});
    for (usize i = 0; i < regions.size(); ++i) {
        atlas.lay(regions[i], inset(rects[i], 14.0f, 14.0f));
    }
    // A search draws at least the districts, where its pins fall.
    const int level = input.matches ? std::max(1, atlasLevel(camera)) : atlasLevel(camera);
    for (const Place& r : regions) {
        atlas.drawPlace(r, level);
    }
    if (!input.poster) {
        atlas.card(regions, level);
    }
    result.contentHeight = input.height; // the map does not scroll; it pans
    return result;
}

} // namespace geo::gallery
