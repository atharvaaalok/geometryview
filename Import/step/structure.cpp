#include "step/structure.h"

#include "base/parallel.h"
#include "base/parallel_sort.h"
#include "base/trace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace geo::import::step {
namespace {

constexpr int kMaxDepth = 256;
constexpr int kMaxStyleDepth = 10;

// A one-to-many relation built once and then only searched.
class Relation {
public:
    void add(u32 key, u32 value) { pairs_.push_back(u64(key) << 32 | value); }
    // Sorting whole pairs keeps each key's values in file order, as a stable sort by key would.
    void finish() {
        parallelSort(pairs_, [](u64 pair) { return pair; });
    }

    std::span<const u64> all() const { return pairs_; }
    /// Where a pair from `find` sits in `all`, so callers can keep data beside each pair.
    usize indexOf(const u64& pair) const { return usize(&pair - pairs_.data()); }

    std::span<const u64> find(u32 key) const {
        const u64 probe = u64(key) << 32;
        const auto range = std::equal_range(pairs_.begin(), pairs_.end(), probe, compareKeys);
        return {range.first, range.second};
    }

private:
    static bool compareKeys(u64 a, u64 b) { return (a >> 32) < (b >> 32); }
    std::vector<u64> pairs_;
};

u32 valueOf(u64 pair) {
    return u32(pair);
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

// Names writers generate, which say nothing about the part.
bool isPlaceholderName(std::string_view name) {
    if (name.empty() || name.starts_with("=>[")) {
        return true;
    }
    std::string upper(name);
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](char c) { return char(c >= 'a' && c <= 'z' ? c - 32 : c); });
    while (!upper.empty() && (isDigit(upper.back()) || upper.back() == ' ')) {
        upper.pop_back();
    }
    static constexpr std::array<std::string_view, 12> kPlaceholders = {
        "",      "NONE",     "NAUO",      "UNKNOWN", "UNNAMED", "SOLID",
        "SHELL", "COMPOUND", "COMPSOLID", "FACE",    "BODY",    "PART"};
    return std::find(kPlaceholders.begin(), kPlaceholders.end(), upper) != kPlaceholders.end();
}

bool isRepresentation(Kind kind) {
    switch (kind) {
    case Kind::ADVANCED_BREP_SHAPE_REPRESENTATION:
    case Kind::EDGE_BASED_WIREFRAME_SHAPE_REPRESENTATION:
    case Kind::FACETED_BREP_SHAPE_REPRESENTATION:
    case Kind::GEOMETRICALLY_BOUNDED_SURFACE_SHAPE_REPRESENTATION:
    case Kind::GEOMETRICALLY_BOUNDED_WIREFRAME_SHAPE_REPRESENTATION:
    case Kind::MANIFOLD_SURFACE_SHAPE_REPRESENTATION:
    case Kind::REPRESENTATION:
    case Kind::SHAPE_REPRESENTATION:
    case Kind::TESSELLATED_SHAPE_REPRESENTATION:
        return true;
    default:
        return false;
    }
}

bool isBody(Kind kind) {
    switch (kind) {
    case Kind::BREP_WITH_VOIDS:
    case Kind::CLOSED_SHELL:
    case Kind::FACE_BASED_SURFACE_MODEL:
    case Kind::FACETED_BREP:
    case Kind::FACETED_BREP_AND_BREP_WITH_VOIDS:
    case Kind::MANIFOLD_SOLID_BREP:
    case Kind::OPEN_SHELL:
    case Kind::SHELL_BASED_SURFACE_MODEL:
        return true;
    default:
        return false;
    }
}

bool isUnslicedGeometry(Kind kind) {
    switch (kind) {
    case Kind::ADVANCED_FACE:
    case Kind::COMPLEX_TRIANGULATED_FACE:
    case Kind::COMPLEX_TRIANGULATED_SURFACE_SET:
    case Kind::FACE_SURFACE:
    case Kind::TESSELLATED_FACE_SET:
    case Kind::TESSELLATED_GEOMETRIC_SET:
    case Kind::TESSELLATED_SHELL:
    case Kind::TESSELLATED_SOLID:
    case Kind::TRIANGULATED_FACE:
    case Kind::TRIANGULATED_SURFACE_SET:
        return true;
    default:
        return false;
    }
}

bool isProductDefinition(Kind kind) {
    return kind == Kind::PRODUCT_DEFINITION ||
           kind == Kind::PRODUCT_DEFINITION_WITH_ASSOCIATED_DOCUMENTS;
}

