// Writes small STEP files for tests: boxes as exact B-reps, placed in an assembly. Generating
// them keeps fixtures readable and lets each test vary only what it checks.
#pragma once

#include <array>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace geo::test {

class StepWriter {
public:
    /// `unit` is the SI prefix for metres, such as "MILLI", or empty for metres.
    explicit StepWriter(std::string unit = "MILLI") : unit_(std::move(unit)) {}

    /// A box from `lo` to `hi`, wound outward. With `cavity`, a smaller box inside is a void,
    /// written the standard way: an outward shell that the oriented shell reverses.
    int box(std::array<double, 3> lo, std::array<double, 3> hi, bool withCavity = false) {
        return boxes({{lo, hi, withCavity}});
    }

    struct Solid {
        std::array<double, 3> lo, hi;
        bool withCavity = false;
        std::string name = "box"; // the body's own name
        // The top as two faces, split across x at its middle: an edge between coplanar faces,
        // which meet smoothly.
        bool splitTop = false;
    };

    /// Several boxes as the bodies of one shape, as `box` writes each.
    int boxes(const std::vector<Solid>& solids) {
        const int context = ensureContext();
        std::string list;
        for (const Solid& solid : solids) {
            list += std::format(
                "#{},", brep(solid.lo, solid.hi, solid.withCavity, solid.name, solid.splitTop));
        }
        const int origin = placement({0, 0, 0});
        return add(std::format("ADVANCED_BREP_SHAPE_REPRESENTATION('',({}#{}),#{})", list, origin,
                               context));
    }

    /// A product whose shape is `representation`. Returns its product definition.
    int part(const std::string& name, int representation) {
        const int definition = product(name);
        const int shape = add(std::format("PRODUCT_DEFINITION_SHAPE('','',#{})", definition));
        add(std::format("SHAPE_DEFINITION_REPRESENTATION(#{},#{})", shape, representation));
        return definition;
    }

    /// An assembly product with its own (empty) shape representation, to which children attach.
    int assembly(const std::string& name) {
        const int context = ensureContext();
        const int origin = placement({0, 0, 0});
        const int representation =
            add(std::format("SHAPE_REPRESENTATION('',(#{}),#{})", origin, context));
        const int definition = part(name, representation);
        assemblyRepresentations_[definition] = representation;
        return definition;
    }

    /// An assembly's shape representation, for placing it in another as `childRepresentation`.
    int representationOf(int assembly) const { return assemblyRepresentations_.at(assembly); }

    /// Places `child` (with representation `childRepresentation`) in the assembly `parent` at
    /// `offset`.
    void place(int parent, int child, int childRepresentation, std::array<double, 3> offset) {
        const int usage =
            add(std::format("NEXT_ASSEMBLY_USAGE_OCCURRENCE('','','',#{},#{},$)", parent, child));
        const int from = placement({0, 0, 0});
        const int to = placement(offset);
        const int transform =
            add(std::format("ITEM_DEFINED_TRANSFORMATION('','',#{},#{})", from, to));
        const int relation =
            add(std::format("( REPRESENTATION_RELATIONSHIP('','',#{},#{}) "
                            "REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION(#{}) "
                            "SHAPE_REPRESENTATION_RELATIONSHIP() )",
                            childRepresentation, representationOf(parent), transform));
        const int shape = add(std::format("PRODUCT_DEFINITION_SHAPE('','',#{})", usage));
        add(std::format("CONTEXT_DEPENDENT_SHAPE_REPRESENTATION(#{},#{})", relation, shape));
    }

    std::string text() const {
        std::string file = "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION((''),'2;1');\n"
                           "FILE_NAME('test','',(''),(''),'','','');\n"
                           "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\n"
                           "ENDSEC;\nDATA;\n";
        for (const std::string& record : records_) {
            file += record;
            file += '\n';
        }
        file += "ENDSEC;\nEND-ISO-10303-21;\n";
        return file;
    }

private:
    int add(const std::string& body) {
        const int id = int(records_.size()) + 1;
        records_.push_back(std::format("#{}={};", id, body));
        return id;
    }

