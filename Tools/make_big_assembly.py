#!/usr/bin/env python3
"""Writes a STEP assembly of many placed copies of one bolt-sized box, for scale tests.

The tree is a root holding `groups` sub-assemblies of `per_group` parts each, all instancing one
part product, so the file is mostly placements and the geometry dedupes to one definition.

Usage: make_big_assembly.py <out.step> <groups> <per_group>
"""

import sys


def main() -> None:
    path, groups, per_group = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    lines: list[str] = []
    counter = [0]

    def add(body: str) -> int:
        counter[0] += 1
        lines.append(f"#{counter[0]}={body};")
        return counter[0]

    def point(x: float, y: float, z: float) -> int:
        return add(f"CARTESIAN_POINT('',({x:.3f},{y:.3f},{z:.3f}))")

    def placement(x: float, y: float, z: float) -> int:
        p = point(x, y, z)
        zd = add("DIRECTION('',(0.,0.,1.))")
        xd = add("DIRECTION('',(1.,0.,0.))")
        return add(f"AXIS2_PLACEMENT_3D('',#{p},#{zd},#{xd})")

    app = add("APPLICATION_CONTEXT('automotive design')")
    length = add("( LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.) )")
    angle = add("( NAMED_UNIT(*) PLANE_ANGLE_UNIT() SI_UNIT($,.RADIAN.) )")
    solid = add("( NAMED_UNIT(*) SI_UNIT($,.STERADIAN.) SOLID_ANGLE_UNIT() )")
    uncertainty = add(f"UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-07),#{length},'','')")
    context = add(
        f"( GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((#{uncertainty})) "
        f"GLOBAL_UNIT_ASSIGNED_CONTEXT((#{length},#{angle},#{solid})) REPRESENTATION_CONTEXT('','') )"
    )

    def product(name: str) -> int:
        pc = add(f"PRODUCT_CONTEXT('',#{app},'mechanical')")
        prod = add(f"PRODUCT('{name}','{name}','',(#{pc}))")
        form = add(f"PRODUCT_DEFINITION_FORMATION('','',#{prod})")
        dc = add(f"PRODUCT_DEFINITION_CONTEXT('part definition',#{app},'design')")
        return add(f"PRODUCT_DEFINITION('design','',#{form},#{dc})")

    def shape(definition: int, items: list[int]) -> int:
        rep = add(f"SHAPE_REPRESENTATION('',({','.join(f'#{i}' for i in items)}),#{context})")
        pds = add(f"PRODUCT_DEFINITION_SHAPE('','',#{definition})")
        add(f"SHAPE_DEFINITION_REPRESENTATION(#{pds},#{rep})")
        return rep

    # The box: six planar faces of a 4 x 4 x 8 mm block.
    corners = [(x, y, z) for z in (0.0, 8.0) for y in (0.0, 4.0) for x in (0.0, 4.0)]
    vertices = [add(f"VERTEX_POINT('',#{point(*c)})") for c in corners]
    edges: dict[tuple[int, int], int] = {}
    faces_spec = [((0, 2, 3, 1), (0, 0, -1)), ((4, 5, 7, 6), (0, 0, 1)), ((0, 1, 5, 4), (0, -1, 0)),
                  ((2, 6, 7, 3), (0, 1, 0)), ((0, 4, 6, 2), (-1, 0, 0)), ((1, 3, 7, 5), (1, 0, 0))]
    faces = []
    for loop, normal in faces_spec:
        oriented = []
        for i in range(4):
            a, b = loop[i], loop[(i + 1) % 4]
            low, high = min(a, b), max(a, b)
            if (low, high) not in edges:
                d = [corners[high][k] - corners[low][k] for k in range(3)]
                n = sum(v * v for v in d) ** 0.5
                direction = add(f"DIRECTION('',({d[0]/n:.1f},{d[1]/n:.1f},{d[2]/n:.1f}))")
                vector = add(f"VECTOR('',#{direction},{n:.1f})")
                line = add(f"LINE('',#{point(*corners[low])},#{vector})")
                edges[(low, high)] = add(
                    f"EDGE_CURVE('',#{vertices[low]},#{vertices[high]},#{line},.T.)")
            oriented.append(add(f"ORIENTED_EDGE('',*,*,#{edges[(low, high)]},"
                                f"{'.T.' if a == low else '.F.'})"))
        edge_loop = add(f"EDGE_LOOP('',({','.join(f'#{o}' for o in oriented)}))")
        bound = add(f"FACE_OUTER_BOUND('',#{edge_loop},.T.)")
        nz = add(f"DIRECTION('',({normal[0]:.1f},{normal[1]:.1f},{normal[2]:.1f}))")
        ref = add("DIRECTION('',(1.,0.,0.))" if normal[0] == 0 else "DIRECTION('',(0.,1.,0.))")
        axis = add(f"AXIS2_PLACEMENT_3D('',#{point(*corners[loop[0]])},#{nz},#{ref})")
        plane = add(f"PLANE('',#{axis})")
        faces.append(add(f"ADVANCED_FACE('',(#{bound}),#{plane},.T.)"))
    shell = add(f"CLOSED_SHELL('',({','.join(f'#{f}' for f in faces)}))")
    brep = add(f"MANIFOLD_SOLID_BREP('bolt',#{shell})")
    origin = placement(0, 0, 0)
    bolt_rep = add(f"ADVANCED_BREP_SHAPE_REPRESENTATION('',(#{brep},#{origin}),#{context})")
    bolt = product("bolt")
    pds = add(f"PRODUCT_DEFINITION_SHAPE('','',#{bolt})")
    add(f"SHAPE_DEFINITION_REPRESENTATION(#{pds},#{bolt_rep})")

    def place(parent: int, parent_rep: int, child: int, child_rep: int, x: float, y: float,
              z: float) -> None:
        usage = add(f"NEXT_ASSEMBLY_USAGE_OCCURRENCE('','','',#{parent},#{child},$)")
        source = placement(0, 0, 0)
        target = placement(x, y, z)
        transform = add(f"ITEM_DEFINED_TRANSFORMATION('','',#{source},#{target})")
        relation = add(f"( REPRESENTATION_RELATIONSHIP('','',#{child_rep},#{parent_rep}) "
                       f"REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION(#{transform}) "
                       f"SHAPE_REPRESENTATION_RELATIONSHIP() )")
        shape_of_usage = add(f"PRODUCT_DEFINITION_SHAPE('','',#{usage})")
        add(f"CONTEXT_DEPENDENT_SHAPE_REPRESENTATION(#{relation},#{shape_of_usage})")

    root = product("rack")
    root_rep = shape(root, [placement(0, 0, 0)])
    side = int(per_group ** 0.5 + 0.999)
    for g in range(groups):
        group = product(f"tray {g}")
        group_rep = shape(group, [placement(0, 0, 0)])
        place(root, root_rep, group, group_rep, 0.0, 0.0, g * 12.0)
        for i in range(per_group):
            place(group, group_rep, bolt, bolt_rep, (i % side) * 6.0, (i // side) * 6.0, 0.0)

    with open(path, "w") as out:
        out.write("ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION((''),'2;1');\n"
                  "FILE_NAME('rack','',(''),(''),'','','');\n"
                  "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n")
        out.write("\n".join(lines))
        out.write("\nENDSEC;\nEND-ISO-10303-21;\n")


if __name__ == "__main__":
    main()
