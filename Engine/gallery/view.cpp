#include "gallery/view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace geo::gallery {

void Gallery::setEntries(std::string_view title, std::span<const Entry> entries) {
    tree_.build(title, entries);
    folder_ = 0;
    viewOf_.clear();
    peek_ = kNone;
    matches_.reset();
    selected_ = focused_ = kNone;
    hoverKind_ = GeoTargetNone;
    expanded_.clear();
    dirty_ = true;
}

void Gallery::setFacts(u32 file, Facts facts) {
    tree_.setFacts(file, std::move(facts));
    if (matches_) {
        findMatches(); // the name inside the file is searched too
    }
    dirty_ = true;
}

void Gallery::setPicture(u32 file, const Picture& picture) {
    tree_.setPicture(file, picture);
    File& f = tree_.file(file);
    f.reading = -1.0f;
    f.stale = false;
    dirty_ = true;
}

void Gallery::setPoster(bool poster, std::string date) {
    if (poster != poster_ || date != posterDate_) {
        poster_ = poster;
        posterDate_ = std::move(date);
        pendingMotion_ = GeoTransitionNone;
        dirty_ = true;
    }
}

void Gallery::setLost(bool lost) {
    if (lost != lost_) {
        lost_ = lost;
        dirty_ = true;
    }
}

// A folder that can no longer be read: what was last seen, faded, under a card in the view's top
// left with one way back.
void Gallery::layoutLost() {
    for (usize i = 0; i < display_.items().size(); ++i) {
        display_.item(i).flags |= GeoItemLost;
    }
    const u32 ink = hex(0x231E16), muted = hex(0x6F685C), paper = hex(0xF7F5EE);
    constexpr TextStyle kTitle{12.5f, 600, 0.0f, false};
    constexpr TextStyle kLine{11.5f, 400, 0.0f, false};
    constexpr TextStyle kPillText{11.5f, 500, 0.0f, false};
    const usize first = display_.items().size();
    const float w = std::min(300.0f, width_ - 16.0f), x = 8.0f, y = 8.0f;
    const float h =
        14.0f + lineHeightOf(kTitle) + 2.0f + lineHeightOf(kLine) + 10.0f + 30.0f + 14.0f;
    display_.key(keyOf("lost", folder_));
    display_.group(0);
    display_.target(GeoTargetPeek, 0); // the card itself does nothing
    display_.rect(x, y, w, h, paper, ink, 1.5f, 12.0f, GeoItemSticker, ink);
    float baseline = y + 14.0f + ascentOf(kTitle);
    display_.text(tree_.titleOf(folder_), kTitle, ink, x + 16.0f, baseline, w - 32.0f);
    baseline += kDescent * kTitle.size + 2.0f + ascentOf(kLine);
    display_.text("Allow reading this folder again.", kLine, muted, x + 16.0f, baseline, w - 32.0f);
    const float py = baseline + kDescent * kLine.size + 10.0f;
    const std::string label = "Reconnect";
    const float pw = std::round(textWidth(label, kPillText) + 21.0f);
    display_.target(GeoTargetReconnect, 0);
    display_.rect(x + 16.0f, py, pw, 30.0f, ink, ink, 1.5f, 15.0f);
    display_.text(label, kPillText, paper, x + 16.0f + 10.5f,
                  py + (30.0f - lineHeightOf(kPillText)) / 2.0f + ascentOf(kPillText), pw);
    display_.target(GeoTargetNone, 0);
    display_.key(0);
    for (usize i = first; i < display_.items().size(); ++i) {
        display_.item(i).flags |= GeoItemFixed;
    }
}

void Gallery::setReading(u32 file, float fraction) {
    File& f = tree_.file(file);
    // A layout for each sixteenth read at most: the bar moves in steps it can show.
    const float next =
        fraction < 0.0f ? -1.0f : std::floor(std::min(fraction, 1.0f) * 16.0f) / 16.0f;
    if (next != f.reading) {
        f.reading = next;
        dirty_ = true;
    }
}

void Gallery::setChanged(u32 file, bool changed) {
    File& f = tree_.file(file);
    if (f.changed != changed) {
        f.changed = changed;
        f.stale = changed && f.picture.has_value();
        dirty_ = true;
    }
}

void Gallery::seen(u32 file) {
    if (file < tree_.fileCount() && tree_.file(file).changed) {
        tree_.file(file).changed = false;
        dirty_ = true;
    }
}

void Gallery::setFailure(u32 file, std::string message) {
    tree_.setFailure(file, std::move(message));
    dirty_ = true;
}

void Gallery::setStyle(u32 style) {
    if (style != style_) {
        style_ = style;
        dirty_ = true;
    }
}

void Gallery::setViewport(float width, float height) {
    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        dirty_ = true;
    }
}

