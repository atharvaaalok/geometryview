// Ladders: every folder that holds files is a strip of them, smallest to largest, standing on a
// ruler drawn at the strip's own scale (Docs/design/gallery-spec.md, "Views"). Each strip's parts
// are in a lane of their own, which scrolls sideways; a flat folder of many very different sizes
// falls into size bands, whose strips wrap. Hover shows calipers with the measured box; with a part
// chosen, the one hovered shows its difference.
#include "gallery/draw.h"
#include "gallery/names.h"
#include "gallery/plates.h"
#include "gallery/text.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace geo::gallery {
namespace {

// The board's geometry, in points.
constexpr float kHeadW = 200.0f; // the strip's head column
constexpr float kPadTop = 30.0f; // above the tallest part
constexpr float kDrawH = 84.0f;  // the parts' height
constexpr float kRulerH = 22.0f; // the ruler and its numbers
constexpr float kLabH = 28.0f;   // the labels under it
constexpr float kPadB = 10.0f;   // below them
constexpr float kRowH = kPadTop + kDrawH + kRulerH + kLabH + kPadB;
constexpr float kGap = 16.0f;    // between parts
constexpr float kMargin = 18.0f; // at the ends of a strip
constexpr float kOverW = 300.0f; // the widest a part is drawn at its strip's scale
constexpr float kMinStrip = 150.0f;

constexpr TextStyle kHeading{11.5f, 600, 0.16f, true};
constexpr TextStyle kHeadingCount{11.0f, 500, 0.04f, false};
constexpr TextStyle kEyebrow{10.0f, 600, 0.14f, true};
constexpr TextStyle kTitle{13.5f, 600, 0.0f, false};
constexpr TextStyle kMeta{11.5f, 400, 0.0f, false};
constexpr TextStyle kStamp{10.0f, 600, 0.04f, false};
constexpr TextStyle kTick{8.5f, 400, 0.0f, false};
constexpr TextStyle kPartLabel{10.0f, 400, 0.0f, false};
constexpr TextStyle kPartSub{8.5f, 400, 0.0f, false};
constexpr TextStyle kNote{9.5f, 400, 0.0f, false};
constexpr TextStyle kCaliper{10.0f, 600, 0.0f, false};
constexpr TextStyle kDelta{11.0f, 600, 0.0f, false};
constexpr TextStyle kDeltaSmall{10.0f, 400, 0.0f, false};

// One point at 1:1, in millimetres: a CSS pixel, as the board measures.
constexpr double kPointMm = 25.4 / 96.0;

/// A drafting scale: millimetres a point, and how it reads ("1:2.5", "2:1").
struct Scale {
    double mm = 1.0;
    std::string label = "1:1";
};

/// The drafting scale at or below `mm` millimetres a point: enlargements 80:1 … 1.5:1, 1:1, then
/// reductions 1:1.5 … 1:800000, each a round number of 1, 1.5, 2, 2.5, 3, 4, 5, 6 or 8 × 10ⁿ.
Scale snap(double mm) {
    constexpr double nice[] = {1, 1.5, 2, 2.5, 3, 4, 5, 6, 8};
    const double want = kPointMm / std::max(mm, 1e-9); // the ratio that would fit exactly
    struct Ratio {
        double r;
        std::string label;
    };
    std::vector<Ratio> ratios;
    for (const int e : {1, 0}) {
        for (int i = 8; i >= 0; --i) {
            const double r = nice[i] * std::pow(10.0, double(e));
            ratios.push_back({r, number(r) + ":1"});
        }
    }
    for (int e = 0; e <= 5; ++e) {
        for (const double n : nice) {
            const double d = n * std::pow(10.0, double(e));
            if (d > 1.0) {
                // 1:2.5 and 1:40 as written, 1:1000 without a separator, as drawings write them.
                ratios.push_back(
                    {1.0 / d, "1:" + (d < 10.0 ? number(d) : std::format("{}", u64(d)))});
            }
        }
    }
    for (const Ratio& r : ratios) {
        if (r.r <= want * 1.0001) {
            return {kPointMm / r.r, r.label};
        }
    }
    return {kPointMm / ratios.back().r, ratios.back().label};
}

std::string signedMm(double v) {
    if (std::fabs(v) < 0.005) {
        return "\xC2\xB1"
               "0";
    }
    return (v > 0 ? "+" : "\xE2\x88\x92") + number(std::fabs(v));
}

struct Strip {
    std::string title, eyebrow;
    u32 folder = kNone; // entered from its title, or kNone for a size band
    bool wrap = false;  // a size band: its rows wrap, it does not scroll
    std::vector<u32> files;
};

struct Group {
    std::string heading; // empty: no heading
    u32 folder = kNone;
    u32 count = 0;
    std::vector<Strip> strips;
};

class Ladders : public Draw {
public:
    Ladders(const LayoutInput& input, Display& display, LayoutResult& result)
        : Draw(input, display, result) {}

