#include "render/shell.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace geo::render {
namespace {

std::string lowered(std::string_view name) {
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

bool contains(std::string_view text, std::initializer_list<std::string_view> words) {
    return std::any_of(words.begin(), words.end(), [&](std::string_view word) {
        return text.find(word) != std::string_view::npos;
    });
}

bool isWordCharacter(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// "cap" as a whole word, or "_cap" anywhere.
bool hasCap(std::string_view text) {
    if (text.find("_cap") != std::string_view::npos) {
        return true;
    }
    for (usize at = text.find("cap"); at != std::string_view::npos; at = text.find("cap", at + 1)) {
        const usize end = at + 3;
        if (end == text.size() || !isWordCharacter(text[end])) {
            return true;
        }
    }
    return false;
}

// Whether `outer`, grown by 2% of its diagonal, holds `inner`.
bool encloses(const Aabb& outer, const Aabb& inner) {
    const double grow = 0.02 * simd_length(outer.max - outer.min);
    return simd_all(inner.min >= outer.min - grow) && simd_all(inner.max <= outer.max + grow);
}

} // namespace

ShellCounts::ShellCounts(u32 partCount)
    : parts(partCount), front(partCount, 0.0), behind(partCount, 0.0) {}

void ShellCounts::accumulate(std::span<const u32> frontIds, std::span<const u32> behindIds) {
    for (usize i = 0; i < frontIds.size(); ++i) {
        const u32 f = frontIds[i], b = behindIds[i];
        if (f != 0 && f <= parts) {
            front[f - 1] += 1.0;
        }
        if (b != 0 && b <= parts) {
            behind[b - 1] += 1.0;
        }
        if (f != 0 && b != 0 && f <= parts && b <= parts) {
            pairs[u64(f - 1) * parts + (b - 1)] += 1.0;
        }
    }
}

std::vector<Vec3> shellViewDirections() {
    std::vector<Vec3> out;
    for (int axis = 0; axis < 3; ++axis) {
        Vec3 v = simd_make_double3(0, 0, 0);
        v[axis] = 1.0;
        out.push_back(v);
        out.push_back(-v);
    }
    for (const double x : {-1.0, 1.0}) {
        for (const double y : {-1.0, 1.0}) {
            for (const double z : {-1.0, 1.0}) {
                out.push_back(simd_normalize(simd_make_double3(x, y, z)));
            }
        }
    }
    return out;
}

bool isMechanismName(std::string_view name) {
    const std::string n = lowered(name);
    return contains(n, {"coil", "winding", "magnet", "shaft", "gear", "pinion", "ball", "roller",
                        "needle", "spring", "pcb", "board", "sensor"});
}

bool isShellName(std::string_view name) {
    const std::string n = lowered(name);
    return contains(n, {"housing", "case", "casing", "cover", "shell", "enclosure", "bell", "lid",
                        "shroud", "frame"}) ||
           hasCap(n);
}

std::vector<u8> decideShell(const ShellCounts& counts, std::span<const ShellPart> parts,
                            double minOcclusion) {
    const u32 n = counts.parts;
    std::vector<double> exposure(n, 0.0);
    for (u32 p = 0; p < n; ++p) {
        const double total = counts.front[p] + counts.behind[p];
        exposure[p] = total > 0.0 ? counts.front[p] / total : 0.0;
    }
    // What each part hides of the inner parts, those mostly hidden themselves.
    std::vector<double> innerCover(n, 0.0);
    double innerHidden = 0.0;
    for (const auto& [key, pixels] : counts.pairs) {
        const u32 behindPart = u32(key % n);
        if (exposure[behindPart] < 0.5) {
            innerCover[key / n] += pixels;
            innerHidden += pixels;
        }
    }
    double totalFront = 0.0;
    for (const double f : counts.front) {
        totalFront += f;
    }
    totalFront = std::max(totalFront, 1.0);

    // Whether a part hides an inner part that its bounds enclose.
    std::vector<u8> wraps(n, 0);
    for (const auto& [key, pixels] : counts.pairs) {
        const u32 frontPart = u32(key / n), behindPart = u32(key % n);
        if (pixels > 0.0 && exposure[behindPart] < 0.5 &&
            encloses(parts[frontPart].bounds, parts[behindPart].bounds)) {
            wraps[frontPart] = 1;
        }
    }

    std::vector<u8> shell(n, 0);
    u32 chosen = 0, visible = 0;
    for (u32 p = 0; p < n; ++p) {
        if (counts.front[p] <= 0.0) {
            continue;
        }
        ++visible;
        if (isMechanismName(parts[p].name)) {
            continue;
        }
        const double occlusion = innerCover[p] / counts.front[p];
        const double hiding = innerHidden > 0.0 ? innerCover[p] / innerHidden : 0.0;
        const double share = counts.front[p] / totalFront;
        const bool strict = exposure[p] > 0.5 && occlusion > minOcclusion && share > 0.01 &&
                            hiding >= 0.05 && wraps[p] != 0;
        const bool relaxed =
            isShellName(parts[p].name) && exposure[p] > 0.3 && occlusion > minOcclusion / 2.0;
        if (strict || relaxed) {
            shell[p] = 1;
            ++chosen;
        }
    }
    // Something must stay solid: if every visible part qualified, the test says nothing.
    if (chosen >= visible) {
        std::fill(shell.begin(), shell.end(), u8(0));
    }
    return shell;
}

} // namespace geo::render
