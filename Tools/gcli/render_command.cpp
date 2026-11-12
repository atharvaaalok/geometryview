// gcli render: one cache file drawn to a PNG, or many in one run with one renderer kept for all.
//
//   gcli render <metallib> <blob> <png> [width height azimuth elevation ortho|perspective style
//                                        colour section plain|overlay isolate]
//   gcli render <metallib> <blob> <png> --flag value ...
//   gcli render <metallib> --batch [--same-scale] [--flags for every shot] < shots
//
// The positional form is the one MCP's render_view uses, and stays as it is. The flags reach
// everything the renderer offers. Named, the overlay is off unless asked for. In a batch each line
// is one shot, `<blob> <png> [--flags]`, its flags over the batch's; blank lines and lines
// starting with # are skipped, and paths with spaces go in double quotes. --same-scale draws every
// shot at the scale the largest needs, so a turntable or a cut doesn't change size as it moves.
#include "render_command.h"

#include "base/types.h"
#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace geo::gcli {
namespace {

std::optional<GeoStyle> styleNamed(std::string_view name) {
    const char* names[] = {"illustration", "metal", "clay", "blueprint", "technical", "studio"};
    for (int i = 0; i < 6; ++i) {
        if (name == names[i] || name == std::to_string(i)) {
            return GeoStyle(i);
        }
    }
    return std::nullopt;
}

std::optional<GeoColorMode> colourNamed(std::string_view name) {
    const char* names[] = {"material", "parts", "mono", "file"};
    for (int i = 0; i < 4; ++i) {
        if (name == names[i] || name == std::to_string(i)) {
            return GeoColorMode(i);
        }
    }
    return std::nullopt;
}

// A section as axis letters and fractions of the bounds, such as "x0.5" or "x0.5,z-0.3", where a
// minus removes the low side. Anything else, such as "none", cuts nothing.
void parseSection(std::string_view text, GeoSection& section) {
    for (std::string_view cut : std::views::split(text, ',') | std::views::transform([](auto r) {
                                    return std::string_view(r.begin(), r.end());
                                })) {
        if (cut.size() < 2 || cut[0] < 'x' || cut[0] > 'z') {
            continue;
        }
        const int axis = cut[0] - 'x';
        const bool flip = cut[1] == '-';
        section.enabled[axis] = true;
        section.flip[axis] = flip;
        section.position[axis] =
            std::strtof(std::string(cut.substr(flip ? 2 : 1)).c_str(), nullptr);
    }
}

// One shot: what to draw, where to, and how. Strings are kept here, since options only point at
// them.
struct Shot {
    std::string blob, png, isolate;
    GeoRenderOptions options = geo_default_render_options();
    bool fixedScale = false; // given its own --world-per-pixel
};

// Applies flags to a shot. Returns false, with the reason, on one it doesn't know or a bad value.
bool applyFlags(const std::vector<std::string>& args, usize from, Shot& shot, bool& report,
                std::string& error) {
    GeoRenderOptions& o = shot.options;
    for (usize i = from; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        // Flags without a value.
        if (flag == "--perspective") {
            o.perspective = true;
            continue;
        }
        if (flag == "--ortho") {
            o.perspective = false;
            continue;
        }
        if (flag == "--overlay") {
            o.overlay = true;
            continue;
        }
        if (flag == "--plain") {
            o.overlay = false;
            continue;
        }
        if (flag == "--transparent") {
            o.transparent = true;
            continue;
        }
        if (flag == "--opaque") {
            o.transparent = false;
            continue;
        }
        if (flag == "--report") {
            report = true;
            continue;
        }
        if (flag == "--same-scale") {
            continue; // the batch's, read before
        }
        if (i + 1 >= args.size()) {
            error = std::string(flag) + " needs a value";
            return false;
        }
        const std::string& value = args[++i];
        const double number = std::atof(value.c_str());
        if (flag == "--size") {
            const usize x = value.find('x');
            if (x == std::string::npos) {
                error = "--size takes WIDTHxHEIGHT";
                return false;
            }
            o.width = u32(std::atoi(value.substr(0, x).c_str()));
            o.height = u32(std::atoi(value.substr(x + 1).c_str()));
        } else if (flag == "--az" || flag == "--azimuth") {
            o.azimuth_degrees = number;
        } else if (flag == "--el" || flag == "--elevation") {
            o.elevation_degrees = number;
        } else if (flag == "--style") {
            const std::optional<GeoStyle> style = styleNamed(value);
            if (!style) {
                error = "unknown style " + value;
                return false;
            }
            o.look.style = *style;
            o.look.cast_shadow = geo_style_casts_shadow(*style);
        } else if (flag == "--colour" || flag == "--color") {
            const std::optional<GeoColorMode> colour = colourNamed(value);
            if (!colour) {
                error = "unknown colour mode " + value;
                return false;
            }
            o.look.color_mode = *colour;
        } else if (flag == "--section") {
            o.section = {};
            parseSection(value, o.section);
        } else if (flag == "--isolate") {
            shot.isolate = value;
        } else if (flag == "--supersample") {
            o.supersample = u32(std::clamp(std::atoi(value.c_str()), 1, 4));
        } else if (flag == "--world-per-pixel") {
            o.world_per_pixel = number;
            shot.fixedScale = number > 0.0;
        } else if (flag == "--line-scale") {
            o.line_scale = float(number);
        } else if (flag == "--pixels-per-point") {
            o.pixels_per_point = float(number);
        } else if (flag == "--first-color") {
            o.first_color = std::atoi(value.c_str());
        } else if (flag == "--shadow") {
            o.look.cast_shadow = value == "on" || value == "1";
        } else if (flag == "--grain") {
            o.look.grain = float(number);
        } else if (flag == "--wobble") {
            o.look.wobble = float(number);
        } else {
            error = "unknown flag " + std::string(flag);
            return false;
        }
    }
    return true;
}

// Splits a line into words, keeping double-quoted ones whole.
std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> out;
    usize i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(u8(line[i]))) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        std::string word;
        if (line[i] == '"') {
            const usize end = line.find('"', i + 1);
            word = line.substr(i + 1, (end == std::string_view::npos ? line.size() : end) - i - 1);
            i = end == std::string_view::npos ? line.size() : end + 1;
        } else {
            const usize start = i;
            while (i < line.size() && !std::isspace(u8(line[i]))) {
                ++i;
            }
            word = line.substr(start, i - start);
        }
        out.push_back(std::move(word));
    }
    return out;
}