class StyleTable {
public:
    explicit StyleTable(const Part21& index) : index_(index) {
        std::vector<u32> styled, overriding;
        for (u32 r = 0; r < index.recordCount(); ++r) {
            if (index.kinds[r] == Kind::STYLED_ITEM) {
                styled.push_back(r);
            } else if (index.kinds[r] == Kind::OVER_RIDING_STYLED_ITEM) {
                overriding.push_back(r);
            }
        }
        // Overrides are applied last, so they win.
        for (const u32 record : styled) {
            apply(record, 1);
        }
        for (const u32 record : overriding) {
            apply(record, 2);
        }
    }

    u32 colorOf(u32 record) const {
        const auto it = colors_.find(record);
        return it != colors_.end() ? it->second : kNoColor;
    }

private:
    // STYLED_ITEM(name, (styles), item), and OVER_RIDING_STYLED_ITEM adds the style it replaces.
    void apply(u32 record, usize trailing) {
        const std::span<const u32> refs = index_.refsOf(record);
        if (refs.size() < trailing + 1) {
            return;
        }
        const u32 item = refs[refs.size() - trailing];
        for (usize i = 0; i + trailing < refs.size(); ++i) {
            const u32 color = findColor(refs[i], 0);
            if (color != kNoColor && item != kNoRecord) {
                colors_[item] = color;
                return;
            }
        }
    }

    u32 findColor(u32 record, int depth) const {
        if (record == kNoRecord || depth > kMaxStyleDepth) {
            return kNoColor;
        }
        switch (index_.kinds[record]) {
        case Kind::COLOUR_RGB:
            return readRgb(record);
        case Kind::DRAUGHTING_PRE_DEFINED_COLOUR:
            return readNamedColor(record);
        case Kind::FILL_AREA_STYLE:
        case Kind::FILL_AREA_STYLE_COLOUR:
        case Kind::PRESENTATION_STYLE_ASSIGNMENT:
        case Kind::PRESENTATION_STYLE_BY_CONTEXT:
        case Kind::SURFACE_SIDE_STYLE:
        case Kind::SURFACE_STYLE_FILL_AREA:
        case Kind::SURFACE_STYLE_RENDERING:
        case Kind::SURFACE_STYLE_RENDERING_WITH_PROPERTIES:
        case Kind::SURFACE_STYLE_USAGE:
            for (const u32 next : index_.refsOf(record)) {
                if (const u32 color = findColor(next, depth + 1); color != kNoColor) {
                    return color;
                }
            }
            return kNoColor;
        default:
            return kNoColor;
        }
    }

    static u32 packColor(double r, double g, double b) {
        const auto channel = [](double value) {
            return u32(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
        };
        return channel(r) | channel(g) << 8 | channel(b) << 16 | 0xFFu << 24;
    }

    u32 readRgb(u32 record) const {
        ParamCursor cursor(index_.text(record));
        double r = 0, g = 0, b = 0;
        if (cursor.skip() && cursor.readReal(r) && cursor.readReal(g) && cursor.readReal(b)) {
            return packColor(r, g, b);
        }
        return kNoColor;
    }

    u32 readNamedColor(u32 record) const {
        ParamCursor cursor(index_.text(record));
        std::string name;
        if (!cursor.readString(name)) {
            return kNoColor;
        }
        static constexpr std::array<std::pair<std::string_view, std::array<double, 3>>, 8> kNames =
            {{{"red", {1, 0, 0}},
              {"green", {0, 1, 0}},
              {"blue", {0, 0, 1}},
              {"yellow", {1, 1, 0}},
              {"magenta", {1, 0, 1}},
              {"cyan", {0, 1, 1}},
              {"black", {0, 0, 0}},
              {"white", {1, 1, 1}}}};
        for (const auto& [text, rgb] : kNames) {
            if (name == text) {
                return packColor(rgb[0], rgb[1], rgb[2]);
            }
        }
        return kNoColor;
    }

    const Part21& index_;
    std::unordered_map<u32, u32> colors_;
};

class StructureReader {
public:
    explicit StructureReader(const Part21& index) : index_(index), styles_(index) {}

