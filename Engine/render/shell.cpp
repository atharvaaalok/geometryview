#include "render/shell.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numeric>
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

namespace {

struct UnionFind {
    explicit UnionFind(usize n) : parent(n) { std::iota(parent.begin(), parent.end(), u32(0)); }
    u32 find(u32 a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    }
    void join(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a != b) {
            parent[std::max(a, b)] = std::min(a, b);
        }
    }
    std::vector<u32> parent;
};

bool overlaps(const Aabb& a, const Aabb& b) {
    return simd_all(a.min <= b.max) && simd_all(b.min <= a.max);
}

} // namespace

std::vector<u32> clusterParts(std::span<const Aabb> boxes) {
    const usize n = boxes.size();
    std::vector<Aabb> grown(n);
    std::vector<double> sizes;
    sizes.reserve(n);
    for (usize i = 0; i < n; ++i) {
        const Vec3 extent = boxes[i].max - boxes[i].min;
        const double grow = 0.01 * simd_length(extent);
        grown[i] = {boxes[i].min - grow, boxes[i].max + grow};
        const double largest = std::max({extent.x, extent.y, extent.z});
        if (std::isfinite(largest) && largest > 0.0) {
            sizes.push_back(largest);
        }
    }
    UnionFind sets(n);
    if (!sizes.empty()) {
        // A grid of cells the size of a typical part, so each box is tested only against boxes in
        // the cells it covers. The few that cover many cells, as a base plate does, are tested
        // against every box instead.
        std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
        const double cell = sizes[sizes.size() / 2];
        std::unordered_map<u64, std::vector<u32>> cells;
        std::vector<u32> large;
        const auto key = [](i64 x, i64 y, i64 z) {
            return (u64(x) & 0x1FFFFF) | (u64(y) & 0x1FFFFF) << 21 | (u64(z) & 0x1FFFFF) << 42;
        };
        for (usize i = 0; i < n; ++i) {
            if (!simd_all(grown[i].min <= grown[i].max)) {
                continue; // empty or not finite
            }
            const auto floored = [&](Vec3 v) {
                return simd_make_double3(std::floor(v.x / cell), std::floor(v.y / cell),
                                         std::floor(v.z / cell));
            };
            const Vec3 lo = floored(grown[i].min), hi = floored(grown[i].max);
            const Vec3 span = hi - lo + 1.0;
            if (span.x * span.y * span.z > 64.0) {
                large.push_back(u32(i));
                continue;
            }
            for (i64 x = i64(lo.x); x <= i64(hi.x); ++x) {
                for (i64 y = i64(lo.y); y <= i64(hi.y); ++y) {
                    for (i64 z = i64(lo.z); z <= i64(hi.z); ++z) {
                        cells[key(x, y, z)].push_back(u32(i));
                    }
                }
            }
        }
        for (const auto& [_, members] : cells) {
            for (usize a = 0; a < members.size(); ++a) {
                for (usize b = a + 1; b < members.size(); ++b) {
                    if (overlaps(grown[members[a]], grown[members[b]])) {
                        sets.join(members[a], members[b]);
                    }
                }
            }
        }
        for (const u32 big : large) {
            for (usize i = 0; i < n; ++i) {
                if (i != big && overlaps(grown[big], grown[i])) {
                    sets.join(big, u32(i));
                }
            }
        }
    }
    std::vector<u32> group(n);
    std::unordered_map<u32, u32> number;
    for (usize i = 0; i < n; ++i) {
        group[i] = number.try_emplace(sets.find(u32(i)), u32(number.size())).first->second;
    }
    return group;
}

std::vector<u32> nestedGroups(std::span<const AssemblyRow> rows, std::span<const u32> groupOf) {
    std::vector<u32> size;
    for (const u32 g : groupOf) {
        if (g >= size.size()) {
            size.resize(usize(g) + 1, 0);
        }
        ++size[g];
    }
    std::vector<u32> out(groupOf.size(), 0);
    u32 count = 0;
    for (usize r = 0; r < rows.size();) {
        const AssemblyRow& row = rows[r];
        bool take = false;
        if (row.assembly && row.first + 2 <= row.end && row.end <= groupOf.size()) {
            const u32 g = groupOf[row.first];
            take = g != 0 && row.end - row.first < size[g] &&
                   std::all_of(
                       groupOf.begin() + row.first, groupOf.begin() + row.end,
                       [&](u32 other) { return other == g; });
        }
        if (!take) {
            ++r;
            continue;
        }
        ++count;
        std::fill(out.begin() + row.first, out.begin() + row.end, count);
        r = std::max<usize>(row.subtreeEnd, r + 1);
    }
    return out;
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
                            double minOcclusion, std::vector<ShellScore>* scores) {
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
            innerCover[usize(key / n)] += pixels;
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
    if (scores != nullptr) {
        scores->assign(n, ShellScore{});
    }
    u32 chosen = 0, visible = 0;
    for (u32 p = 0; p < n; ++p) {
        if (counts.front[p] <= 0.0) {
            continue;
        }
        ++visible;
        const double occlusion = innerCover[p] / counts.front[p];
        const double hiding = innerHidden > 0.0 ? innerCover[p] / innerHidden : 0.0;
        const double share = counts.front[p] / totalFront;
        if (scores != nullptr) {
            (*scores)[p] = {exposure[p], occlusion, share, hiding, wraps[p] != 0, false};
        }
        if (isMechanismName(parts[p].name)) {
            continue;
        }
        const bool strict = exposure[p] > 0.5 && occlusion > minOcclusion && share > 0.01 &&
                            hiding >= 0.05 && wraps[p] != 0;
        const bool relaxed =
            isShellName(parts[p].name) && exposure[p] > 0.3 && occlusion > minOcclusion / 2.0;
        // Or it does a decisive share of all the hiding and wraps what it hides, even where the
        // inner parts are small beside it: a long motor's flange, stator and cap around a shaft and
        // two bearings, or the plate a belt axis's motor hangs from.
        const bool decisive = exposure[p] > 0.5 && share > 0.01 && hiding >= 0.2 && wraps[p] != 0 &&
                              occlusion > minOcclusion / 6.0;
        if (strict || relaxed || decisive) {
            shell[p] = 1;
            ++chosen;
        }
    }
    // Copies of one shape are decided alike, by majority of those seen, a tie fading them: two
    // identical bearings must not differ by a pixel's worth of the threshold.
    std::unordered_map<u64, std::pair<u32, u32>> votes; // shell, seen
    for (u32 p = 0; p < n; ++p) {
        if (parts[p].definition != ~u64(0) && counts.front[p] > 0.0) {
            auto& [yes, seen] = votes[parts[p].definition];
            yes += shell[p];
            ++seen;
        }
    }
    chosen = 0;
    for (u32 p = 0; p < n; ++p) {
        const auto vote = votes.find(parts[p].definition);
        if (vote != votes.end() && vote->second.second > 1) {
            shell[p] = u8(2 * vote->second.first >= vote->second.second);
        }
        chosen += counts.front[p] > 0.0 && shell[p] != 0 ? 1 : 0;
    }
    // Something must stay solid: if every visible part qualified, the test says nothing.
    if (chosen >= visible) {
        std::fill(shell.begin(), shell.end(), u8(0));
    }
    if (scores != nullptr) {
        for (u32 p = 0; p < n; ++p) {
            (*scores)[p].shell = shell[p] != 0;
        }
    }
    return shell;
}

} // namespace geo::render