    std::vector<Group> groupsOf(u32 folderIndex) const {
        const Folder& folder = tree.folder(folderIndex);
        std::vector<Group> out;
        if (folder.folders.empty()) {
            if (wantsBands(folder.files)) {
                Group g;
                g.heading = "By size, since this folder has no subfolders";
                g.count = u32(folder.files.size());
                static const struct {
                    double lo, hi;
                    const char* title;
                } bands[] = {{0, 10, "Under 10 mm"},       {10, 30, "10 to 30 mm"},
                             {30, 100, "30 to 100 mm"},    {100, 300, "100 to 300 mm"},
                             {300, 1000, "300 mm to 1 m"}, {1000, 1e300, "1 m and over"}};
                for (const auto& band : bands) {
                    Strip s;
                    s.title = band.title;
                    s.eyebrow = "size band";
                    s.wrap = true;
                    for (const u32 f : folder.files) {
                        const double size = tree.file(f).size();
                        if (size >= band.lo && size < band.hi) {
                            s.files.push_back(f);
                        }
                    }
                    if (!s.files.empty()) {
                        g.strips.push_back(std::move(s));
                    }
                }
                out.push_back(std::move(g));
                return out;
            }
            Group g;
            Strip s;
            s.title = tree.titleOf(folderIndex);
            s.folder = kNone;
            s.files = folder.files;
            g.strips.push_back(std::move(s));
            out.push_back(std::move(g));
            return out;
        }
        if (!folder.files.empty()) {
            Group g;
            g.heading = "In this folder";
            g.count = u32(folder.files.size());
            Strip s;
            s.title = tree.titleOf(folderIndex);
            s.files = folder.files;
            g.strips.push_back(std::move(s));
            out.push_back(std::move(g));
        }
        for (const u32 sub : folder.folders) {
            if (tree.folder(sub).count == 0) {
                continue;
            }
            Group g;
            g.heading = tree.folder(sub).name;
            g.folder = sub;
            g.count = tree.folder(sub).count;
            const std::string& base = tree.folder(sub).path;
            for (const u32 family : tree.familiesUnder(sub)) {
                Strip s;
                s.folder = family;
                s.files = tree.folder(family).files;
                if (family == sub) {
                    s.title = tree.titleOf(sub);
                } else {
                    // Its path below the heading's folder, its last name the title, the rest above.
                    const std::string inner = tree.folder(family).path.substr(base.size() + 1);
                    const usize slash = inner.rfind('/');
                    s.title =
                        folderTitle(slash == std::string::npos ? inner : inner.substr(slash + 1));
                    if (slash != std::string::npos) {
                        s.eyebrow = inner.substr(0, slash);
                    }
                }
                g.strips.push_back(std::move(s));
            }
            out.push_back(std::move(g));
        }
        return out;
    }

    /// A flat folder of many parts of very different sizes reads better as size bands.
    bool wantsBands(const std::vector<u32>& files) const {
        std::vector<double> sizes;
        for (const u32 f : files) {
            if (const double s = tree.file(f).size(); s > 0) {
                sizes.push_back(s);
            }
        }
        if (sizes.size() < 40) {
            return false;
        }
        std::sort(sizes.begin(), sizes.end());
        return sizes[sizes.size() * 9 / 10] / std::max(1e-6, sizes[sizes.size() / 10]) > 12.0;
    }

    /// The scale every strip shares, with Scale: Shared.
    Scale sharedScale(const std::vector<Group>& groups) const {
        double mm = 0;
        for (const Group& g : groups) {
            for (const Strip& s : g.strips) {
                mm = std::max(mm, fitMm(s.files, kOverW * 1.4f, kDrawH));
            }
        }
        return snap(mm > 0 ? mm : 1.0);
    }

    /// Millimetres a point at which every part and its shadow fit `w` × `h`.
    double fitMm(const std::vector<u32>& files, float w, float h) const {
        double mm = 0;
        for (const u32 f : files) {
            if (const std::optional<Drawn> d = drawnOf(tree.file(f))) {
                mm = std::max({mm, d->w / double(w), (d->h + d->below) / double(h)});
            }
        }
        return mm;
    }

