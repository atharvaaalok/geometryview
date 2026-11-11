#include "gallery/view.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace geo::gallery {

void Gallery::setEntries(std::string_view title, std::span<const Entry> entries) {
    tree_.build(title, entries);
    folder_ = 0;
    selected_ = focused_ = kNone;
    hoverKind_ = GeoTargetNone;
    expanded_.clear();
    dirty_ = true;
}

void Gallery::setFacts(u32 file, Facts facts) {
    tree_.setFacts(file, std::move(facts));
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
        folder_ = folder;
        selected_ = focused_ = kNone;
        hoverKind_ = GeoTargetNone;
        scroll_ = 0.0f;
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
    result_ = layoutPlates(input, display_);
    return true;
}

GeoGalleryHit Gallery::hit(float x, float y) const {
    GeoGalleryHit hit{GeoTargetNone, 0, 0};
    // The topmost item with a target under the point; text and pictures by their boxes.
    const std::vector<GeoGalleryItem>& items = display_.items();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->target_kind == GeoTargetNone) {
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
    folder_ = folder;
    selected_ = focused_ = kNone;
    hoverKind_ = GeoTargetNone;
    scroll_ = 0.0f;
    dirty_ = true;
    return {GeoActionEnterFolder, folder};
}

GeoGalleryAction Gallery::click(u32 kind, u32 index, bool twice) {
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
        if (selected_ != kNone) {
            return {GeoActionPeek, selected_};
        }
        return {GeoActionNone, 0};
    case GeoKeyEscape:
        // The selection, then the focus, then up a folder; at the top the gallery closes.
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