bool Gallery::setFolder(std::string_view path) {
    const u32 folder = tree_.folderAt(path);
    if (folder == kNone) {
        return false;
    }
    if (folder != folder_) {
        viewOf_[folder_] = view_;
        folder_ = folder;
        if (const auto it = viewOf_.find(folder);
            it != viewOf_.end() && (views_ & (1u << it->second))) {
            view_ = it->second;
        }
        selected_ = focused_ = kNone;
        hoverKind_ = GeoTargetNone;
        peek_ = kNone;
        scroll_ = 0.0f;
        atlas_.reset();
        spread_ = 0;
        findMatches();
        dirty_ = true;
    }
    return true;
}

std::string Gallery::subtitle() const {
    const Folder& f = tree_.folder(folder_);
    std::string out = count(f.count, "file");
    if (!f.folders.empty()) {
        out += " \xC2\xB7 " + count(f.folders.size(), "folder");
    }
    return out;
}

bool Gallery::layout() {
    if (!dirty_) {
        return false;
    }
    dirty_ = false;
    display_.clear();
    LayoutInput input;
    input.tree = &tree_;
    input.folder = folder_;
    input.width = width_;
    input.height = height_;
    input.style = style_;
    input.hoverKind = hoverKind_;
    input.hover = hover_;
    input.selected = selected_;
    input.focused = focused_;
    input.expanded = &expanded_;
    input.sheetScale = sheetScale_;
    input.sheetCell = sheetCell_;
    input.sort = sort_;
    input.matches = matches_ ? &*matches_ : nullptr;
    if (poster_) {
        // Printed: nothing hovered, selected, focused or peeked, every band whole.
        input.poster = true;
        input.posterDate = posterDate_;
        input.hoverKind = GeoTargetNone;
        input.selected = input.focused = kNone;
        input.matches = nullptr;
    }
    input.laddersShared = laddersShared_;
    switch (view_) {
    case GeoViewSheet:
        result_ = layoutSheet(input, display_);
        break;
    case GeoViewLadders:
        result_ = layoutLadders(input, display_, [this](u32 lane) { return laneScroll(lane); });
        break;
    case GeoViewAtlas:
        result_ = layoutAtlas(input, display_, atlasCamera());
        break;
    case GeoViewCatalogue:
        result_ = layoutCatalogue(input, display_, spread_);
        break;
    default:
        result_ = layoutPlates(input, display_);
        break;
    }
    if (lost_ && !poster_) {
        layoutLost();
    }
    if (peek_ != kNone && !poster_) {
        layoutPeek();
    }
    // What this layout plays against the one before: once, for the change that made it.
    transition_ = {};
    tracks_.clear();
    if (pendingMotion_ != GeoTransitionNone) {
        transition_.kind = pendingMotion_;
        transition_.duration = pendingMotion_ == GeoTransitionSwitch ? 520.0f : 420.0f;
        transition_.fade_out = pendingMotion_ == GeoTransitionSwitch ? 260.0f : 420.0f;
        std::optional<std::array<float, 4>> rect = pendingRect_;
        if (pendingMotion_ == GeoTransitionOut && !rect) {
            // Back into the plate it came from, where that plate is now.
            for (const LayoutResult::Place& p : result_.plates) {
                if (p.index == focused_) {
                    rect = std::array<float, 4>{p.x, p.y - scroll_, p.w, p.h};
                }
            }
        }
        if (rect) {
            std::copy(rect->begin(), rect->end(), transition_.rect);
        } else if (pendingMotion_ != GeoTransitionSwitch) {
            transition_.kind = GeoTransitionNone; // nothing to grow from: it just shows
        }
        if (pendingMotion_ == GeoTransitionSwitch) {
            // Parts in both views fly to their new places; parts new to the view fade in from 35%
            // of the way.
            const std::vector<GeoGalleryItem>& items = display_.items();
            for (u32 i = 0; i < items.size(); ++i) {
                if (items[i].kind != GeoItemPicture || (items[i].flags & GeoItemFixed) != 0) {
                    continue;
                }
                GeoGalleryTrack track{};
                track.item = i;
                if (const auto it = before_.find(items[i].file); it != before_.end()) {
                    track.kind = GeoTrackFly;
                    std::copy(it->second.begin(), it->second.end(), track.from);
                    track.delay = 0.0f;
                    track.duration = 520.0f;
                } else {
                    track.kind = GeoTrackFadeIn;
                    track.from[0] = items[i].x;
                    track.from[1] = items[i].y - scroll_;
                    track.from[2] = items[i].width;
                    track.from[3] = items[i].height;
                    track.delay = 520.0f * 0.35f;
                    track.duration = 520.0f * 0.65f;
                }
                tracks_.push_back(track);
            }
        }
        pendingMotion_ = GeoTransitionNone;
        pendingRect_.reset();
        before_.clear();
    }
    return true;
}