    Structure read() {
        StageTrace trace("structure");
        collect();
        trace.lap("collect");
        computePlacements();
        trace.lap("placements");
        std::vector<u32> rootDefinitions;
        for (const u32 definition : productDefinitions_) {
            if (!isChild_[definition]) {
                rootDefinitions.push_back(definition);
            }
        }
        if (!productDefinitions_.empty() && rootDefinitions.empty()) {
            rootDefinitions.push_back(productDefinitions_.front()); // every product is in a cycle
        }
        for (const u32 definition : rootDefinitions) {
            walkProduct(definition, Affine{}, kNoRecord, 0);
        }
        if (out_.nodes.empty()) {
            walkLooseRepresentations();
        }
        trace.lap("walk");
        collapseSingleBodies();
        trace.lap("collapse");
        return std::move(out_);
    }

private:
    void collect() {
        isChild_.assign(index_.recordCount(), false);
        placementRelationships_.assign(index_.recordCount(), false);
        for (u32 r = 0; r < index_.recordCount(); ++r) {
            const Kind kind = index_.kinds[r];
            const std::span<const u32> refs = index_.refsOf(r);
            if (isProductDefinition(kind)) {
                productDefinitions_.push_back(r);
            } else if (kind == Kind::NEXT_ASSEMBLY_USAGE_OCCURRENCE && refs.size() >= 2 &&
                       refs[0] != kNoRecord && refs[1] != kNoRecord) {
                usagesOfParent_.add(refs[0], r);
                isChild_[refs[1]] = true;
            } else if (kind == Kind::PRODUCT_DEFINITION_SHAPE && !refs.empty() &&
                       refs[0] != kNoRecord) {
                shapesOfDefinition_.add(refs[0], r);
            } else if (kind == Kind::SHAPE_DEFINITION_REPRESENTATION && refs.size() >= 2 &&
                       refs[0] != kNoRecord && refs[1] != kNoRecord) {
                representationsOfShape_.add(refs[0], refs[1]);
                ownerOfRepresentation_.add(refs[1], refs[0]);
            } else if (kind == Kind::CONTEXT_DEPENDENT_SHAPE_REPRESENTATION && refs.size() >= 2 &&
                       refs[0] != kNoRecord && refs[1] != kNoRecord) {
                relationshipsOfShape_.add(refs[1], refs[0]);
                placementRelationships_[refs[0]] = true;
            } else if (isRepresentation(kind)) {
                representations_.push_back(r);
            }
        }
        // Only plain relationships link one product's representations, as hybrid models do. The
        // others position a component, which CDSRs and usages already describe.
        for (u32 r = 0; r < index_.recordCount(); ++r) {
            if (index_.kinds[r] == Kind::SHAPE_REPRESENTATION_RELATIONSHIP &&
                !placementRelationships_[r]) {
                const std::span<const u32> refs = index_.refsOf(r);
                if (refs.size() >= 2 && refs[0] != kNoRecord && refs[1] != kNoRecord) {
                    linkedRepresentations_.add(refs[0], refs[1]);
                    linkedRepresentations_.add(refs[1], refs[0]);
                }
            }
        }
        usagesOfParent_.finish();
        shapesOfDefinition_.finish();
        representationsOfShape_.finish();
        ownerOfRepresentation_.finish();
        relationshipsOfShape_.finish();
        linkedRepresentations_.finish();
    }

    // Every usage's placement, read in parallel before the walk. Reading placements means parsing
    // text, and in a large assembly that is most of the work.
    void computePlacements() {
        // Units are read once up front, so the parallel reads below only look them up.
        for (u32 r = 0; r < index_.recordCount(); ++r) {
            if (index_.kinds[r] == Kind::GEOMETRIC_REPRESENTATION_CONTEXT) {
                lengthFactors_.emplace(r, lengthFactor(index_, r));
            }
        }
        const std::span<const u64> usages = usagesOfParent_.all();
        usagePlacements_.resize(usages.size());
        parallelRanges(usages.size(), 1024, [&](usize first, usize last, usize) {
            for (usize i = first; i < last; ++i) {
                const u32 parent = u32(usages[i] >> 32);
                const u32 usage = valueOf(usages[i]);
                usagePlacements_[i] = placementOf(usage, parent, index_.refsOf(usage)[1]);
            }
        });
    }

    std::string productName(u32 definition) const {
        // PRODUCT_DEFINITION -> FORMATION -> PRODUCT.
        for (const u32 formation : index_.refsOf(definition)) {
            if (formation == kNoRecord) {
                continue;
            }
            const Kind kind = index_.kinds[formation];
            if (kind != Kind::PRODUCT_DEFINITION_FORMATION &&
                kind != Kind::PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE) {
                continue;
            }
            for (const u32 product : index_.refsOf(formation)) {
                if (product != kNoRecord && index_.kinds[product] == Kind::PRODUCT) {
                    ParamCursor cursor(index_.text(product));
                    std::string id, name;
                    cursor.readString(id);
                    cursor.readString(name);
                    return isPlaceholderName(name) ? id : name;
                }
            }
        }
        return {};
    }