    float heading(const Group& g, float y, float width, usize& firstItem) {
        firstItem = out.items().size();
        constexpr float height = 34.0f;
        const float baseline = y + (height - lineHeightOf(kHeading)) / 2.0f + ascentOf(kHeading);
        out.rect(edgeL_, y, edgeR_ - edgeL_, height, palette.band);
        const u32 group = newGroup();
        out.target(g.folder == kNone ? GeoTargetNone : GeoTargetFolder, g.folder);
        out.group(group);
        const std::string count = grouped(g.count);
        const float countW = textWidth(count, kHeadingCount);
        const float used = out.text(g.heading, kHeading, palette.on, left_ + 4.0f, baseline,
                                    width - countW - 40.0f);
        out.target(GeoTargetNone, 0);
        out.text(count, kHeadingCount, palette.muted, left_ + width - countW - 4.0f, baseline,
                 countW, 2);
        out.group(0);
        const float ruleLeft = left_ + 4.0f + used + 12.0f;
        const float ruleRight = left_ + width - countW - 16.0f;
        if (ruleRight > ruleLeft) {
            out.line(ruleLeft, y + height / 2.0f, ruleRight, y + height / 2.0f, palette.on, 1.5f);
        }
        return y + height;
    }

    /// A ruler under a segment of a strip: its line on `baseline`, ticks at the scale, and numbers.
    void ruler(float x, float baseline, float length, double mm) {
        constexpr double steps[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1,    2,    5,
                                    10,   20,   50,   100, 200, 500, 1000, 2000, 5000,
                                    1e4,  2e4,  5e4,  1e5, 2e5, 5e5, 1e6};
        double minor = steps[24];
        for (const double s : steps) {
            if (s / mm >= 5.0) {
                minor = s;
                break;
            }
        }
        const int lead =
            int(std::lround(minor / std::pow(10.0, std::floor(std::log10(minor) + 1e-9))));
        const double major = minor * (lead == 5 ? 2.0 : 5.0);
        double every = major;
        for (const int m : {1, 2, 5, 10, 20, 50, 100}) {
            every = major * m;
            if (every / mm >= 90.0) {
                break;
            }
        }
        // The line and its ticks, one item the painters fill: minor pitch in `radius`, major in
        // `tracking`.
        out.rect(x, baseline - 0.75f, length, 8.0f, 0, palette.on, 1.5f, float(minor / mm),
                 GeoItemRuler);
        out.last().tracking = float(major / mm);
        out.last().line = baseline;
        const bool metres = every >= 1000.0;
        const auto value = [&](double v) { return metres ? number(v / 1000.0) : number(v); };
        const usize n = usize(std::floor(double(length) * mm / every + 1e-6));
        for (usize i = 0; i <= n; ++i) {
            const double v = double(i) * every;
            const float tx = x + float(v / mm);
            if (tx > x + length - 20.0f) {
                break;
            }
            const std::string text = i == 1 ? value(v) + (metres ? " m" : " mm") : value(v);
            const float w = textWidth(text, kTick);
            out.text(text, kTick, palette.faint, i == 0 ? tx : tx - w / 2.0f, baseline + 18.0f, w);
        }
    }