    static std::string number(double value) { return std::format("{:.6f}", value); }

    int point(std::array<double, 3> p) {
        return add(std::format("CARTESIAN_POINT('',({},{},{}))", number(p[0]), number(p[1]),
                               number(p[2])));
    }

    int direction(std::array<double, 3> d) {
        return add(
            std::format("DIRECTION('',({},{},{}))", number(d[0]), number(d[1]), number(d[2])));
    }

    int placement(std::array<double, 3> origin, std::array<double, 3> axis = {0, 0, 1},
                  std::array<double, 3> reference = {1, 0, 0}) {
        const int location = point(origin);
        const int z = direction(axis);
        const int x = direction(reference);
        return add(std::format("AXIS2_PLACEMENT_3D('',#{},#{},#{})", location, z, x));
    }

    int ensureContext() {
        if (context_ != 0) {
            return context_;
        }
        const int length =
            add(unit_.empty()
                    ? "( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT($,.METRE.) )"
                    : std::format("( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.{}.,.METRE.) )", unit_));
        const int angle = add("( NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.) )");
        const int solid = add("( NAMED_UNIT(*) SI_UNIT($,.STERADIAN.) SOLID_ANGLE_UNIT() )");
        const int uncertainty = add(std::format("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-"
                                                "07),#{},'distance_accuracy_value','')",
                                                length));
        context_ = add(std::format(
            "( GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((#{})) "
            "GLOBAL_UNIT_ASSIGNED_CONTEXT((#{},#{},#{})) REPRESENTATION_CONTEXT('','') )",
            uncertainty, length, angle, solid));
        return context_;
    }

    int product(const std::string& name) {
        if (applicationContext_ == 0) {
            applicationContext_ = add("APPLICATION_CONTEXT('automotive design')");
        }
        const int productContext =
            add(std::format("PRODUCT_CONTEXT('',#{},'mechanical')", applicationContext_));
        const int item =
            add(std::format("PRODUCT('{}','{}','',(#{}))", name, name, productContext));
        const int formation = add(std::format("PRODUCT_DEFINITION_FORMATION('','',#{})", item));
        const int definitionContext = add(std::format(
            "PRODUCT_DEFINITION_CONTEXT('part definition',#{},'design')", applicationContext_));
        return add(
            std::format("PRODUCT_DEFINITION('design','',#{},#{})", formation, definitionContext));
    }

    // A closed shell of six planar faces with outward normals.
    // One solid, named `name`: a box, with a void if `withCavity`.
    int brep(std::array<double, 3> lo, std::array<double, 3> hi, bool withCavity,
             const std::string& name, bool splitTop = false) {
        const int outer = shell(lo, hi, splitTop);
        int body = 0;
        if (withCavity) {
            std::array<double, 3> innerLo{}, innerHi{};
            for (int axis = 0; axis < 3; ++axis) {
                const double quarter = (hi[axis] - lo[axis]) * 0.25;
                innerLo[axis] = lo[axis] + quarter;
                innerHi[axis] = hi[axis] - quarter;
            }
            const int inner = shell(innerLo, innerHi);
            const int oriented = add(std::format("ORIENTED_CLOSED_SHELL('',*,#{},.F.)", inner));
            body = add(std::format("BREP_WITH_VOIDS('{}',#{},(#{}))", name, outer, oriented));
        } else {
            body = add(std::format("MANIFOLD_SOLID_BREP('{}',#{})", name, outer));
        }
        return body;
    }

