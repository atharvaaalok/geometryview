#include "scene/facts.h"

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <format>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace geo::scene {
namespace {

struct Geometry {
    const format::SegmentView* view = nullptr;
    const format::DefinitionRecord* record = nullptr;
};

std::array<double, 3> decodeVertex(const format::SegmentView& view,
                                   const format::DefinitionRecord& d, u32 vertex) {
    const PackedVertex& v = view.vertices[d.firstVertex + vertex];
    std::array<u32, 3> grid = {v.x, v.y, v.z};
    if ((d.flags & kDefinitionWide) != 0) {
        const u32 high = view.highBits[d.firstHighBits + vertex];
        grid[0] |= (high & 0xFF) << 16;
        grid[1] |= ((high >> 8) & 0xFF) << 16;
        grid[2] |= ((high >> 16) & 0xFF) << 16;
    }
    return {double(float(grid[0]) * d.decodeScale[0] + d.boundsMin[0]),
            double(float(grid[1]) * d.decodeScale[1] + d.boundsMin[1]),
            double(float(grid[2]) * d.decodeScale[2] + d.boundsMin[2])};
}

std::array<double, 3> place(const std::array<float, 12>& t, const std::array<double, 3>& p) {
    return {t[0] * p[0] + t[3] * p[1] + t[6] * p[2] + t[9],
            t[1] * p[0] + t[4] * p[1] + t[7] * p[2] + t[10],
            t[2] * p[0] + t[5] * p[1] + t[8] * p[2] + t[11]};
}

// Whether a placement only swaps and flips axes, so it carries a box exactly onto a box.
bool keepsBoxes(const std::array<float, 12>& t) {
    for (int column = 0; column < 3; ++column) {
        int nonzero = 0;
        for (int row = 0; row < 3; ++row) {
            nonzero += t[column * 3 + row] != 0.0f ? 1 : 0;
        }
        if (nonzero != 1) {
            return false;
        }
    }
    return true;
}

struct Box {
    std::array<double, 3> min{INFINITY, INFINITY, INFINITY};
    std::array<double, 3> max{-INFINITY, -INFINITY, -INFINITY};

    void add(const std::array<double, 3>& p) {
        for (int axis = 0; axis < 3; ++axis) {
            min[axis] = std::min(min[axis], p[axis]);
            max[axis] = std::max(max[axis], p[axis]);
        }
    }
    void addCorners(const std::array<float, 12>& t, const std::array<float, 3>& lo,
                    const std::array<float, 3>& hi) {
        for (int corner = 0; corner < 8; ++corner) {
            add(place(t, {(corner & 1) != 0 ? hi[0] : lo[0], (corner & 2) != 0 ? hi[1] : lo[1],
                          (corner & 4) != 0 ? hi[2] : lo[2]}));
        }
    }
    bool empty() const { return !(min[0] <= max[0]); }
};

std::string_view nameOf(const format::SegmentView& model, u32 node) {
    const format::NodeRecord& record = model.nodes[node];
    return {model.strings.data() + record.nameOffset, record.nameLength};
}

json::Value triple(const std::array<double, 3>& v) {
    json::Value array = json::Value::array();
    for (const double x : v) {
        array.push(x);
    }
    return array;
}

bool readTriple(const json::Value& value, std::array<double, 3>& v) {
    if (!value.isArray() || value.size() != 3) {
        return false;
    }
    for (usize i = 0; i < 3; ++i) {
        if (!value[i].isNumber()) {
            return false;
        }
        v[i] = value[i].asNumber();
    }
    return true;
}

} // namespace

Facts measure(std::span<const std::shared_ptr<Segment>> segments) {
    Facts facts;
    const format::SegmentView* model = nullptr;
    for (const std::shared_ptr<Segment>& segment : segments) {
        if (segment->view().hasModel()) {
            model = &segment->view();
        }
    }
    if (model == nullptr) {
        return facts;
    }

    // The finest geometry of each definition, as a viewer would draw it last.
    std::vector<Geometry> geometry(model->definitionInfos.size());
    for (const std::shared_ptr<Segment>& segment : segments) {
        for (const format::DefinitionRecord& record : segment->view().definitions) {
            if (record.definition >= geometry.size()) {
                continue;
            }
            Geometry& g = geometry[record.definition];
            if (g.record == nullptr || record.level > g.record->level) {
                g = {&segment->view(), &record};
            }
        }
    }

    Box box;
    bool estimated = false;
    std::unordered_set<u32> parts;
    for (const format::OccurrenceRecord& occurrence : model->occurrences) {
        const format::DefinitionInfo& info = model->definitionInfos[occurrence.definition];
        facts.faces += info.faceCount;
        // The part a body belongs to, as the colour cycle counts it.
        u32 part = occurrence.node;
        if (part < model->nodes.size()) {
            const format::NodeRecord& node = model->nodes[part];
            if (node.kind == u32(format::NodeKind::Body) && node.parent != format::kNoParent) {
                part = node.parent;
            }
        }
        parts.insert(part);

        const Geometry& g = geometry[occurrence.definition];
        if (g.record == nullptr) {
            estimated = true;
            if (info.boundsMin[0] <= info.boundsMax[0]) {
                box.addCorners(occurrence.transform, info.boundsMin, info.boundsMax);
            }
            continue;
        }
        const format::DefinitionRecord& d = *g.record;
        facts.triangles += d.triangleCount;
        if (d.vertexCount == 0) {
            continue;
        }
        if (keepsBoxes(occurrence.transform)) {
            box.addCorners(occurrence.transform, d.boundsMin, d.boundsMax);
        } else {
            for (u32 v = 0; v < d.vertexCount; ++v) {
                box.add(place(occurrence.transform, decodeVertex(*g.view, d, v)));
            }
        }
    }

    facts.estimated = estimated;
    facts.hasBox = !box.empty();
    if (facts.hasBox) {
        facts.min = box.min;
        facts.max = box.max;
    }
    facts.parts = u32(parts.size());
    facts.bodies = u32(model->occurrences.size());
    facts.definitions = u32(model->definitionInfos.size());

    // The top product's name, or else the first part with one.
    for (u32 n = 0; n < model->nodes.size() && facts.name.empty(); ++n) {
        const format::NodeRecord& node = model->nodes[n];
        if (n == 0 || node.kind == u32(format::NodeKind::Part)) {
            facts.name = std::string(nameOf(*model, n));
        }
    }
    return facts;
}

bool measureCacheFile(const std::string& path, Facts& facts, std::string& error) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = std::format("Could not open {}.", path);
        return false;
    }
    format::Header header{};
    const bool read = pread(fd, &header, sizeof(header), 0) == ssize_t(sizeof(header));
    std::vector<std::shared_ptr<Segment>> segments;
    const bool opened = read && openCacheFile(fd, segments, error);
    close(fd);
    if (!opened) {
        if (error.empty()) {
            error = std::format("Could not read {}.", path);
        }
        return false;
    }
    facts = measure(segments);
    facts.hash.clear();
    for (const u8 byte : header.sourceHash) {
        facts.hash += std::format("{:02x}", byte);
    }
    return true;
}