    /// A strip at `y`: its head, and its parts on their rulers, in a lane of its own unless it
    /// wraps. Returns the y below it.
    float strip(const Strip& s, float y, float width, const std::optional<Scale>& shared,
                u32 leftGroup, u32 headGroups[5]) {
        const float trackX = left_ + kHeadW;
        const float trackW = width - kHeadW;
        // The parts, smallest to largest or by name.
        std::vector<u32> files = s.files;
        if (in.sort == GeoSortSize) {
            files = bySize(tree, files);
        }
        std::vector<u32> drawn, unmeasured;
        for (const u32 f : files) {
            (drawnOf(tree.file(f)) ? drawn : unmeasured).push_back(f);
        }
        // The strip's scale fits most of it; the few that would need it far smaller go after a
        // divider, at a scale of their own.
        // A size band wraps, and so does every strip on a poster, where nothing scrolls.
        const bool wraps = s.wrap || in.poster;
        const float maxW = s.wrap ? 240.0f : kOverW;
        std::vector<std::pair<double, u32>> needs;
        for (const u32 f : drawn) {
            const Drawn d = *drawnOf(tree.file(f));
            needs.push_back({std::max(d.w / double(maxW), (d.h + d.below) / double(kDrawH)), f});
        }
        std::sort(needs.begin(), needs.end());
        double own = needs.empty() ? 1.0 : needs.back().first;
        std::vector<u32> big;
        if (shared && in.poster) {
            // On a poster's one scale, a part too big for its row is drawn smaller at the end.
            for (const u32 f : drawn) {
                const Drawn d = *drawnOf(tree.file(f));
                if (d.w / shared->mm > 520.0 || (d.h + d.below) / shared->mm > double(kDrawH)) {
                    big.push_back(f);
                }
            }
        } else if (!shared && needs.size() >= 8) {
            const double p85 = needs[(needs.size() - 1) * 85 / 100].first;
            if (own > p85 * 2.0) {
                own = p85;
                for (const auto& [n, f] : needs) {
                    if (n > p85 * 1.0001) {
                        big.push_back(f);
                    }
                }
            }
        }
        const Scale scale = shared ? *shared : snap(own);
        std::vector<u32> normal;
        for (const u32 f : drawn) {
            if (std::find(big.begin(), big.end(), f) == big.end()) {
                normal.push_back(f);
            }
        }
        std::optional<Scale> bigScale;
        if (!big.empty()) {
            bigScale = snap(fitMm(big, in.poster ? 420.0f : 260.0f, kDrawH));
        }
        // Where every part goes: rows of the track, wrapping only for a size band.
        const u32 lane = wraps ? 0u : ++laneCount_;
        struct Placed {
            u32 file;
            float left, w, h, below;
            double mm;
            u32 row;
            float slot;
        };
        std::vector<Placed> placed;
        struct Segment {
            u32 row;
            float x, length;
            double mm;
        };
        std::vector<Segment> segments;
        struct Divider {
            u32 row;
            float x;
            std::string label;
        };
        std::vector<Divider> dividers;
        u32 row = 0;
        float x = kMargin, segStart = kMargin;
        const float limit = wraps ? trackW : 1e9f;
        // Labels repeat: then their subtitles tell them apart.
        std::map<std::string, u32> labels;
        for (const u32 f : files) {
            ++labels[tree.file(f).names.label];
        }
        const auto endSegment = [&](float until, double mm) {
            if (until - segStart > 4.0f) {
                segments.push_back({row, segStart, until - segStart, mm});
            }
        };
        const auto place = [&](u32 f, double mm) {
            const Drawn d = *drawnOf(tree.file(f));
            const float w = float(d.w / mm), h = float(d.h / mm), below = float(d.below / mm);
            const File& file = tree.file(f);
            const bool dup = labels[file.names.label] > 1 && !file.names.sub.empty();
            const float labW = std::min(
                88.0f,
                float(std::max<usize>(characters(file.names.label), dup ? 9 : 0)) * 6.1f + 10.0f);
            const float slot = std::max(w, labW);
            if (x + slot > limit - kMargin && x > segStart + 1.0f) {
                endSegment(x - kGap + 8.0f, mm);
                ++row;
                x = segStart = kMargin;
            }
            placed.push_back({f, x + (slot - w) / 2.0f, w, h, below, mm, row, slot});
            x += slot + kGap;
        };
        for (const u32 f : normal) {
            place(f, scale.mm);
        }
        endSegment(x - kGap + 8.0f, scale.mm);
        if (bigScale) {
            x += 14.0f;
            dividers.push_back({row, x,
                                std::format("drawn {}\xC3\x97 smaller \xC2\xB7 {}",
                                            number(bigScale->mm / scale.mm), bigScale->label)});
            x += 20.0f;
            segStart = x;
            for (const u32 f : big) {
                place(f, bigScale->mm);
            }
            endSegment(x - kGap + 8.0f, bigScale->mm);
        }
        const u32 rows = row + 1;
        const float base0 = y + kPadTop + kDrawH;
        const float contentW = wraps ? trackW : std::max(x + kMargin, trackW);
        if (lane != 0) {
            result.lanes.push_back({lane, {trackX, y, trackW, float(rows) * kRowH}, contentW});
        }

        // The head stands on the first ruler line: stamps, two meta lines, the title, its eyebrow.
        const float stampTop = base0 - 17.5f;
        const float metaBottom = stampTop - 7.0f;
        const float meta2 = metaBottom - 15.5f, meta1 = meta2 - 15.5f;
        const float titleTop = meta1 - 3.0f - 17.0f;
        const float eyebrowTop = titleTop - 2.0f - 14.0f;
        const auto inBox = [](float top, float box, const TextStyle& style) {
            return top + (box - lineHeightOf(style)) / 2.0f + ascentOf(style);
        };
        const float headX = left_ + 4.0f, headRoom = kHeadW - 18.0f;
        if (!s.eyebrow.empty()) {
            out.group(headGroups[0]);
            out.text(s.eyebrow, kEyebrow, palette.muted, headX, inBox(eyebrowTop, 14.0f, kEyebrow),
                     headRoom);
        }
        out.group(headGroups[1]);
        out.target(s.folder == kNone ? GeoTargetNone : GeoTargetFolder, s.folder);
        out.text(s.title, kTitle, palette.on, headX, inBox(titleTop, 17.0f, kTitle), headRoom);
        out.column(leftGroup);
        out.target(GeoTargetNone, 0);
        out.group(headGroups[2]);
        out.text(count(s.files.size(), "file"), kMeta, palette.muted, headX,
                 inBox(meta1, 15.5f, kMeta), headRoom);
        out.group(headGroups[3]);
        if (const std::string range = rangeOf(tree, bySize(tree, normal)); !range.empty()) {
            out.text(range, kMeta, palette.muted, headX, inBox(meta2, 15.5f, kMeta), headRoom);
        }
        // The stamps: this strip's scale, and the larger files' after the divider.
        out.group(headGroups[4]);
        float sx = headX;
        const std::string stamps[2] = {scale.label,
                                       bigScale ? "larger " + bigScale->label : std::string()};
        for (usize i = 0; i < 2; ++i) {
            const std::string& stamp = stamps[i];
            if (stamp.empty()) {
                continue;
            }
            const float w = std::ceil(textWidth(stamp, kStamp) + 10.0f);
            const bool soft = i == 1; // the larger files' scale, quieter
            // The box aligns on nothing; the stamps' texts on one baseline.
            out.group(0);
            out.rect(sx, stampTop, w, 17.5f, 0, soft ? palette.faint : palette.on, 1.25f, 3.0f);
            out.group(headGroups[4]);
            out.text(stamp, kStamp, soft ? palette.muted : palette.on, sx + 5.0f,
                     inBox(stampTop, 17.5f, kStamp), w - 9.0f);
            sx += w + 6.0f;
        }
        out.group(0);
        // More than the track shows: how many, on the ruler's number line.
        if (lane != 0 && contentW > trackW + 2.0f) {
            usize shown = 0;
            for (const Placed& p : placed) {
                shown += p.left + p.w <= trackW ? 1 : 0;
            }
            out.text(grouped(placed.size() - shown) + " more \xE2\x86\x92 drag or scroll", kNote,
                     palette.muted, headX, base0 + 18.0f, headRoom);
        } else if (rows > 1) {
            out.text(count(rows, "row"), kNote, palette.muted, headX, base0 + 18.0f, headRoom);
        }

        // The track: rulers, dividers, then each part over its label.
        out.lane(lane);
        const float offsetX = trackX; // lane content starts at the track's left edge
        for (const Segment& g : segments) {
            ruler(offsetX + g.x, y + float(g.row) * kRowH + kPadTop + kDrawH, g.length, g.mm);
        }
        for (const Divider& d : dividers) {
            const float dy = y + float(d.row) * kRowH + 10.0f;
            out.line(offsetX + d.x, dy, offsetX + d.x, dy + kPadTop + kDrawH + kRulerH - 6.0f,
                     palette.faint, 1.25f, GeoItemDashed);
            out.text(d.label, kNote, palette.muted, offsetX + d.x + 8.0f,
                     dy + 4.0f + ascentOf(kNote), 260.0f);
        }
        std::vector<u32> rowGroups;
        for (u32 r = 0; r < rows * 3; ++r) {
            rowGroups.push_back(newGroup());
        }
        const u32 selectedFile = in.selected;
        for (const Placed& p : placed) {
            const File& file = tree.file(p.file);
            const float baseline = y + float(p.row) * kRowH + kPadTop + kDrawH;
            // Ink stands this far above the ruler's line: clear of its stroke, half of which lies
            // above the line, and a point more.
            constexpr float kClear = 1.75f;
            const float stand = baseline - kClear;
            const float left = offsetX + p.left;
            const bool hovered = in.hoverKind == GeoTargetFile && in.hover == p.file;
            const bool selected = selectedFile == p.file;
            const bool faded = in.matches && !in.matches->contains(p.file);
            const usize first = out.items().size();
            out.target(GeoTargetFile, p.file);
            out.key(keyOf("part", p.file));
            const float top = stand - p.below - p.h;
            if (selected || hovered) {
                out.rect(left - 8.0f, top - 8.0f, p.w + 16.0f, p.h + p.below + 16.0f,
                         selected ? palette.wash : palette.lift,
                         selected ? palette.washLine : palette.liftLine, 1.5f, 8.0f, GeoItemSticker,
                         selected ? palette.washLine : palette.liftLine);
            }
            // Its ink stands just above the ruler: the part's foot is its shadow's depth above
            // that.
            part(p.file, left, stand - p.below, p.mm, rowGroups[p.row * 3]);
            lastLine(stand);
            const bool dup = labels[file.names.label] > 1 && !file.names.sub.empty();
            const float labW = p.slot + kGap - 6.0f;
            const float labX = left + p.w / 2.0f - labW / 2.0f;
            const float labBase = baseline + kRulerH + 2.0f + ascentOf(kPartLabel);
            out.group(rowGroups[p.row * 3 + 1]);
            out.text(file.names.label, kPartLabel, palette.on, labX, labBase, labW, 1);
            out.group(rowGroups[p.row * 3 + 2]);
            if (dup) {
                out.text(file.names.sub, kPartSub, palette.muted, labX, labBase + 12.5f, labW, 1);
            }
            out.group(0);
            if (hovered && !selected) {
                calipers(file, left, top, p.w, p.h);
            }
            if (faded) {
                for (usize i = first; i < out.items().size(); ++i) {
                    out.item(i).flags |= GeoItemFading;
                }
            }
            out.target(GeoTargetNone, 0);
            out.key(0);
            result.parts.push_back({GeoTargetFile, p.file, left - 8.0f, top - 8.0f, p.w + 16.0f,
                                    baseline + kRulerH + kLabH - top + 8.0f, lane});
            if (hovered && selectedFile != kNone && selectedFile != p.file) {
                compare_ = {p.file, left + p.w / 2.0f, top, lane};
            }
        }
        out.lane(0);
        // Files not measured yet, listed at the end of the head column's last line.
        if (!unmeasured.empty() && placed.empty()) {
            out.text(count(unmeasured.size(), "file") + " not measured yet", kNote, palette.muted,
                     trackX + kMargin, base0 - 4.0f, trackW - 2.0f * kMargin);
        }
        const float height = std::max(float(rows) * kRowH, kMinStrip);
        out.line(edgeL_, y + height, edgeR_, y + height, palette.rule, 1.0f);
        return y + height + 1.0f;
    }