    u32 addNode(NodeKind kind, std::string_view name, u32 parent) {
        const u32 node = u32(out_.nodes.size());
        TreeNode row;
        row.parent = parent;
        row.kind = kind;
        row.firstOccurrence = u32(out_.occurrences.size());
        row.nameOffset = u32(out_.names.size());
        row.nameLength = u32(name.size());
        out_.names.append(name);
        out_.nodes.push_back(row);
        return node;
    }

    void closeNode(u32 node) {
        out_.nodes[node].subtreeEnd = u32(out_.nodes.size());
        out_.nodes[node].occurrenceEnd = u32(out_.occurrences.size());
    }

    void walkProduct(u32 definition, const Affine& world, u32 parent, int depth) {
        // The path is as deep as the tree, so a scan of it beats a hash set.
        if (depth > kMaxDepth ||
            std::find(onPath_.begin(), onPath_.end(), definition) != onPath_.end()) {
            out_.warnings.push_back("A product contains itself, so the repeat was skipped.");
            return;
        }
        onPath_.push_back(definition);
        auto [name, inserted] = productNames_.try_emplace(definition);
        if (inserted) {
            name->second = productName(definition);
        }
        const u32 node = addNode(NodeKind::Assembly, name->second, parent);

        for (const u64 shapePair : shapesOfDefinition_.find(definition)) {
            for (const u64 repPair : representationsOfShape_.find(valueOf(shapePair))) {
                visitedRepresentations_.clear();
                walkRepresentation(valueOf(repPair), world, node, depth);
            }
        }
        bool hasComponents = false;
        for (const u64& usagePair : usagesOfParent_.find(definition)) {
            const u32 usage = valueOf(usagePair);
            const u32 child = index_.refsOf(usage)[1];
            if (!isProductDefinition(index_.kinds[child])) {
                continue;
            }
            const Affine placed =
                compose(world, usagePlacements_[usagesOfParent_.indexOf(usagePair)]);
            walkProduct(child, placed, node, depth + 1);
            hasComponents = true;
        }
        out_.nodes[node].kind = hasComponents ? NodeKind::Assembly : NodeKind::Part;
        closeNode(node);
        onPath_.pop_back();
    }

    bool representationBelongsTo(u32 representation, u32 definition) const {
        for (const u64 pair : ownerOfRepresentation_.find(representation)) {
            const std::span<const u32> refs = index_.refsOf(valueOf(pair));
            if (!refs.empty() && refs[0] == definition) {
                return true;
            }
        }
        return false;
    }

    // Where a child sits in its parent, from the CDSR attached to the usage.
    Affine placementOf(u32 usage, u32 parent, u32 child) const {
        for (const u64 shapePair : shapesOfDefinition_.find(usage)) {
            for (const u64 relationPair : relationshipsOfShape_.find(valueOf(shapePair))) {
                const u32 relation = valueOf(relationPair);
                const std::span<const u32> refs = index_.refsOf(relation);
                if (refs.size() < 3) {
                    continue;
                }
                const u32 rep1 = refs[0], rep2 = refs[1];
                Affine transform = relationTransform(relation, rep1, rep2);
                // The standard has rep1 as the child, but not every writer agrees.
                const bool reversed = (representationBelongsTo(rep2, child) &&
                                       !representationBelongsTo(rep1, child)) ||
                                      (representationBelongsTo(rep1, parent) &&
                                       !representationBelongsTo(rep2, parent));
                return reversed ? inverse(transform) : transform;
            }
        }
        return Affine{};
    }

    Affine relationTransform(u32 relation, u32 rep1, u32 rep2) const {
        const u32 item = index_.refsOf(relation)[2];
        if (item == kNoRecord) {
            return Affine{};
        }
        if (index_.kinds[item] == Kind::CARTESIAN_TRANSFORMATION_OPERATOR_3D) {
            return operatorTransform(item, rep2);
        }
        if (index_.kinds[item] != Kind::ITEM_DEFINED_TRANSFORMATION) {
            return Affine{};
        }
        const std::span<const u32> refs = index_.refsOf(item);
        if (refs.size() < 2 || refs[0] == kNoRecord || refs[1] == kNoRecord) {
            return Affine{};
        }
        u32 origin = refs[0], target = refs[1];
        // Some writers swap the two placements. Each must belong to its own representation.
        if (belongsToItems(origin, rep2) && belongsToItems(target, rep1) &&
            !belongsToItems(origin, rep1)) {
            std::swap(origin, target);
        }
        return mappingBetween(origin, rep1, target, rep2);
    }

    bool belongsToItems(u32 item, u32 representation) const {
        if (representation == kNoRecord) {
            return false;
        }
        const std::span<const u32> refs = index_.refsOf(representation);
        return std::find(refs.begin(), refs.end(), item) != refs.end();
    }