void printRect(const char* name, const uint32_t r[4]) {
    std::print(",\"{}\":[{},{},{},{}]", name, r[0], r[1], r[2], r[3]);
}

// Draws the shots with one renderer, writing each PNG; prints a JSON line a shot when asked.
int drawShots(const char* shaders, std::vector<Shot>& shots, bool sameScale, bool report) {
    for (const Shot& shot : shots) {
        const u32 n = std::max(shot.options.supersample, 1u);
        if (std::max(shot.options.width, shot.options.height) * n > 16384) {
            std::println(stderr, "{}: {}x{} drawn {}x larger is over 16384 pixels a side", shot.png,
                         shot.options.width, shot.options.height, n);
            return 1;
        }
    }
    GeoThumbnailer* thumbnailer = geo_thumbnailer_create(shaders);
    if (thumbnailer == nullptr) {
        return 1;
    }
    char error[512] = {};
    // One scale for all: each shot probed small at its own aspect, the coarsest scale kept.
    if (sameScale) {
        double shared = 0.0;
        for (Shot& shot : shots) {
            if (shot.fixedScale) {
                continue;
            }
            GeoRenderOptions probe = shot.options;
            probe.isolate = shot.isolate.empty() ? nullptr : shot.isolate.c_str();
            probe.width = 64;
            probe.height = std::max(1u, u32(std::lround(64.0 * shot.options.height /
                                                        std::max(1u, shot.options.width))));
            probe.supersample = 1;
            probe.overlay = false;
            GeoThumbnail picture{};
            if (geo_thumbnailer_render(thumbnailer, shot.blob.c_str(), &probe, &picture, error,
                                       sizeof(error))) {
                shared = std::max(shared, picture.world_per_pixel * 64.0 / shot.options.width);
            }
            geo_thumbnail_free(&picture);
        }
        for (Shot& shot : shots) {
            if (!shot.fixedScale) {
                shot.options.world_per_pixel = shared;
            }
        }
    }
    int status = 0;
    for (usize i = 0; i < shots.size(); ++i) {
        Shot& shot = shots[i];
        shot.options.isolate = shot.isolate.empty() ? nullptr : shot.isolate.c_str();
        const auto start = std::chrono::steady_clock::now();
        GeoThumbnail picture{};
        const bool drawn = geo_thumbnailer_render(thumbnailer, shot.blob.c_str(), &shot.options,
                                                  &picture, error, sizeof(error));
        const bool written = drawn && geo_thumbnail_write_png(&picture, shot.png.c_str());
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        if (!drawn || !written) {
            std::println(stderr, "{}: {}", shot.png, drawn ? "could not write it" : error);
            status = 1;
        }
        if (report) {
            std::print("{{\"index\":{},\"png\":\"{}\",\"ok\":{}", i, shot.png, drawn && written);
            if (drawn) {
                std::print(",\"world_per_pixel\":{}", picture.world_per_pixel);
                printRect("part_rect", picture.part_rect);
                printRect("ink_rect", picture.ink_rect);
            }
            std::println(",\"ms\":{:.1f}}}", ms);
        }
        geo_thumbnail_free(&picture);
    }
    geo_thumbnailer_destroy(thumbnailer);
    return status;
}