    /// Calipers over a part: its measured x and y across the top, z down the right.
    void calipers(const File& f, float l, float t, float w, float h) {
        if (!f.facts || !f.facts->hasBox) {
            return;
        }
        const float r = l + w, b = t + h, top = t - 11.0f, right = r + 11.0f;
        const u32 ink = palette.on;
        out.line(l, t - 3.0f, l, top - 4.0f, ink, 0.8f);
        out.line(r, t - 3.0f, r, top - 4.0f, ink, 0.8f);
        out.line(l, top, r, top, ink, 1.0f);
        const std::string across =
            number(f.facts->size[0]) + " \xC3\x97 " + number(f.facts->size[1]) + " mm";
        const float aw = textWidth(across, kCaliper);
        out.text(across, kCaliper, ink, (l + r) / 2.0f - aw / 2.0f, top - 5.0f, aw);
        out.line(r + 3.0f, t, right + 4.0f, t, ink, 0.8f);
        out.line(r + 3.0f, b, right + 4.0f, b, ink, 0.8f);
        out.line(right, t, right, b, ink, 1.0f);
        const std::string down = number(f.facts->size[2]) + " mm";
        out.text(down, kCaliper, ink, right + 6.0f, (t + b) / 2.0f + 3.5f,
                 textWidth(down, kCaliper));
    }