    u32 contextOf(u32 representation) const {
        const std::span<const u32> refs = index_.refsOf(representation);
        return refs.empty() ? kNoRecord : refs.back();
    }

    // The rigid motion that carries `origin` onto `target`, each in its own units.
    Affine mappingBetween(u32 origin, u32 originRep, u32 target, u32 targetRep) const {
        const Affine from = readPlacement(origin, lengthFactorOf(contextOf(originRep)));
        const Affine to = readPlacement(target, lengthFactorOf(contextOf(targetRep)));
        return compose(to, inverse(from));
    }

    // Every context's factor is read before anything runs in parallel, so this only looks up.
    double lengthFactorOf(u32 context) const {
        if (context == kNoRecord) {
            return 1.0;
        }
        const auto it = lengthFactors_.find(context);
        return it != lengthFactors_.end() ? it->second : lengthFactor(index_, context);
    }

    bool readVector(u32 record, simd_double3& value) const {
        if (record == kNoRecord) {
            return false;
        }
        ParamCursor cursor(index_.text(record));
        if (!cursor.skip() || !cursor.enterList()) {
            return false;
        }
        double xyz[3] = {0, 0, 0};
        for (double& component : xyz) {
            if (!cursor.inList()) {
                break; // two-dimensional data has no z
            }
            if (!cursor.readReal(component)) {
                return false;
            }
        }
        value = simd_make_double3(xyz[0], xyz[1], xyz[2]);
        return true;
    }

    // AXIS2_PLACEMENT_3D(name, location, axis, ref_direction), where the last two are optional.
    Affine readPlacement(u32 record, double unit) const {
        Affine placement;
        if (index_.kinds[record] != Kind::AXIS2_PLACEMENT_3D) {
            return placement;
        }
        ParamCursor cursor(index_.text(record));
        u32 location = 0, axisId = 0, referenceId = 0;
        if (!cursor.skip() || !cursor.readId(location)) {
            return placement;
        }
        const bool hasAxis = cursor.readId(axisId);
        if (!hasAxis) {
            cursor.skip();
        }
        const bool hasReference = cursor.readId(referenceId);

        const std::span<const u32> refs = index_.refsOf(record);
        simd_double3 origin = {0, 0, 0}, z = {0, 0, 1}, x = {1, 0, 0};
        if (!refs.empty()) {
            readVector(refs[0], origin);
        }
        usize next = 1;
        if (hasAxis && next < refs.size()) {
            readVector(refs[next++], z);
        }
        if (hasReference && next < refs.size()) {
            readVector(refs[next], x);
        }
        z = simd_normalize(z);
        x = x - z * simd_dot(x, z);
        if (simd_length(x) < 1e-12) {
            // The reference direction is unusable, so any perpendicular will do.
            x = std::fabs(z.x) < 0.9 ? simd_make_double3(1, 0, 0) : simd_make_double3(0, 1, 0);
            x = x - z * simd_dot(x, z);
        }
        x = simd_normalize(x);
        placement.linear = simd_matrix(x, simd_cross(z, x), z);
        placement.translation = origin * unit;
        return placement;
    }

    // CARTESIAN_TRANSFORMATION_OPERATOR_3D(name, description, axis1, axis2, origin, scale, axis3).
    Affine operatorTransform(u32 record, u32 representation) const {
        Affine transform;
        ParamCursor cursor(index_.text(record));
        u32 axis1 = 0, axis2 = 0, origin = 0, axis3 = 0;
        double scale = 1.0;
        if (!cursor.skip() || !cursor.skip()) {
            return transform;
        }
        const bool has1 = cursor.readId(axis1);
        if (!has1) {
            cursor.skip();
        }
        const bool has2 = cursor.readId(axis2);
        if (!has2) {
            cursor.skip();
        }
        if (!cursor.readId(origin)) {
            return transform;
        }
        if (!cursor.readReal(scale)) {
            cursor.skip();
        }
        const bool has3 = cursor.readId(axis3);

        const std::span<const u32> refs = index_.refsOf(record);
        usize next = 0;
        simd_double3 x = {1, 0, 0}, y = {0, 1, 0}, z = {0, 0, 0}, position = {0, 0, 0};
        if (has1 && next < refs.size()) {
            readVector(refs[next++], x);
        }
        if (has2 && next < refs.size()) {
            readVector(refs[next++], y);
        }
        if (next < refs.size()) {
            readVector(refs[next++], position);
        }
        x = simd_normalize(x);
        y = simd_normalize(y - x * simd_dot(y, x));
        z = simd_cross(x, y);
        if (has3 && next < refs.size()) {
            readVector(refs[next], z);
            z = simd_normalize(z);
        }
        transform.linear = simd_matrix(x * scale, y * scale, z * scale);
        transform.translation = position * lengthFactorOf(contextOf(representation));
        return transform;
    }

