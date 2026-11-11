#include "gallery/display.h"

#include <algorithm>

namespace geo::gallery {

Palette paletteOf(u32 style) {
    Palette p{};
    p.on = hex(0x231E16);
    p.muted = hex(0x6F685C);
    p.faint = hex(0x9A9284);
    p.rule = hex(0xDDD6C8);
    p.liftLine = p.on;
    p.wash = hex(0xDDE6F4);
    p.washLine = p.on;
    p.folders = {hex(0xF4B829), hex(0x93D3B0), hex(0xEF6A4C), hex(0xF3A6BB)};
    switch (style) {
    case 1: // Metal
        p.lift = hex(0xFFFFFF);
        p.band = hex(0xF6F5F3);
        break;
    case 2: // Clay
        p.lift = hex(0xFAF8F4);
        p.band = hex(0xF3F1EC);
        break;
    case 3: // Blueprint
        p.on = hex(0xEEF4FF);
        p.muted = hex(0xA9BEDF);
        p.faint = hex(0x7E95BD);
        p.rule = hex(0xEEF4FF, 0.22f);
        p.lift = hex(0x2A5592);
        p.liftLine = p.on;
        p.band = hex(0x1D4277);
        p.wash = rgba(118, 160, 226, 0.32f);
        p.washLine = p.on;
        p.dark = true;
        break;
    case 4: // Technical
        p.lift = hex(0xFFFFFF);
        p.band = hex(0xFFFFFF);
        break;
    case 5: // Studio
        p.on = hex(0xECEAE4);
        p.muted = hex(0xA8A59C);
        p.faint = hex(0x77756F);
        p.rule = hex(0xECEAE4, 0.18f);
        p.lift = hex(0x33353A);
        p.liftLine = p.on;
        p.band = hex(0x1F2023);
        p.wash = rgba(118, 160, 226, 0.32f);
        p.washLine = p.on;
        p.dark = true;
        break;
    default: // Illustration
        p.lift = hex(0xF7F5EE);
        p.band = hex(0xF2EFE7);
        break;
    }
    return p;
}

void Display::clear() {
    items_.clear();
    text_.clear();
    targetKind_ = GeoTargetNone;
    target_ = 0;
    key_ = 0;
    group_ = 0;
    dx_ = dy_ = 0.0f;
}

GeoGalleryItem& Display::add(u32 kind) {
    GeoGalleryItem item{};
    item.kind = kind;
    item.target_kind = targetKind_;
    item.target = target_;
    item.key = key_;
    item.group = group_;
    item.lane = lane_;
    items_.push_back(item);
    return items_.back();
}

void Display::rect(float x, float y, float w, float h, u32 fill, u32 stroke, float lineWidth,
                   float radius, u32 flags, u32 shadow) {
    GeoGalleryItem& item = add(GeoItemRect);
    item.x = x + dx_;
    item.y = y + dy_;
    item.width = w;
    item.height = h;
    item.fill = fill;
    item.stroke = stroke;
    item.line_width = stroke != 0 ? lineWidth : 0.0f;
    item.radius = radius;
    item.flags = flags;
    item.shadow = shadow;
    item.line = item.y + h;
}

void Display::line(float x0, float y0, float x1, float y1, u32 stroke, float width, u32 flags) {
    GeoGalleryItem& item = add(GeoItemLine);
    item.x = x0 + dx_;
    item.y = y0 + dy_;
    item.width = x1 - x0;
    item.height = y1 - y0;
    item.stroke = stroke;
    item.line_width = width;
    item.flags = flags;
    item.line = std::min(y0, y1) + dy_; // its top, where painters measure a line not level
}

float Display::text(std::string_view text, const TextStyle& style, u32 colour, float x,
                    float baseline, float width, u32 align) {
    const std::string set = setText(text, style, width);
    if (set.empty()) {
        return 0.0f;
    }
    GeoGalleryItem& item = add(GeoItemText);
    const float used = textWidth(set, style);
    item.x = x + dx_;
    item.width = width > 0.0f ? width : used;
    item.baseline = baseline + dy_;
    item.y = item.baseline - ascentOf(style);
    item.height = lineHeightOf(style);
    item.fill = colour;
    item.text = u32(text_.size());
    item.text_length = u32(set.size());
    item.font_size = style.size;
    item.tracking = style.tracking * style.size;
    item.weight = style.weight;
    item.align = align;
    item.line = item.baseline;
    text_ += set;
    return used;
}

void Display::picture(u32 file, float x, float y, float w, float h) {
    GeoGalleryItem& item = add(GeoItemPicture);
    item.x = x + dx_;
    item.y = y + dy_;
    item.width = w;
    item.height = h;
    item.file = file;
    item.line = item.y + h;
}

u32 keyOf(std::string_view what, u32 index) {
    // FNV-1a, then the index mixed in.
    u32 hash = 2166136261u;
    for (const char c : what) {
        hash = (hash ^ u8(c)) * 16777619u;
    }
    hash = (hash ^ index) * 16777619u;
    return hash == 0 ? 1 : hash;
}

} // namespace geo::gallery