    /// With a part chosen, the one hovered against it: a card over it with the difference.
    void comparison(float width) {
        if (compare_.file == kNone || in.selected == kNone) {
            return;
        }
        const File& a = tree.file(compare_.file);
        const File& b = tree.file(in.selected);
        if (!a.facts || !b.facts || !a.facts->hasBox || !b.facts->hasBox) {
            return;
        }
        const std::string head = a.names.label + " against " + b.names.label;
        const std::string delta = std::format("\xCE\x94 x {}   y {}   z {} mm",
                                              signedMm(a.facts->size[0] - b.facts->size[0]),
                                              signedMm(a.facts->size[1] - b.facts->size[1]),
                                              signedMm(a.facts->size[2] - b.facts->size[2]));
        const std::string parts =
            std::format("parts {}", signedMm(double(a.facts->parts) - double(b.facts->parts)));
        const float w = std::max({textWidth(head, kDeltaSmall), textWidth(delta, kDelta),
                                  textWidth(parts, kDeltaSmall)}) +
                        24.0f;
        const float h = 12.0f + 3.0f * 15.0f + 6.0f;
        const float laneLeft = left_ + kHeadW;
        const float cx = compare_.x - laneScroll_(compare_.lane);
        const float x = std::clamp(cx - w / 2.0f, laneLeft + 6.0f, left_ + width - w - 6.0f);
        const float y = std::max(2.0f, compare_.top - h - 30.0f);
        const u32 ink = hex(0x231E16), muted = hex(0x6F685C), paper = hex(0xF7F5EE);
        out.rect(x, y, w, h, paper, ink, 1.25f, 8.0f, GeoItemSticker, ink);
        float baseline = y + 8.0f + ascentOf(kDeltaSmall);
        out.text(head, kDeltaSmall, muted, x + 12.0f, baseline, w - 24.0f);
        baseline += 15.0f;
        out.text(delta, kDelta, ink, x + 12.0f, baseline, w - 24.0f);
        baseline += 15.0f;
        out.text(parts, kDeltaSmall, muted, x + 12.0f, baseline, w - 24.0f);
    }