    Affine mappedItemTransform(u32 item, u32 mapping, u32 mappedRep, u32 representation) const {
        const std::span<const u32> refs = index_.refsOf(item);
        if (refs.size() < 2 || refs[1] == kNoRecord) {
            return Affine{};
        }
        const u32 target = refs[1];
        if (index_.kinds[target] == Kind::CARTESIAN_TRANSFORMATION_OPERATOR_3D) {
            return operatorTransform(target, representation);
        }
        const u32 origin = index_.refsOf(mapping)[0];
        if (origin == kNoRecord) {
            return Affine{};
        }
        return mappingBetween(origin, mappedRep, target, representation);
    }

    void walkRepresentation(u32 representation, const Affine& world, u32 node, int depth) {
        if (representation == kNoRecord || depth > kMaxDepth ||
            std::find(visitedRepresentations_.begin(), visitedRepresentations_.end(),
                      representation) != visitedRepresentations_.end()) {
            return;
        }
        visitedRepresentations_.push_back(representation);
        const std::span<const u32> items = index_.refsOf(representation);
        const u32 context = contextOf(representation);
        for (usize i = 0; i + 1 < items.size(); ++i) {
            const u32 item = items[i];
            if (item == kNoRecord) {
                continue;
            }
            const Kind kind = index_.kinds[item];
            if (isBody(kind)) {
                addOccurrence(item, context, world, node);
            } else if (kind == Kind::MAPPED_ITEM) {
                const std::span<const u32> mapped = index_.refsOf(item);
                if (mapped.empty() || mapped[0] == kNoRecord) {
                    continue;
                }
                const u32 mapping = mapped[0];
                const std::span<const u32> map = index_.refsOf(mapping);
                if (index_.kinds[mapping] != Kind::REPRESENTATION_MAP || map.size() < 2 ||
                    map[1] == kNoRecord) {
                    continue;
                }
                const Affine placed =
                    compose(world, mappedItemTransform(item, mapping, map[1], representation));
                // A mapped representation can be reached again with another placement.
                std::vector<u32> outer;
                std::swap(outer, visitedRepresentations_);
                walkRepresentation(map[1], placed, node, depth + 1);
                std::swap(outer, visitedRepresentations_);
            } else if (isUnslicedGeometry(kind)) {
                out_.needsWholeFile = true;
            }
        }
        for (const u64 pair : linkedRepresentations_.find(representation)) {
            walkRepresentation(valueOf(pair), world, node, depth + 1);
        }
    }

    void addOccurrence(u32 bodyRecord, u32 context, const Affine& world, u32 parent) {
        BodyOccurrence occurrence;
        occurrence.body = internBody(bodyRecord, context);
        occurrence.world = world;
        occurrence.node = addBodyNode(bodyRecord, parent);
        out_.occurrences.push_back(occurrence);
        closeNode(occurrence.node);
    }

    u32 addBodyNode(u32 bodyRecord, u32 parent) {
        auto [name, inserted] = bodyNames_.try_emplace(bodyRecord);
        if (inserted) {
            ParamCursor cursor(index_.text(bodyRecord));
            cursor.readString(name->second);
            if (isPlaceholderName(name->second)) {
                name->second.clear();
            }
        }
        return addNode(NodeKind::Body, name->second, parent);
    }

    u32 internBody(u32 record, u32 context) {
        const auto [it, inserted] =
            bodyOfRecord_.try_emplace(u64(record) << 32 | context, u32(out_.bodies.size()));
        if (!inserted) {
            return it->second;
        }
        Body body;
        body.record = record;
        body.context = context;
        body.firstFace = u32(out_.faces.size());
        body.color = styles_.colorOf(record);
        collectFaces(record, body);
        body.faceCount = u32(out_.faces.size()) - body.firstFace;
        out_.bodies.push_back(body);
        return it->second;
    }

    bool isReversed(u32 oriented) const {
        ParamCursor cursor(index_.text(oriented));
        std::string_view flag;
        // ORIENTED_x(name, *, element, orientation): the name, the derived star, the element.
        for (int i = 0; i < 3; ++i) {
            if (!cursor.skip()) {
                return false;
            }
        }
        return cursor.readEnum(flag) && flag == "F";
    }