    int shell(std::array<double, 3> lo, std::array<double, 3> hi, bool splitTop = false) {
        std::vector<std::array<double, 3>> corners(8);
        for (int c = 0; c < 8; ++c) {
            corners[c] = {c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]};
        }
        if (splitTop) {
            // The top edges' middles, along x, at the low and high y: corners 8 and 9.
            const double middle = (lo[0] + hi[0]) / 2.0;
            corners.push_back({middle, lo[1], hi[2]});
            corners.push_back({middle, hi[1], hi[2]});
        }
        std::vector<int> vertices;
        for (const auto& corner : corners) {
            vertices.push_back(add(std::format("VERTEX_POINT('',#{})", point(corner))));
        }
        // Each face as its corners, counter-clockwise seen from outside, with its outward normal.
        struct Face {
            std::vector<int> corners;
            std::array<double, 3> normal;
            std::array<double, 3> reference;
        };
        std::vector<Face> faces = {
            {{0, 2, 3, 1}, {0, 0, -1}, {1, 0, 0}}, {{4, 5, 7, 6}, {0, 0, 1}, {1, 0, 0}},
            {{0, 1, 5, 4}, {0, -1, 0}, {1, 0, 0}}, {{2, 6, 7, 3}, {0, 1, 0}, {1, 0, 0}},
            {{0, 4, 6, 2}, {-1, 0, 0}, {0, 1, 0}}, {{1, 3, 7, 5}, {1, 0, 0}, {0, 1, 0}},
        };
        if (splitTop) {
            faces[1] = {{4, 8, 9, 6}, {0, 0, 1}, {1, 0, 0}};
            faces.insert(faces.begin() + 2, Face{{8, 5, 7, 9}, {0, 0, 1}, {1, 0, 0}});
            faces[3].corners = {0, 1, 5, 8, 4};
            faces[4].corners = {2, 6, 9, 7, 3};
        }
        std::vector<int> faceIds;
        std::map<std::pair<int, int>, int> edges;
        for (const Face& face : faces) {
            std::vector<int> oriented;
            const usize n = face.corners.size();
            for (usize i = 0; i < n; ++i) {
                const int a = face.corners[i], b = face.corners[(i + 1) % n];
                const int low = std::min(a, b), high = std::max(a, b);
                int& edge = edges[{low, high}];
                if (edge == 0) {
                    std::array<double, 3> d{};
                    double length = 0;
                    for (int axis = 0; axis < 3; ++axis) {
                        d[axis] = corners[high][axis] - corners[low][axis];
                        length += d[axis] * d[axis];
                    }
                    length = std::sqrt(length);
                    for (double& value : d) {
                        value /= length;
                    }
                    const int vector =
                        add(std::format("VECTOR('',#{},{})", direction(d), number(length)));
                    const int line =
                        add(std::format("LINE('',#{},#{})", point(corners[low]), vector));
                    edge = add(std::format("EDGE_CURVE('',#{},#{},#{},.T.)", vertices[low],
                                           vertices[high], line));
                }
                oriented.push_back(add(
                    std::format("ORIENTED_EDGE('',*,*,#{},{})", edge, a == low ? ".T." : ".F.")));
            }
            std::string loopEdges;
            for (const int e : oriented) {
                loopEdges += std::format("{}#{}", loopEdges.empty() ? "" : ",", e);
            }
            const int loop = add(std::format("EDGE_LOOP('',({}))", loopEdges));
            const int bound = add(std::format("FACE_OUTER_BOUND('',#{},.T.)", loop));
            const int plane = add(std::format(
                "PLANE('',#{})", placement(corners[face.corners[0]], face.normal, face.reference)));
            faceIds.push_back(add(std::format("ADVANCED_FACE('',(#{}),#{},.T.)", bound, plane)));
        }
        std::string list;
        for (const int id : faceIds) {
            list += std::format("{}#{}", list.empty() ? "" : ",", id);
        }
        return add(std::format("CLOSED_SHELL('',({}))", list));
    }

    std::string unit_;
    std::vector<std::string> records_;
    int context_ = 0;
    int applicationContext_ = 0;
    std::map<int, int> assemblyRepresentations_; // assembly definition to its representation
};

} // namespace geo::test