void Gallery::beginMotion(u32 kind, std::optional<std::array<float, 4>> rect) {
    before_.clear();
    for (const GeoGalleryItem& item : display_.items()) {
        if (item.kind == GeoItemPicture && (item.flags & GeoItemFixed) == 0) {
            before_[item.file] = {item.x, item.y - scroll_, item.width, item.height};
        }
    }
    pendingMotion_ = kind;
    pendingRect_ = rect;
}

void Gallery::setViews(u32 mask) {
    views_ = mask | (1u << GeoViewPlates);
    if ((views_ & (1u << view_)) == 0) {
        view_ = GeoViewPlates;
        dirty_ = true;
    }
}

bool Gallery::setView(u32 view) {
    if (view > GeoViewCatalogue || (views_ & (1u << view)) == 0) {
        return false;
    }
    viewOf_[folder_] = view;
    if (view == view_) {
        return true;
    }
    beginMotion(GeoTransitionSwitch);
    view_ = view;
    hoverKind_ = GeoTargetNone;
    focused_ = kNone; // plates are Plates' own; the selection carries across
    dirty_ = true;
    return true;
}

void Gallery::setSheet(u32 scale, float cell, u32 sort) {
    scale = std::min<u32>(scale, GeoScaleTrue);
    cell = std::clamp(cell, 72.0f, 200.0f);
    sort = std::min<u32>(sort, GeoSortSize);
    if (scale != sheetScale_ || cell != sheetCell_ || sort != sort_) {
        sheetScale_ = scale;
        sheetCell_ = cell;
        sort_ = sort;
        dirty_ = true;
    }
}

void Gallery::setSearch(std::string_view query) {
    if (query == search_) {
        return;
    }
    search_ = std::string(query);
    findMatches();
    dirty_ = true;
}