    void addShell(u32 shell, bool flipped, Body& body, bool outer) {
        if (shell == kNoRecord) {
            return;
        }
        const Kind kind = index_.kinds[shell];
        if (kind == Kind::ORIENTED_CLOSED_SHELL || kind == Kind::ORIENTED_OPEN_SHELL) {
            const std::span<const u32> refs = index_.refsOf(shell);
            if (!refs.empty()) {
                addShell(refs[0], flipped != isReversed(shell), body, outer);
            }
            return;
        }
        if (kind == Kind::CLOSED_SHELL && outer) {
            body.closed = true;
        }
        if (kind != Kind::CLOSED_SHELL && kind != Kind::OPEN_SHELL &&
            kind != Kind::CONNECTED_FACE_SET) {
            out_.needsWholeFile = true;
            return;
        }
        const u16 shellIndex = body.shellCount++;
        for (const u32 face : index_.refsOf(shell)) {
            addFace(face, flipped, shellIndex);
        }
    }

    void addFace(u32 face, bool flipped, u16 shell) {
        if (face == kNoRecord) {
            return;
        }
        const Kind kind = index_.kinds[face];
        if (kind == Kind::ORIENTED_FACE) {
            const std::span<const u32> refs = index_.refsOf(face);
            if (!refs.empty()) {
                addFace(refs[0], flipped != isReversed(face), shell);
            }
            return;
        }
        if (kind != Kind::ADVANCED_FACE && kind != Kind::FACE_SURFACE) {
            out_.needsWholeFile = true;
            return;
        }
        out_.faces.push_back({face, styles_.colorOf(face), shell, flipped});
    }

    void collectFaces(u32 record, Body& body) {
        const Kind kind = index_.kinds[record];
        const std::span<const u32> refs = index_.refsOf(record);
        switch (kind) {
        case Kind::MANIFOLD_SOLID_BREP:
        case Kind::FACETED_BREP:
            if (!refs.empty()) {
                addShell(refs[0], false, body, true);
            }
            break;
        case Kind::BREP_WITH_VOIDS:
        case Kind::FACETED_BREP_AND_BREP_WITH_VOIDS:
            body.hasVoids = refs.size() > 1;
            for (usize i = 0; i < refs.size(); ++i) {
                addShell(refs[i], false, body, i == 0);
            }
            break;
        case Kind::SHELL_BASED_SURFACE_MODEL:
        case Kind::FACE_BASED_SURFACE_MODEL:
            for (const u32 shell : refs) {
                addShell(shell, false, body, true);
            }
            break;
        case Kind::CLOSED_SHELL:
        case Kind::OPEN_SHELL:
            addShell(record, false, body, true);
            break;
        default:
            break;
        }
    }

    // A part with one body needs no row of its own for that body.
    void collapseSingleBodies() {
        std::vector<u32> childCount(out_.nodes.size(), 0);
        for (const TreeNode& node : out_.nodes) {
            if (node.parent != kNoRecord) {
                ++childCount[node.parent];
            }
        }
        std::vector<u32> newIndex(out_.nodes.size());
        std::vector<TreeNode> kept;
        kept.reserve(out_.nodes.size());
        for (u32 n = 0; n < out_.nodes.size(); ++n) {
            const TreeNode& node = out_.nodes[n];
            const bool collapse = node.kind == NodeKind::Body && node.parent != kNoRecord &&
                                  childCount[node.parent] == 1 &&
                                  out_.nodes[node.parent].kind == NodeKind::Part;
            if (collapse) {
                newIndex[n] = newIndex[node.parent];
                continue;
            }
            newIndex[n] = u32(kept.size());
            kept.push_back(node);
            if (node.parent != kNoRecord) {
                kept.back().parent = newIndex[node.parent];
            }
        }
        // A node's subtree ends where the next kept node at or above its level starts.
        for (u32 n = 0; n < out_.nodes.size(); ++n) {
            kept[newIndex[n]].subtreeEnd = out_.nodes[n].subtreeEnd == out_.nodes.size()
                                               ? u32(kept.size())
                                               : newIndex[out_.nodes[n].subtreeEnd];
        }
        for (BodyOccurrence& occurrence : out_.occurrences) {
            occurrence.node = newIndex[occurrence.node];
        }
        out_.nodes = std::move(kept);
    }