json::Value Facts::toJson() const {
    json::Value value = json::Value::object();
    value.set("version", kVersion);
    if (hasBox) {
        value.set("min", triple(min));
        value.set("max", triple(max));
    }
    value.set("estimated", estimated);
    value.set("triangles", triangles);
    value.set("faces", faces);
    value.set("parts", parts);
    value.set("bodies", bodies);
    value.set("definitions", definitions);
    value.set("name", name);
    value.set("hash", hash);
    return value;
}

bool Facts::fromJson(const json::Value& value, Facts& facts) {
    if (value["version"].asNumber() != kVersion) {
        return false;
    }
    Facts read;
    read.hasBox = value["min"].isArray();
    if (read.hasBox &&
        (!readTriple(value["min"], read.min) || !readTriple(value["max"], read.max))) {
        return false;
    }
    read.estimated = value["estimated"].asBool(true);
    read.triangles = u64(value["triangles"].asNumber());
    read.faces = u64(value["faces"].asNumber());
    read.parts = u32(value["parts"].asNumber());
    read.bodies = u32(value["bodies"].asNumber());
    read.definitions = u32(value["definitions"].asNumber());
    read.name = value["name"].asString();
    read.hash = value["hash"].asString();
    facts = std::move(read);
    return true;
}

} // namespace geo::scene