int usage() {
    std::println(stderr, "usage: gcli render <metallib> <blob> <png> [--size WxH] [--az A] "
                         "[--el E] [--perspective] [--style S] [--colour C] [--section x0.5,z-0.3] "
                         "[--isolate a,b] [--overlay] [--transparent] [--supersample N] "
                         "[--world-per-pixel X] [--line-scale X] [--pixels-per-point X] "
                         "[--first-color N] [--shadow on|off] [--grain X] [--wobble X] [--report]");
    std::println(stderr, "       gcli render <metallib> --batch [--same-scale] [--flags] < shots "
                         "(each line: <blob> <png> [--flags])");
    return 2;
}

// The original form: positional, every later option needing the ones before it.
int positional(int argc, const char** argv) {
    GeoRenderOptions options = geo_default_render_options();
    if (argc > 5) {
        options.width = u32(std::atoi(argv[5]));
    }
    if (argc > 6) {
        options.height = u32(std::atoi(argv[6]));
    }
    if (argc > 7) {
        options.azimuth_degrees = std::atof(argv[7]);
    }
    if (argc > 8) {
        options.elevation_degrees = std::atof(argv[8]);
    }
    options.perspective = argc > 9 && std::string_view(argv[9]) == "perspective";
    if (argc > 10) {
        options.look.style = GeoStyle(std::atoi(argv[10]));
        options.look.cast_shadow = geo_style_casts_shadow(options.look.style);
    }
    if (argc > 11) {
        options.look.color_mode = GeoColorMode(std::atoi(argv[11]));
    }
    if (argc > 12) {
        parseSection(argv[12], options.section);
    }
    options.overlay = !(argc > 13 && std::string_view(argv[13]) == "plain");
    // Comma-separated part names to draw alone.
    if (argc > 14) {
        options.isolate = argv[14];
    }
    return geo_render_blob_to_png(argv[2], argv[3], argv[4], &options) ? 0 : 1;
}

} // namespace

int renderCommand(int argc, const char** argv) {
    if (argc < 4) {
        return usage();
    }
    const std::vector<std::string> args(argv, argv + argc);
    const bool batch = args[3] == "--batch";
    if (!batch && (argc <= 5 || !args[5].starts_with("--"))) {
        return argc < 5 ? usage() : positional(argc, argv);
    }

    bool report = false;
    std::string error;
    if (!batch) {
        Shot shot;
        shot.blob = args[3];
        shot.png = args[4];
        shot.options.overlay = false;
        if (!applyFlags(args, 5, shot, report, error)) {
            std::println(stderr, "{}", error);
            return usage();
        }
        std::vector<Shot> shots{std::move(shot)};
        return drawShots(argv[2], shots, false, report);
    }

    // A batch: the flags after --batch apply to every shot, each line's own over them.
    const bool sameScale = std::ranges::contains(args, std::string("--same-scale"));
    Shot common;
    common.options.overlay = false;
    if (!applyFlags(args, 4, common, report, error)) {
        std::println(stderr, "{}", error);
        return usage();
    }
    std::vector<Shot> shots;
    std::string line;
    for (usize number = 1; std::getline(std::cin, line); ++number) {
        const std::vector<std::string> parts = words(line);
        if (parts.empty() || parts[0].starts_with("#")) {
            continue;
        }
        Shot shot = common;
        if (parts.size() < 2 || !applyFlags(parts, 2, shot, report, error)) {
            std::println(stderr, "line {}: {}", number,
                         parts.size() < 2 ? "needs a blob and a png" : error);
            return 2;
        }
        shot.blob = parts[0];
        shot.png = parts[1];
        shots.push_back(std::move(shot));
    }
    return drawShots(argv[2], shots, sameScale, report);
}

} // namespace geo::gcli