void Gallery::findMatches() {
    // Trimmed, and compared in lower case.
    std::string q = search_;
    while (!q.empty() && q.back() == ' ') {
        q.pop_back();
    }
    while (!q.empty() && q.front() == ' ') {
        q.erase(q.begin());
    }
    if (q.empty()) {
        matches_.reset();
        return;
    }
    const auto lower = [](std::string text) {
        for (char& c : text) {
            c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        return text;
    };
    q = lower(q);
    std::unordered_set<u32> found;
    for (const u32 f : tree_.filesUnder(folder_)) {
        const File& file = tree_.file(f);
        const std::string haystack =
            lower(file.names.label + " " + file.names.sub + " " + file.path + " " +
                  (file.facts ? file.facts->product : std::string()));
        if (haystack.find(q) != std::string::npos) {
            found.insert(f);
        }
    }
    matches_ = std::move(found);
}

void Gallery::peek(u32 file) {
    const u32 next = file < tree_.fileCount() ? file : kNone;
    if (next != peek_) {
        peek_ = next;
        peekTurnable_ = false; // another file's turntable is yet to come
        seen(next);
        dirty_ = true;
    }
}

float Gallery::reveal() const {
    if (selected_ == kNone || view_ == GeoViewAtlas) {
        return -1.0f; // the map pans; it does not scroll
    }
    for (const auto* places : {&result_.parts, &result_.covers}) {
        for (const LayoutResult::Place& p : *places) {
            if (p.index != selected_) {
                continue;
            }
            if (p.y >= scroll_ && p.y + p.h <= scroll_ + height_) {
                return -1.0f;
            }
            const float most = std::max(0.0f, result_.contentHeight - height_);
            return std::clamp(p.y - height_ / 3.0f, 0.0f, most);
        }
    }
    return -1.0f;
}

namespace {

// A count as Peek writes it: 1,234 up to ten thousand, then 12.3k and 4.56M.
std::string compactCount(u64 n) {
    if (n < 10000) {
        return grouped(n);
    }
    return n < 1000000 ? number(double(n) / 1e3) + "k" : number(double(n) / 1e6) + "M";
}

} // namespace

std::optional<std::pair<u32, float>> Gallery::revealLane() const {
    if (selected_ == kNone) {
        return std::nullopt;
    }
    for (const LayoutResult::Place& p : result_.parts) {
        if (p.index != selected_ || p.lane == 0) {
            continue;
        }
        const GeoGalleryLane* lane = laneOf(p.lane);
        if (lane == nullptr) {
            return std::nullopt;
        }
        const float scroll = laneScroll(p.lane);
        const float left = p.x - lane->rect[0] - scroll, right = left + p.w;
        if (left >= 0.0f && right <= lane->rect[2]) {
            return std::nullopt; // it shows
        }
        const float most = std::max(0.0f, lane->content_width - lane->rect[2]);
        const float centred = p.x - lane->rect[0] + p.w / 2.0f - lane->rect[2] / 2.0f;
        return std::pair{p.lane, std::clamp(centred, 0.0f, most)};
    }
    return std::nullopt;
}

// Peek: the file large over the view, as the design's card has it. A scrim over the view, then the
// card, in the chrome's own paper and ink whatever the style: the name and where the file is, the
// part on the style's sheet, its measured size, parts, triangles and file, and Open.
void Gallery::layoutPeek() {
    const File& f = tree_.file(peek_);
    const Palette palette = paletteOf(style_);
    const u32 ink = hex(0x231E16), muted = hex(0x6F685C), paper = hex(0xF7F5EE),
              rule = hex(0xDDD6C8);
    constexpr float kPad = 18.0f, kTop = 16.0f, kGap = 12.0f, kPill = 30.0f;
    constexpr TextStyle kName{15.0f, 600, 0.0f, false};
    constexpr TextStyle kNameSub{15.0f, 400, 0.0f, false};
    constexpr TextStyle kWhere{11.5f, 400, 0.0f, false};
    constexpr TextStyle kStat{20.0f, 600, 0.0f, false};
    constexpr TextStyle kStatFirst{17.0f, 600, 0.0f, false};
    constexpr TextStyle kUnit{10.5f, 400, 0.04f, false};
    constexpr TextStyle kPillText{11.5f, 500, 0.0f, false};
    const usize first = display_.items().size();
    display_.key(keyOf("peek", peek_));
    display_.group(0);

    // The scrim: a click on it closes Peek.
    display_.target(GeoTargetNone, 0);
    display_.rect(0.0f, 0.0f, width_, height_,
                  palette.dark ? rgba(10, 18, 36, 0.55f) : rgba(242, 239, 231, 0.62f), 0, 0.0f);

    const float headH = lineHeightOf(kName) + 2.0f + lineHeightOf(kWhere);
    const float w = std::max(240.0f, std::min(780.0f, width_ - 32.0f));
    // On a narrow view, a phone's, the stats go in two rows of two.
    const bool twoRows = w - 2.0f * kPad < 420.0f;
    const float statH = lineHeightOf(kStat) + lineHeightOf(kUnit);
    const float statsH = twoRows ? 2.0f * statH + 8.0f : statH;
    const float rest = kTop + headH + kGap + kGap + statsH + kGap + kPill + kTop;
    const float picH = std::clamp(height_ - 48.0f - rest, 120.0f, 390.0f);
    const float h = rest + picH;
    const float x = std::round((width_ - w) / 2.0f);
    const float y = std::round(std::max(8.0f, (height_ - h) / 2.0f));
    const float inner = w - 2.0f * kPad;
    display_.target(GeoTargetPeek, 0);
    display_.rect(x, y, w, h, paper, ink, 1.5f, 12.0f, GeoItemSticker, ink);

    // The name, the name inside it, and where the file is; the close button beside them.
    const float nameBaseline = y + kTop + ascentOf(kName);
    const float nameRoom = inner - kPill - 12.0f;
    const float nameW = display_.text(f.names.label, kName, ink, x + kPad, nameBaseline, nameRoom);
    const float subX = x + kPad + nameW + textWidth("  ", kNameSub);
    if (!f.names.sub.empty() && x + kPad + nameRoom - subX > textWidth("    ", kNameSub)) {
        display_.text(f.names.sub, kNameSub, muted, subX, nameBaseline, x + kPad + nameRoom - subX);
    }
    std::vector<std::string> trail;
    for (u32 at = f.folder; at != kNone; at = tree_.folder(at).parent) {
        trail.push_back(tree_.folder(at).parent == kNone ? tree_.title() : tree_.folder(at).name);
    }
    std::string where;
    for (auto it = trail.rbegin(); it != trail.rend(); ++it) {
        where += (where.empty() ? "" : " / ") + *it;
    }
    where += (where.empty() ? "" : " \xC2\xB7 ") + f.name;
    display_.text(where, kWhere, muted, x + kPad,
                  y + kTop + lineHeightOf(kName) + 2.0f + ascentOf(kWhere), nameRoom);
    const float cx = x + w - kPad - kPill, cy = y + kTop;
    constexpr TextStyle kClose{17.0f, 400, 0.0f, false};
    display_.target(GeoTargetPeek, 1);
    display_.rect(cx, cy, kPill, kPill, paper, ink, 1.5f, kPill / 2.0f);
    display_.text("\xC3\x97", kClose, ink, cx,
                  cy + (kPill - lineHeightOf(kClose)) / 2.0f + ascentOf(kClose), kPill, 1);
    display_.target(GeoTargetPeek, 0);

    // The part on the style's sheet, as large as its picture shows sharply.
    const float px = x + kPad, py = y + kTop + headH + kGap;
    display_.rect(px, py, inner, picH, 0, rule, 1.0f, 8.0f, GeoItemSheet);
    const auto centred = [&](std::string_view text) {
        display_.text(text, kWhere, palette.muted, px, py + picH / 2.0f, inner, 1);
    };
    if (peekTurnable_) {
        // The turntable: the painter fits its frames into the area, with room for the hint.
        constexpr TextStyle kHint{10.5f, 400, 0.04f, false};
        display_.picture(peek_, px, py, inner, picH - 22.0f);
        display_.last().flags |= GeoItemTurn;
        display_.text("Drag to turn \xC2\xB7 \xE2\x86\x90 \xE2\x86\x92 turn", kHint, palette.muted,
                      px, py + picH - 10.0f, inner, 1);
    } else if (f.picture && f.picture->mmPerPixel > 0 && f.picture->ink[2] > 0 &&
               f.picture->ink[3] > 0) {
        const Picture& p = *f.picture;
        const float k =
            std::min({(inner - 48.0f) / p.ink[2], (picH - 48.0f) / p.ink[3], kMaxPointsPerPixel});
        const float midX = px + inner / 2.0f, midY = py + picH / 2.0f;
        display_.picture(peek_, midX - (p.ink[0] + p.ink[2] / 2.0f) * k,
                         midY - (p.ink[1] + p.ink[3] / 2.0f) * k, float(p.width) * k,
                         float(p.height) * k);
    } else if (f.failed()) {
        centred(f.failure);
    } else {
        centred(f.facts ? "Drawing\xE2\x80\xA6" : "Not measured yet");
    }

    // Size, parts, triangles and file, each a number over what it counts, on two baselines.
    const float sy = py + picH + kGap;
    const std::string none = "\xE2\x80\x94"; // —
    std::string size = none;
    if (f.facts && f.facts->hasBox) {
        size = number(f.facts->size[0]) + " \xC3\x97 " + number(f.facts->size[1]) + " \xC3\x97 " +
               number(f.facts->size[2]);
    }
    std::string ext = f.name.substr(std::min(f.name.size(), f.name.rfind('.')));
    std::ranges::transform(ext, ext.begin(), [](char c) { return char(std::tolower(c)); });
    const u64 parts = f.facts ? f.facts->parts : 0;
    const std::string bytes = f.bytes == 0 ? none
                              : f.bytes >= 1000000
                                  ? number(double(f.bytes) / 1e6) + " MB"
                                  : number(double(std::max<u64>(f.bytes, 100)) / 1e3) + " KB";
    struct Stat {
        std::string value, unit;
        float share;
    };
    const Stat stats[4] = {
        {size,
         f.facts && f.facts->estimated ? "mm, x \xC2\xB7 y \xC2\xB7 z, estimated"
                                       : "mm, x \xC2\xB7 y \xC2\xB7 z",
         1.9f},
        {f.facts ? grouped(parts) : none, parts == 1 ? "part" : "parts", 1.0f},
        {f.facts && f.facts->triangles > 0 ? compactCount(f.facts->triangles) : none, "triangles",
         1.0f},
        {bytes, ext.empty() ? "file" : ext + " file", 1.0f},
    };
    const usize perRow = twoRows ? 2 : 4;
    for (usize row = 0; row * perRow < 4; ++row) {
        const float top = sy + float(row) * (statH + 8.0f);
        const float statBaseline = top + ascentOf(kStat);
        const float unitBaseline = top + lineHeightOf(kStat) + ascentOf(kUnit);
        const u32 numbers = ++result_.groups, units = ++result_.groups;
        float shares = 0.0f;
        for (usize i = row * perRow; i < (row + 1) * perRow; ++i) {
            shares += twoRows ? 1.0f : stats[i].share;
        }
        const float room = inner - float(perRow - 1) * 8.0f;
        float sx = px;
        for (usize i = row * perRow; i < (row + 1) * perRow; ++i) {
            const float cw = room * (twoRows ? 1.0f : stats[i].share) / shares;
            display_.group(numbers);
            display_.text(stats[i].value, i == 0 ? kStatFirst : kStat, ink, sx, statBaseline, cw);
            display_.last().line = statBaseline;
            display_.group(units);
            display_.text(stats[i].unit, kUnit, muted, sx, unitBaseline, cw);
            display_.last().line = unitBaseline;
            sx += cw + 8.0f;
        }
    }
    display_.group(0);

    // Open, as ⏎ does.
    const float ay = sy + statsH + kGap;
    const std::string open = "Open  \xE2\x8F\x8E";
    const float openW = std::round(textWidth(open, kPillText) + 21.0f);
    display_.target(GeoTargetFile, peek_);
    display_.rect(px, ay, openW, kPill, ink, ink, 1.5f, kPill / 2.0f);
    display_.text(open, kPillText, paper, px + 10.5f,
                  ay + (kPill - lineHeightOf(kPillText)) / 2.0f + ascentOf(kPillText), openW);
    display_.target(GeoTargetNone, 0);
    // A narrow card is a phone's, where nothing types: a tap outside closes it.
    display_.text(twoRows ? "Tap outside to close" : "Space or Esc closes", kWhere, muted,
                  px + openW + 12.0f, ay + (kPill - lineHeightOf(kWhere)) / 2.0f + ascentOf(kWhere),
                  inner - openW - 12.0f);

    display_.key(0);
    for (usize i = first; i < display_.items().size(); ++i) {
        display_.item(i).flags |= GeoItemFixed;
    }
}

GeoGalleryHit Gallery::hit(float x, float y) const {
    GeoGalleryHit hit{GeoTargetNone, 0, 0};
    // The topmost item with a target under the point; text and pictures by their boxes. Peek's
    // card covers the view: only what is on it, in the view's points, can be hit.
    const std::vector<GeoGalleryItem>& items = display_.items();
    const bool peeking = peek_ != kNone;
    const float contentY = y, contentX = x;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        // Peek's items cover everything; the lost card's cover the content beneath it. Fixed items
        // are in the view's points.
        const bool fixed = (it->flags & GeoItemFixed) != 0;
        const bool reachable =
            peeking ? fixed
                    : !fixed || it->target_kind == GeoTargetReconnect ||
                          ((lost_ || view_ == GeoViewAtlas) && it->target_kind == GeoTargetPeek);
        if (it->target_kind == GeoTargetNone || !reachable) {
            continue;
        }
        y = fixed ? contentY - scroll_ : contentY;
        // An item in a lane is placed in the lane's content, moved by its scroll, and shows only
        // within the lane.
        x = contentX;
        if (it->lane != 0) {
            const GeoGalleryLane* lane = laneOf(it->lane);
            if (lane == nullptr || contentX < lane->rect[0] ||
                contentX > lane->rect[0] + lane->rect[2]) {
                continue;
            }
            x = contentX + laneScroll(it->lane);
        }
        float x0 = it->x, y0 = it->y, x1 = it->x + it->width, y1 = it->y + it->height;
        if (x0 > x1) {
            std::swap(x0, x1);
        }
        if (y0 > y1) {
            std::swap(y0, y1);
        }
        if (x >= x0 - 2 && x <= x1 + 2 && y >= y0 - 2 && y <= y1 + 2) {
            return {it->target_kind, it->target, it->key};
        }
    }
    if (peeking) {
        return hit;
    }
    x = contentX;
    y = contentY;
    // A plate is wholly its folder's, between its items too.
    for (const LayoutResult::Place& p : result_.plates) {
        if (x >= p.x && x <= p.x + p.w && y >= p.y && y <= p.y + p.h) {
            return {p.kind, p.index, keyOf("plate", p.index)};
        }
    }
    y = contentY;
    for (const LayoutResult::Place& p : result_.parts) {
        x = contentX;
        if (p.lane != 0) {
            const GeoGalleryLane* lane = laneOf(p.lane);
            if (lane == nullptr || contentX < lane->rect[0] ||
                contentX > lane->rect[0] + lane->rect[2]) {
                continue;
            }
            x += laneScroll(p.lane);
        }
        if (x >= p.x && x <= p.x + p.w && y >= p.y && y <= p.y + p.h) {
            return {p.kind, p.index, keyOf("part", p.index)};
        }
    }
    return hit;
}

