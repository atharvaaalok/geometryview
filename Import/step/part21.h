// An index over a STEP Part 21 file: every record's id, entity type, position and references, in
// flat arrays. Building it touches each byte once, on all cores, so a model's structure is known
// in milliseconds. Values other than references are decoded on demand, by `ParamCursor`.
#pragma once

#include "base/types.h"

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace geo::import::step {

inline constexpr u32 kNoRecord = 0xFFFFFFFF;

// Entity types the importer acts on. Everything else is `Other`, and is only ever copied or hashed.
#define GEO_STEP_KINDS(X)                                                                          \
    X(ADVANCED_BREP_SHAPE_REPRESENTATION)                                                          \
    X(ADVANCED_FACE)                                                                               \
    X(AXIS2_PLACEMENT_3D)                                                                          \
    X(BREP_WITH_VOIDS)                                                                             \
    X(CARTESIAN_POINT)                                                                             \
    X(CARTESIAN_TRANSFORMATION_OPERATOR_3D)                                                        \
    X(CIRCLE)                                                                                      \
    X(CLOSED_SHELL)                                                                                \
    X(COLOUR_RGB)                                                                                  \
    X(COMPLEX_TRIANGULATED_FACE)                                                                   \
    X(COMPLEX_TRIANGULATED_SURFACE_SET)                                                            \
    X(CONNECTED_FACE_SET)                                                                          \
    X(CONSTRUCTIVE_GEOMETRY_REPRESENTATION)                                                        \
    X(CONSTRUCTIVE_GEOMETRY_REPRESENTATION_RELATIONSHIP)                                           \
    X(CONTEXT_DEPENDENT_SHAPE_REPRESENTATION)                                                      \
    X(CONVERSION_BASED_UNIT)                                                                       \
    X(DIRECTION)                                                                                   \
    X(DRAUGHTING_PRE_DEFINED_COLOUR)                                                               \
    X(EDGE_BASED_WIREFRAME_SHAPE_REPRESENTATION)                                                   \
    X(ELLIPSE)                                                                                     \
    X(FACE_BASED_SURFACE_MODEL)                                                                    \
    X(FACE_SURFACE)                                                                                \
    X(FACETED_BREP)                                                                                \
    X(FACETED_BREP_AND_BREP_WITH_VOIDS)                                                            \
    X(FACETED_BREP_SHAPE_REPRESENTATION)                                                           \
    X(FILL_AREA_STYLE)                                                                             \
    X(FILL_AREA_STYLE_COLOUR)                                                                      \
    X(GEOMETRIC_CURVE_SET)                                                                         \
    X(GEOMETRIC_REPRESENTATION_CONTEXT)                                                            \
    X(GEOMETRIC_SET)                                                                               \
    X(GEOMETRICALLY_BOUNDED_SURFACE_SHAPE_REPRESENTATION)                                          \
    X(GEOMETRICALLY_BOUNDED_WIREFRAME_SHAPE_REPRESENTATION)                                        \
    X(ITEM_DEFINED_TRANSFORMATION)                                                                 \
    X(LENGTH_MEASURE_WITH_UNIT)                                                                    \
    X(MANIFOLD_SOLID_BREP)                                                                         \
    X(MANIFOLD_SURFACE_SHAPE_REPRESENTATION)                                                       \
    X(MAPPED_ITEM)                                                                                 \
    X(NAMED_UNIT)                                                                                  \
    X(NEXT_ASSEMBLY_USAGE_OCCURRENCE)                                                              \
    X(OPEN_SHELL)                                                                                  \
    X(ORIENTED_CLOSED_SHELL)                                                                       \
    X(ORIENTED_FACE)                                                                               \
    X(ORIENTED_OPEN_SHELL)                                                                         \
    X(OVER_RIDING_STYLED_ITEM)                                                                     \
    X(PRESENTATION_STYLE_ASSIGNMENT)                                                               \
    X(PRESENTATION_STYLE_BY_CONTEXT)                                                               \
    X(PRODUCT)                                                                                     \
    X(PRODUCT_DEFINITION)                                                                          \
    X(PRODUCT_DEFINITION_FORMATION)                                                                \
    X(PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE)                                          \
    X(PRODUCT_DEFINITION_SHAPE)                                                                    \
    X(PRODUCT_DEFINITION_WITH_ASSOCIATED_DOCUMENTS)                                                \
    X(REPRESENTATION)                                                                              \
    X(REPRESENTATION_MAP)                                                                          \
    X(REPRESENTATION_RELATIONSHIP)                                                                 \
    X(REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION)                                             \
    X(SHAPE_DEFINITION_REPRESENTATION)                                                             \
    X(SHAPE_REPRESENTATION)                                                                        \
    X(SHAPE_REPRESENTATION_RELATIONSHIP)                                                           \
    X(SHAPE_REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION)                                       \
    X(SHELL_BASED_SURFACE_MODEL)                                                                   \
    X(SI_UNIT)                                                                                     \
    X(SPHERICAL_SURFACE)                                                                           \
    X(STYLED_ITEM)                                                                                 \
    X(SURFACE_SIDE_STYLE)                                                                          \
    X(SURFACE_STYLE_FILL_AREA)                                                                     \
    X(SURFACE_STYLE_RENDERING)                                                                     \
    X(SURFACE_STYLE_RENDERING_WITH_PROPERTIES)                                                     \
    X(SURFACE_STYLE_USAGE)                                                                         \
    X(TESSELLATED_FACE_SET)                                                                        \
    X(TESSELLATED_GEOMETRIC_SET)                                                                   \
    X(TESSELLATED_SHAPE_REPRESENTATION)                                                            \
    X(TESSELLATED_SHELL)                                                                           \
    X(TESSELLATED_SOLID)                                                                           \
    X(TOROIDAL_SURFACE)                                                                            \
    X(TRIANGULATED_FACE)                                                                           \
    X(TRIANGULATED_SURFACE_SET)                                                                    \
    X(VERTEX_POINT)

enum class Kind : u16 {
    Other,
    Complex, // several entity types, none of which the importer acts on
#define X(name) name,
    GEO_STEP_KINDS(X)
#undef X
};

/// The index. Arrays run parallel over records, in file order.
struct Part21 {
    std::span<const u8> file;
    std::string_view header; // everything before the first record, ending with the DATA statement

    std::vector<u32> ids;
    std::vector<Kind> kinds;
    std::vector<u64> offsets;   // first byte of the record
    std::vector<u32> lengths;   // bytes through the closing semicolon
    std::vector<u32> refStarts; // one more than there are records
    std::vector<u32> refs;      // record indices, or kNoRecord for a dangling reference

    u32 maxId = 0;
    bool needsWholeFile = false; // uses a construct only a full reader understands

    u32 recordCount() const { return u32(ids.size()); }
    std::span<const u32> refsOf(u32 record) const {
        return std::span(refs).subspan(refStarts[record],
                                       refStarts[record + 1] - refStarts[record]);
    }
    std::string_view text(u32 record) const {
        return {reinterpret_cast<const char*>(file.data() + offsets[record]), lengths[record]};
    }
};

/// Indexes `file`, which must outlive `index`. If the file contains comments, they are blanked in
/// `scratch`, which then backs the index and must outlive it too.
bool indexPart21(std::span<const u8> file, Part21& index, std::vector<u8>& scratch,
                 std::string& error);

/// Reads one entity's parameters in order. Skipping is cheap, so callers read only what they need.
///
/// Files are untrusted, so every read reports failure rather than assuming a shape: a wrong type
/// leaves the cursor in place and returns false.
class ParamCursor {
public:
    /// Starts at the first parameter. In a complex entity, it starts at the sub-entity `name`.
    ParamCursor(std::string_view record, std::string_view name = {});

    bool valid() const { return valid_; }
    bool atEnd() const;

    bool skip();                            // past one parameter, of any form
    bool readString(std::string& value);    // decodes quotes and \X2\ escapes to UTF-8
    bool readReal(double& value);           // also accepts an integer and a typed value: NAME(1.5)
    bool readEnum(std::string_view& value); // the text between the dots
    bool readId(u32& id);                   // the number after #
    bool enterList();                       // positions at the list's first element
    bool leaveList();                       // past the list's closing bracket
    bool inList() const;                    // true unless at the list's closing bracket

private:
    void skipSeparators();

    const char* cursor_;
    const char* end_;
    bool valid_ = true;
};

} // namespace geo::import::step
