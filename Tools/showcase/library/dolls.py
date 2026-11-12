"""06 Nesting Dolls: matryoshka, each file one more doll, the smaller ones nested inside.

Each doll is 0.8 of the next, in two halves that meet at the belly over a lip. A file of seven
holds thirteen halves and the solid smallest doll, which a cut shows as rings within rings.
"""

from __future__ import annotations

from build123d import Location, Solid

from common import assembly, part
from shapes import Profile

CHAPTER = "06_nesting_dolls"
SMALLEST = 41.0
RATIO = 1.25

# The doll's outline against its height: radius at heights from the foot to the crown.
OUTLINE = [(0.300, 0.03), (0.335, 0.20), (0.320, 0.38), (0.262, 0.56), (0.245, 0.64),
           (0.262, 0.76), (0.230, 0.90), (0.120, 0.985)]
SPLIT = 0.40  # where the halves meet, against the height


def solid_doll(H: float, shrink: float = 0.0, lift: float = 0.0) -> Solid:
    """The doll's body, or its inside when shrunk by a wall `shrink` thick and lifted."""
    pts = [(max(r * H - shrink, 0.2), z * H * (1 - 2 * shrink / H) + lift + shrink) for r, z in OUTLINE]
    p = Profile(0, lift + shrink).line(pts[0][0] - 0.03 * H, lift + shrink)
    p.spline(*pts)
    top = H * (1 - shrink / H) + lift if shrink else H
    p.spline((0.05 * H, top - 0.004 * H), (0, top))
    return p.turn()


def doll(H: float, smallest: bool) -> list[Solid]:
    """A doll's two halves, upper and lower, or one solid doll."""
    body = solid_doll(H)
    if smallest:
        return [body]
    wall = max(1.2, 0.035 * H)
    hollow = solid_doll(H, wall)
    shell = body - hollow
    split = SPLIT * H
    below = Solid.make_box(2 * H, 2 * H, split).moved(Location((-H, -H, 0)))
    lower = shell & below
    upper = shell - below
    # The lip: a short ring rising from the lower half into the upper.
    r_in = max(OUTLINE[2][0] * H - wall, 1)
    lip = (Solid.make_cylinder(r_in + 0.45 * wall, 0.08 * H) - Solid.make_cylinder(r_in - 0.1 * wall, 0.08 * H)) \
        .moved(Location((0, 0, split)))
    lip = lip - hollow
    upper = upper - Solid.make_cylinder(r_in + 0.5 * wall, 0.08 * H).moved(Location((0, 0, split)))
    return [upper, lower + lip]


def nested(count: int) -> list:
    """Dolls nested, the largest first, each standing on the floor of the one around it."""
    children = []
    z = 0.0
    for k in range(count, 0, -1):
        H = SMALLEST * RATIO ** (k - 1)
        halves = doll(H, k == 1)
        names = ["doll"] if k == 1 else [f"doll_{k}_upper", f"doll_{k}_lower"]
        if k == 1:
            names = ["doll_1"]
        for shape, name in zip(halves, names):
            children.append(part(shape.moved(Location((0, 0, z))), name))
        z += max(1.2, 0.035 * H) + 0.4
    return children


def files():
    for count in range(1, 8):
        name = f"matryoshka_{count}"
        if count == 1:
            yield f"{CHAPTER}/matryoshka/{count:02d} Matryoshka 1.step", part(solid_doll(SMALLEST), "doll_1")
        else:
            yield f"{CHAPTER}/matryoshka/{count:02d} Matryoshka {count}.step", assembly(name, nested(count))