bool Gallery::hover(u32 kind, u32 index) {
    if (kind == GeoTargetMore) {
        kind = GeoTargetNone;
    }
    if (kind == hoverKind_ && (kind == GeoTargetNone || index == hover_)) {
        return false;
    }
    hoverKind_ = kind;
    hover_ = index;
    if (kind == GeoTargetFile) {
        seen(index);
    }
    dirty_ = true;
    return true;
}

GeoGalleryAction Gallery::enter(u32 folder) {
    // Into a folder from its plate: it grows out of the plate. Up: it shrinks back into its plate,
    // where the folder above shows plates.
    const bool up = tree_.folder(folder_).parent == folder;
    std::optional<std::array<float, 4>> rect;
    for (const LayoutResult::Place& p : result_.plates) {
        if (p.index == folder) {
            rect = std::array<float, 4>{p.x, p.y - scroll_, p.w, p.h};
        }
    }
    // A folder shows the view it last showed; one not seen keeps the view in use.
    u32 next = view_;
    if (const auto it = viewOf_.find(folder);
        it != viewOf_.end() && (views_ & (1u << it->second))) {
        next = it->second;
    }
    if (up ? next == GeoViewPlates : view_ == GeoViewPlates && rect) {
        beginMotion(up ? GeoTransitionOut : GeoTransitionInto, rect);
    }
    viewOf_[folder_] = view_;
    folder_ = folder;
    view_ = next;
    selected_ = focused_ = kNone;
    hoverKind_ = GeoTargetNone;
    peek_ = kNone;
    scroll_ = 0.0f;
    atlas_.reset();
    spread_ = 0;
    findMatches();
    dirty_ = true;
    return {GeoActionEnterFolder, folder};
}