    std::function<float(u32)> laneScroll_ = [](u32) { return 0.0f; };
    // Where strips start, and how far bands and rules run either side: the content's edges, or a
    // poster's frame.
    float left_ = kContentLeft, edgeL_ = 0.0f, edgeR_ = 0.0f;
    usize posterFamilies_ = 0;
    bool posterBands_ = false;
    u32 ink() const { return palette.on; }

    /// A poster's one scale: the typical part about 34 points tall. None when the families differ
    /// in size by more than 20 times, or the folder falls into size bands: each keeps its own.
    std::optional<Scale> posterScale(const std::vector<Group>& groups) const {
        std::vector<double> typical, all;
        bool bands = false;
        for (const Group& g : groups) {
            for (const Strip& s : g.strips) {
                bands = bands || s.wrap;
                std::vector<double> hs;
                for (const u32 f :
                     familyScale(tree, drawable(tree, s.files), kOverW, kDrawH).normal) {
                    const Drawn d = *drawnOf(tree.file(f));
                    hs.push_back(d.h + d.above + d.below);
                }
                if (hs.empty()) {
                    continue;
                }
                std::sort(hs.begin(), hs.end());
                typical.push_back(hs[(hs.size() - 1) / 2]);
                all.insert(all.end(), hs.begin(), hs.end());
            }
        }
        if (bands || all.empty()) {
            return std::nullopt;
        }
        const auto [lo, hi] = std::minmax_element(typical.begin(), typical.end());
        if (typical.size() > 1 && *hi / std::max(1e-9, *lo) > 20.0) {
            return std::nullopt;
        }
        std::sort(all.begin(), all.end());
        return snap(all[all.size() / 2] / 34.0);
    }

    /// A drafting title block, its bottom right at (right, bottom): title, sheet, contents, scale,
    /// source, and who drew it. Returns its top.
    float titleBlock(float right, float bottom,
                     const std::vector<std::pair<std::string, std::string>>& rows) {
        constexpr float w = 380.0f, rowH = 22.0f, firstH = 32.0f, keyW = 92.0f;
        const float h = firstH + rowH * float(rows.size() - 1);
        const float x = right - w, top = bottom - h;
        out.rect(x, top, w, h, 0, palette.on, 1.5f, 0.0f, GeoItemSheet);
        out.line(x + keyW, top, x + keyW, bottom, palette.on, 1.0f);
        constexpr TextStyle key{9.0f, 600, 0.12f, true}, value{11.0f, 500, 0.0f, false},
            big{15.0f, 600, 0.0f, false};
        float y = top;
        for (usize i = 0; i < rows.size(); ++i) {
            const float rh = i == 0 ? firstH : rowH;
            if (i > 0) {
                out.line(x, y, x + w, y, palette.on, 1.0f);
            }
            const TextStyle& v = i == 0 ? big : value;
            const float baseline = y + (rh - lineHeightOf(v)) / 2.0f + ascentOf(v);
            const u32 g = newGroup();
            out.group(g);
            out.text(rows[i].first, key, palette.muted, x + 10.0f, baseline, keyW - 16.0f);
            out.text(rows[i].second, v, palette.on, x + keyW + 10.0f, baseline, w - keyW - 20.0f);
            out.group(0);
            y += rh;
        }
        return top;
    }

private:
    u32 laneCount_ = 0;
    struct Compare {
        u32 file = kNone;
        float x = 0, top = 0;
        u32 lane = 0;
    } compare_;
};

} // namespace