    // A file with no products still has representations: each with bodies becomes a root.
    void walkLooseRepresentations() {
        std::unordered_set<u32> nested;
        for (const u32 representation : representations_) {
            for (const u32 item : index_.refsOf(representation)) {
                if (item != kNoRecord && index_.kinds[item] == Kind::MAPPED_ITEM) {
                    const std::span<const u32> mapped = index_.refsOf(item);
                    if (!mapped.empty() && mapped[0] != kNoRecord) {
                        const std::span<const u32> map = index_.refsOf(mapped[0]);
                        if (map.size() >= 2) {
                            nested.insert(map[1]);
                        }
                    }
                }
            }
        }
        for (const u32 representation : representations_) {
            if (nested.contains(representation)) {
                continue;
            }
            const u32 node = addNode(NodeKind::Part, {}, kNoRecord);
            visitedRepresentations_.clear();
            walkRepresentation(representation, Affine{}, node, 0);
            closeNode(node);
            if (out_.nodes[node].firstOccurrence == out_.nodes[node].occurrenceEnd) {
                out_.nodes.pop_back();
            }
        }
    }

    const Part21& index_;
    StyleTable styles_;
    Structure out_;

    std::vector<u32> productDefinitions_;
    std::vector<u32> representations_;
    std::vector<bool> isChild_;                // by record
    std::vector<bool> placementRelationships_; // by record
    Relation usagesOfParent_;
    Relation shapesOfDefinition_;
    Relation representationsOfShape_;
    Relation ownerOfRepresentation_;
    Relation relationshipsOfShape_;
    Relation linkedRepresentations_;

    std::vector<u32> onPath_;                 // products from the root to here
    std::vector<u32> visitedRepresentations_; // a part has only a few
    std::unordered_map<u64, u32> bodyOfRecord_;
    std::unordered_map<u32, double> lengthFactors_;
    std::vector<Affine> usagePlacements_; // parallel to usagesOfParent_.all()
    std::unordered_map<u32, std::string> productNames_;
    std::unordered_map<u32, std::string> bodyNames_;
};

// A unit's size in millimetres, or zero if it is not a length unit.
double unitInMillimetres(const Part21& index, u32 unit, int depth) {
    if (unit == kNoRecord || depth > 4) {
        return 0.0;
    }
    const Kind kind = index.kinds[unit];
    if (kind == Kind::SI_UNIT) {
        const std::string_view text = index.text(unit);
        if (text.find("METRE") == std::string_view::npos ||
            text.find("LENGTH_UNIT") == std::string_view::npos) {
            return 0.0;
        }
        ParamCursor cursor(text, "SI_UNIT");
        std::string_view prefix;
        if (!cursor.readEnum(prefix)) {
            return 1000.0; // no prefix: metres
        }
        static constexpr std::array<std::pair<std::string_view, double>, 7> kPrefixes = {
            {{"KILO", 1e6},
             {"DECI", 100.0},
             {"CENTI", 10.0},
             {"MILLI", 1.0},
             {"MICRO", 1e-3},
             {"NANO", 1e-6},
             {"PICO", 1e-9}}};
        for (const auto& [name, value] : kPrefixes) {
            if (prefix == name) {
                return value;
            }
        }
        return 1000.0;
    }
    if (kind == Kind::CONVERSION_BASED_UNIT) {
        const std::string_view text = index.text(unit);
        if (text.find("LENGTH_UNIT") == std::string_view::npos) {
            return 0.0;
        }
        // CONVERSION_BASED_UNIT(name, LENGTH_MEASURE_WITH_UNIT(value, base unit)).
        for (const u32 measure : index.refsOf(unit)) {
            if (measure == kNoRecord || index.kinds[measure] != Kind::LENGTH_MEASURE_WITH_UNIT) {
                continue;
            }
            const std::span<const u32> refs = index.refsOf(measure);
            ParamCursor cursor(index.text(measure));
            double value = 0.0;
            if (refs.empty() || !cursor.readReal(value)) {
                continue;
            }
            const double base = unitInMillimetres(index, refs[0], depth + 1);
            if (base > 0.0) {
                return value * base;
            }
        }
    }
    return 0.0;
}

} // namespace

Affine compose(const Affine& outer, const Affine& inner) {
    Affine result;
    result.linear = simd_mul(outer.linear, inner.linear);
    result.translation = simd_mul(outer.linear, inner.translation) + outer.translation;
    return result;
}

Affine inverse(const Affine& transform) {
    Affine result;
    result.linear = simd_inverse(transform.linear);
    result.translation = -simd_mul(result.linear, transform.translation);
    return result;
}

double lengthFactor(const Part21& index, u32 context) {
    if (context == kNoRecord) {
        return 1.0;
    }
    for (const u32 unit : index.refsOf(context)) {
        const double millimetres = unitInMillimetres(index, unit, 0);
        if (millimetres > 0.0) {
            return millimetres;
        }
    }
    return 1.0;
}

Structure readStructure(const Part21& index) {
    return StructureReader(index).read();
}

} // namespace geo::import::step