GeoGalleryAction Gallery::click(u32 kind, u32 index, bool twice) {
    if (peek_ != kNone) {
        // On the card nothing happens; Open opens; anywhere else closes Peek.
        if (kind == GeoTargetPeek && index == 0) {
            return {GeoActionNone, 0};
        }
        const u32 file = peek_;
        peek(kNone);
        if (kind == GeoTargetFile && index == file) {
            return {GeoActionOpenFile, file};
        }
        return {GeoActionHandled, 0};
    }
    switch (kind) {
    case GeoTargetPage:
        setCatalogueSpread(index / 2); // the spread the page is on
        return {GeoActionHandled, 0};
    case GeoTargetReconnect:
        return {GeoActionReconnect, 0};
    case GeoTargetPeek:
        return {GeoActionNone, 0}; // the lost card itself
    case GeoTargetFolder:
        return enter(index);
    case GeoTargetFile:
        seen(index);
        if (twice) {
            return {GeoActionOpenFile, index};
        }
        selected_ = index;
        focused_ = kNone;
        dirty_ = true;
        return {GeoActionHandled, index};
    case GeoTargetMore:
        if (index < result_.bands.size()) {
            expanded_.insert(result_.bands[index]);
            dirty_ = true;
            return {GeoActionHandled, 0};
        }
        return {GeoActionNone, 0};
    default:
        if (selected_ != kNone || focused_ != kNone) {
            selected_ = focused_ = kNone;
            dirty_ = true;
            return {GeoActionHandled, 0};
        }
        return {GeoActionNone, 0};
    }
}