LayoutResult layoutLadders(const LayoutInput& input, Display& display,
                           const std::function<float(u32)>& laneScroll) {
    LayoutResult result;
    Ladders ladders(input, display, result);
    ladders.laneScroll_ = laneScroll;
    const Tree& tree = *input.tree;
    if (tree.folder(input.folder).count == 0) {
        return layoutPlates(input, display); // nothing to draw: as Plates says it
    }
    float width = input.width - kContentLeft - kContentRight;
    const std::vector<Group> groups = ladders.groupsOf(input.folder);
    std::optional<Scale> shared;
    if (input.poster) {
        shared = ladders.posterScale(groups);
    } else if (input.laddersShared) {
        shared = ladders.sharedScale(groups);
    }
    const u32 leftGroup = ladders.newGroup();
    float y = kContentTop;
    ladders.edgeR_ = input.width;
    // A poster: every family wrapped on one plate inside a drawing's frame, a title block below.
    constexpr float kFrame = 7.0f, kInside = 28.0f;
    const float frameL = kContentLeft, frameR = input.width - kContentRight;
    if (input.poster) {
        ladders.left_ = frameL + kInside;
        ladders.edgeL_ = frameL + kFrame;
        ladders.edgeR_ = frameR - kFrame;
        width = frameR - frameL - 2.0f * kInside;
        const u32 n = tree.folder(input.folder).count;
        usize families = 0;
        bool bands = false;
        for (const Group& g : groups) {
            families += g.strips.size();
            for (const Strip& s : g.strips) {
                bands = bands || s.wrap;
            }
        }
        const std::string summary =
            shared ? std::format("{} on one plate, every family at {}", count(n, "file"),
                                 shared->label)
                   : std::format("{}, each {} to its own scale, stamped beside it",
                                 count(n, "file"), bands ? "band" : "family");
        y = ladders.posterHead(summary, "Ladders", width, ladders.left_, kContentTop + kInside);
        ladders.posterFamilies_ = families;
        ladders.posterBands_ = bands;
    }
    struct Stuck {
        usize first, last;
        float top, height;
    };
    std::vector<Stuck> stuck;
    for (const Group& g : groups) {
        // A heading that would only repeat its one strip's name is left out.
        const bool echo =
            g.folder != kNone && g.strips.size() == 1 && g.strips[0].folder == g.folder;
        if (!g.heading.empty() && !echo && !input.poster) {
            usize first = 0;
            const float top = y;
            y = ladders.heading(g, y, width, first);
            stuck.push_back({first, display.items().size(), top, y - top});
        }
        for (const Strip& s : g.strips) {
            u32 headGroups[5];
            for (u32& h : headGroups) {
                h = ladders.newGroup();
            }
            y = ladders.strip(s, y, width, shared, leftGroup, headGroups);
        }
    }
    for (usize i = 0; i < stuck.size(); ++i) {
        const float next = i + 1 < stuck.size() ? stuck[i + 1].top : y;
        const float until = std::max(0.0f, next - stuck[i].top - stuck[i].height);
        for (usize item = stuck[i].first; item < stuck[i].last; ++item) {
            display.item(item).sticky_top = stuck[i].top;
            display.item(item).sticky_until = until;
        }
    }
    ladders.comparison(width);
    if (input.poster) {
        const Tree& t = tree;
        std::string source = t.title();
        if (const std::string& inside = t.folder(input.folder).path; !inside.empty()) {
            source += "/" + inside;
        }
        const u32 n = t.folder(input.folder).count;
        const float top = y + 24.0f;
        const float bottom =
            top + 32.0f + 22.0f * 5.0f; // the title block's height: a title row and five more
        ladders.titleBlock(frameR - kInside, bottom,
                           {{"Title", t.titleOf(input.folder)},
                            {"Sheet", "1 of 1"},
                            {"Contents", count(n, "file") + " \xC2\xB7 " +
                                             count(ladders.posterFamilies_,
                                                   ladders.posterBands_ ? "band" : "family",
                                                   ladders.posterBands_ ? "bands" : "families")},
                            {"Scale", shared ? shared->label : std::string("as stamped")},
                            {"Source", source},
                            {"Drawn by", "GeometryView"}});
        const float frameB = bottom + kInside;
        // The frame: a heavy rule, and a light one inside it.
        display.rect(frameL, kContentTop, frameR - frameL, frameB - kContentTop, 0, ladders.ink(),
                     1.5f);
        display.rect(frameL + kFrame, kContentTop + kFrame, frameR - frameL - 2.0f * kFrame,
                     frameB - kContentTop - 2.0f * kFrame, 0, ladders.ink(), 1.0f);
        y = frameB;
    }
    result.contentHeight = y + kContentBottom;
    return result;
}

} // namespace geo::gallery
