"""The Parade: 100,000 chessmen on a field 24 by 15 metres, for the speed claims.

The six pieces of the club set (Library: chessmen) are written once each by build123d, every one
in its own group: pawns, rooks, knights, bishops, queens, kings. The rest of the 100,000 are
placements added to the file as text, as Tools/make_big_assembly.py writes its rack, so the file is
mostly placements and the geometry is six definitions. In material colours each group takes its own
colour, so the field is six bands.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

from build123d import Pos

from common import assembly, part, write_step

COLUMNS = 400
SPACING = 60.0  # mm between pieces: their bases are 24 to 32 mm across
# Rows of each piece, front to back: 250 rows of 400, 100,000 in all.
BANDS = [("Pawn", 42), ("Rook", 42), ("Knight", 42), ("Bishop", 41), ("Queen", 42), ("King", 41)]
KING = 75.0  # the club set's king height


def write(step: Path) -> Path:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from library.chessmen import MAKERS, PIECES

    sizes = {name: (h, b) for name, h, b in PIECES}
    # Each group's first piece where its band starts, written by build123d.
    groups, row = [], 0
    starts = {}
    for name, rows in BANDS:
        h, b = sizes[name]
        piece = part(MAKERS[name](KING, h * KING, b * KING / 2).moved(Pos(0, row * SPACING, 0)), name.lower())
        groups.append(assembly(f"{name.lower()}s", [piece]))
        starts[name] = row
        row += rows
    seed = step.with_suffix(".seed.step")
    write_step(assembly("parade", groups), seed)
    text = seed.read_text()
    seed.unlink()

    entity = {int(m.group(1)): m.group(2) for m in re.finditer(r"#(\d+) = (.*?);\s*$", text, re.S | re.M)}

    def find(pattern: str) -> int:
        for k, v in entity.items():
            if re.fullmatch(pattern, v, re.S):
                return k
        raise KeyError(pattern)

    def definition(name: str) -> tuple[int, int]:
        """A product's definition and its shape representation."""
        prod = find(rf"PRODUCT\('{name}','{name}',.*")
        form = find(rf"PRODUCT_DEFINITION_FORMATION\('','',#{prod}\)")
        pd = find(rf"PRODUCT_DEFINITION\('design','',#{form},#\d+\)")
        pds = find(rf"PRODUCT_DEFINITION_SHAPE\('','',#{pd}\)")
        rep = int(re.search(r"#(\d+)\)$", entity[find(rf"SHAPE_DEFINITION_REPRESENTATION\(#{pds},#\d+\)")]).group(1))
        return pd, rep

    source = int(re.search(r"ITEM_DEFINED_TRANSFORMATION\('',\s*'',\s*#(\d+)", text).group(1))
    lines: list[str] = []
    counter = [max(entity)]

    def add(body: str) -> int:
        counter[0] += 1
        lines.append(f"#{counter[0]} = {body};")
        return counter[0]

    z_dir = add("DIRECTION('',(0.,0.,1.))")
    x_dir = add("DIRECTION('',(1.,0.,0.))")
    for name, rows in BANDS:
        group_pd, group_rep = definition(f"{name.lower()}s")
        piece_pd, piece_rep = definition(name.lower())
        for r in range(rows):
            for c in range(COLUMNS):
                if r == 0 and c == 0:
                    continue  # the piece build123d wrote
                y = (starts[name] + r) * SPACING
                point = add(f"CARTESIAN_POINT('',({c * SPACING:.1f},{y:.1f},0.))")
                target = add(f"AXIS2_PLACEMENT_3D('',#{point},#{z_dir},#{x_dir})")
                transform = add(f"ITEM_DEFINED_TRANSFORMATION('','',#{source},#{target})")
                relation = add(f"( REPRESENTATION_RELATIONSHIP('','',#{piece_rep},#{group_rep}) "
                               f"REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION(#{transform}) "
                               f"SHAPE_REPRESENTATION_RELATIONSHIP() )")
                usage = add(f"NEXT_ASSEMBLY_USAGE_OCCURRENCE('','','',#{group_pd},#{piece_pd},$)")
                usage_shape = add(f"PRODUCT_DEFINITION_SHAPE('','',#{usage})")
                add(f"CONTEXT_DEPENDENT_SHAPE_REPRESENTATION(#{relation},#{usage_shape})")
    end = text.rindex("ENDSEC;")
    step.write_text(text[:end] + "\n".join(lines) + "\n" + text[end:])
    return step