std::optional<usize> Gallery::nearest(const std::vector<LayoutResult::Place>& places, usize from,
                                      int dx, int dy) {
    const auto centre = [](const LayoutResult::Place& p) {
        return std::array<float, 2>{p.x + p.w / 2.0f, p.y + p.h / 2.0f};
    };
    const auto [fx, fy] = centre(places[from]);
    std::optional<usize> best;
    float bestScore = INFINITY;
    for (usize i = 0; i < places.size(); ++i) {
        if (i == from) {
            continue;
        }
        const auto [x, y] = centre(places[i]);
        const float along = (x - fx) * float(dx) + (y - fy) * float(dy);
        if (along <= 4.0f) {
            continue;
        }
        const float across = std::fabs((x - fx) * float(dy)) + std::fabs((y - fy) * float(dx));
        const float score = along + across * 2.0f;
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

GeoGalleryAction Gallery::key(u32 key) {
    const Folder& folder = tree_.folder(folder_);
    const auto indexOf = [](const std::vector<LayoutResult::Place>& places,
                            u32 index) -> std::optional<usize> {
        for (usize i = 0; i < places.size(); ++i) {
            if (places[i].index == index) {
                return i;
            }
        }
        return std::nullopt;
    };
    if (peek_ != kNone && peekTurnable_ && (key == GeoKeyLeft || key == GeoKeyRight)) {
        // A part that can turn: ← and → turn it, ↑ and ↓ still go file to file.
        return {GeoActionTurn, key == GeoKeyLeft ? 0u : 1u};
    }
    if (view_ == GeoViewCatalogue && (key == GeoKeyLeft || key == GeoKeyRight)) {
        // ← and → turn the book a spread.
        const u32 before = spread_;
        setCatalogueSpread(key == GeoKeyLeft ? (spread_ == 0 ? 0 : spread_ - 1) : spread_ + 1);
        return {before != spread_ ? GeoActionHandled : GeoActionNone, 0};
    }
    switch (key) {
    case GeoKeyLeft:
    case GeoKeyRight:
    case GeoKeyUp:
    case GeoKeyDown: {
        const int dx = key == GeoKeyLeft ? -1 : key == GeoKeyRight ? 1 : 0;
        const int dy = key == GeoKeyUp ? -1 : key == GeoKeyDown ? 1 : 0;
        // Between plates while no file is selected, else between files.
        if (selected_ == kNone && !result_.plates.empty()) {
            const std::optional<usize> at = indexOf(result_.plates, focused_);
            if (!at) {
                focused_ = result_.plates.front().index;
            } else if (const auto next = nearest(result_.plates, *at, dx, dy)) {
                focused_ = result_.plates[*next].index;
            } else if (dy > 0 && !result_.parts.empty()) {
                focused_ = kNone;
                selected_ = result_.parts.front().index;
            }
            dirty_ = true;
            return {GeoActionHandled, 0};
        }
        if (!result_.parts.empty()) {
            const std::optional<usize> at = indexOf(result_.parts, selected_);
            if (!at) {
                selected_ = result_.parts.front().index;
            } else if (const auto next = nearest(result_.parts, *at, dx, dy)) {
                selected_ = result_.parts[*next].index;
            } else if (dy < 0 && !result_.plates.empty() && peek_ == kNone) {
                selected_ = kNone;
                focused_ = result_.plates.back().index;
            }
            if (peek_ != kNone && selected_ != kNone) {
                peek(selected_); // Peek follows the selection, file to file
            }
            seen(selected_);
            dirty_ = true;
            return {GeoActionHandled, 0};
        }
        return {GeoActionNone, 0};
    }
    case GeoKeyEnter:
        if (focused_ != kNone) {
            return enter(focused_);
        }
        if (selected_ != kNone) {
            return {GeoActionOpenFile, selected_};
        }
        return {GeoActionNone, 0};
    case GeoKeySpace:
        // Peek opens on the selection, and closes again.
        if (peek_ != kNone) {
            peek(kNone);
            return {GeoActionHandled, 0};
        }
        if (selected_ != kNone) {
            peek(selected_);
            return {GeoActionPeek, selected_};
        }
        return {GeoActionNone, 0};
    case GeoKeyEscape:
        // Peek, then the search, then the selection and focus, then up a folder; at the top the
        // gallery closes.
        if (peek_ != kNone) {
            peek_ = kNone;
            dirty_ = true;
            return {GeoActionHandled, 0};
        }
        if (!search_.empty()) {
            setSearch("");
            return {GeoActionHandled, 0};
        }
        if (selected_ != kNone || focused_ != kNone) {
            selected_ = focused_ = kNone;
            dirty_ = true;
            return {GeoActionHandled, 0};
        }
        [[fallthrough]];
    case GeoKeyFolderUp:
        if (folder.parent != kNone) {
            const u32 from = folder_;
            GeoGalleryAction action = enter(folder.parent);
            focused_ = from; // back on the plate it came from
            return action;
        }
        return {GeoActionLeave, 0};
    default:
        return {GeoActionNone, 0};
    }
}

std::vector<GeoGalleryWant> Gallery::wanted() const {
    std::vector<GeoGalleryWant> out;
    std::unordered_set<u32> seen;
    const float top = scroll_, bottom = scroll_ + height_;
    const float middle = (top + bottom) / 2.0f;
    const auto colourOf = [&](u32 file) {
        return i32(tree_.folder(tree_.file(file).folder).colour);
    };
    const auto add = [&](u32 file, u32 band, double order) {
        if (seen.insert(file).second) {
            out.push_back({file, band, order, colourOf(file)});
        }
    };
    // What is drawn in view, then near it, by distance from the middle. In a lane, what shows
    // across it: a lane's width either side is near.
    for (const auto* places : {&result_.covers, &result_.parts}) {
        for (const LayoutResult::Place& p : *places) {
            const float centre = p.y + p.h / 2.0f;
            bool across = true, nearAcross = true;
            if (const GeoGalleryLane* lane = p.lane != 0 ? laneOf(p.lane) : nullptr) {
                const float left = p.x - laneScroll(p.lane), right = left + p.w;
                const float l0 = lane->rect[0], l1 = lane->rect[0] + lane->rect[2];
                across = right >= l0 && left <= l1;
                nearAcross = right >= l0 - lane->rect[2] && left <= l1 + lane->rect[2];
            }
            const bool inView = across && p.y + p.h >= top && p.y <= bottom;
            const bool near = nearAcross && p.y + p.h >= top - height_ && p.y <= bottom + height_;
            if (inView || near) {
                add(p.index, inView ? 1 : 2, std::fabs(centre - middle));
            }
        }
    }
    // A plate in view wants a sample of its files, spread through them, before any is measured:
    // its cover is drawn from those.
    for (const LayoutResult::Place& p : result_.plates) {
        const bool inView = p.y + p.h >= top && p.y <= bottom;
        const std::vector<u32> under = tree_.filesUnder(p.index);
        const usize n = std::min<usize>(under.size(), 12);
        for (usize i = 0; i < n; ++i) {
            add(under[i * under.size() / n], inView ? 1 : 2, std::fabs(p.y - middle) + double(i));
        }
    }
    // Files not drawn yet in this folder's own shelf are in view too, so they get their facts.
    for (const u32 f : tree_.folder(folder_).files) {
        add(f, 2, 1e6 + double(f));
    }
    // The rest of the folder, for when it is entered.
    for (const u32 f : tree_.filesUnder(folder_)) {
        add(f, 3, double(out.size()));
    }
    return out;
}

} // namespace geo::gallery
