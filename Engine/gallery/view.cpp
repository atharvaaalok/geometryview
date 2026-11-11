#include "gallery/view.h"

#include <algorithm>
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
    dirty_ = true;
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
    result_ = view_ == GeoViewSheet ? layoutSheet(input, display_) : layoutPlates(input, display_);
    if (peek_ != kNone) {
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
        dirty_ = true;
    }
}

float Gallery::reveal() const {
    if (selected_ == kNone) {
        return -1.0f;
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

// Peek: the file large, with its measured size, parts, triangles and file, on a card over the
// view, in the chrome's own paper and ink whatever the style.
void Gallery::layoutPeek() {
    const File& f = tree_.file(peek_);
    const u32 ink = hex(0x231E16), muted = hex(0x6F685C), paper = hex(0xF7F5EE);
    const float w = std::min(560.0f, width_ - 80.0f), h = std::min(480.0f, height_ - 60.0f);
    const float x = (width_ - w) / 2.0f, y = (height_ - h) / 2.0f;
    const usize first = display_.items().size();
    display_.key(keyOf("peek", peek_));
    display_.rect(x, y, w, h, paper, ink, 1.5f, 14.0f, GeoItemSticker, ink);
    const float artW = w - 48.0f, artH = h - 170.0f;
    if (const std::optional<std::array<double, 2>> size = f.drawnSize();
        size && f.picture && f.picture->mmPerPixel > 0) {
        const Picture& p = *f.picture;
        double mm = std::max((*size)[0] / artW, (*size)[1] / artH);
        mm = std::max(mm, p.mmPerPixel / kMaxPointsPerPixel);
        const float k = float(p.mmPerPixel / mm);
        const float partW = p.part[2] * k, partH = p.part[3] * k;
        const float px = x + (w - partW) / 2.0f, bottom = y + 24.0f + (artH + partH) / 2.0f;
        display_.picture(peek_, px - p.part[0] * k, bottom - (p.part[1] + p.part[3]) * k,
                         float(p.width) * k, float(p.height) * k);
    }
    constexpr TextStyle kName{15.0f, 600, 0.0f, false};
    constexpr TextStyle kRow{11.5f, 400, 0.0f, false};
    constexpr TextStyle kRowLabel{10.5f, 600, 0.12f, true};
    float baseline = y + h - 128.0f;
    display_.text(f.names.label, kName, ink, x + 24.0f, baseline, w - 48.0f);
    baseline += 18.0f;
    if (!f.names.sub.empty()) {
        display_.text(f.names.sub, kRow, muted, x + 24.0f, baseline, w - 48.0f);
    }
    std::vector<std::pair<std::string, std::string>> rows;
    if (f.facts && f.facts->hasBox) {
        const double sides[3] = {f.facts->size[0], f.facts->size[1], f.facts->size[2]};
        rows.push_back({"Size", dimensions(sides)});
    }
    if (f.facts) {
        rows.push_back({"Parts", count(f.facts->parts, "part")});
    }
    std::string fileRow = f.name;
    if (f.bytes > 0) {
        fileRow += " Â· " + (f.bytes >= 1000000 ? number(double(f.bytes) / 1e6) + " MB"
                                                : number(double(f.bytes) / 1e3) + " KB");
    }
    rows.push_back({"File", fileRow});
    if (f.failed()) {
        rows.push_back({"Couldn't read", f.failure});
    }
    baseline += 24.0f;
    for (const auto& [label, value] : rows) {
        display_.text(label, kRowLabel, muted, x + 24.0f, baseline, 110.0f);
        display_.text(value, kRow, ink, x + 140.0f, baseline, w - 164.0f);
        baseline += 18.0f;
    }
    display_.key(0);
    for (usize i = first; i < display_.items().size(); ++i) {
        display_.item(i).flags |= GeoItemFixed;
        display_.item(i).group = 0;
    }
}

GeoGalleryHit Gallery::hit(float x, float y) const {
    GeoGalleryHit hit{GeoTargetNone, 0, 0};
    // The topmost item with a target under the point; text and pictures by their boxes.
    const std::vector<GeoGalleryItem>& items = display_.items();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->target_kind == GeoTargetNone || (it->flags & GeoItemFixed) != 0) {
            continue;
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
    // A plate is wholly its folder's, between its items too.
    for (const LayoutResult::Place& p : result_.plates) {
        if (x >= p.x && x <= p.x + p.w && y >= p.y && y <= p.y + p.h) {
            return {p.kind, p.index, keyOf("plate", p.index)};
        }
    }
    for (const LayoutResult::Place& p : result_.parts) {
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
    dirty_ = true;
    return true;
}

GeoGalleryAction Gallery::enter(u32 folder) {
    // Into a folder from its plate: it grows out of the plate. Up: it shrinks back into it.
    const bool up = tree_.folder(folder_).parent == folder;
    std::optional<std::array<float, 4>> rect;
    for (const LayoutResult::Place& p : result_.plates) {
        if (p.index == folder) {
            rect = std::array<float, 4>{p.x, p.y - scroll_, p.w, p.h};
        }
    }
    if (view_ == GeoViewPlates && (rect || up)) {
        beginMotion(up ? GeoTransitionOut : GeoTransitionInto, rect);
    }
    // A folder shows the view it last showed; one not seen keeps the view in use.
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
    findMatches();
    dirty_ = true;
    return {GeoActionEnterFolder, folder};
}

GeoGalleryAction Gallery::click(u32 kind, u32 index, bool twice) {
    if (peek_ != kNone) {
        // A click anywhere closes Peek.
        peek(kNone);
        return {GeoActionHandled, 0};
    }
    switch (kind) {
    case GeoTargetFolder:
        return enter(index);
    case GeoTargetFile:
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
            } else if (dy < 0 && !result_.plates.empty()) {
                selected_ = kNone;
                focused_ = result_.plates.back().index;
            }
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
    // What is drawn in view, then near it, by distance from the middle.
    for (const auto* places : {&result_.covers, &result_.parts}) {
        for (const LayoutResult::Place& p : *places) {
            const float centre = p.y + p.h / 2.0f;
            const bool inView = p.y + p.h >= top && p.y <= bottom;
            const bool near = p.y + p.h >= top - height_ && p.y <= bottom + height_;
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
