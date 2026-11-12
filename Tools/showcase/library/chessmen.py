"""02 Chessmen: three Staunton sets, sold by the height of their king.

Proportions follow the Staunton pattern: the king's base is about 0.42 of its height, and each
piece is a turned profile on the same ogee base, except the knight's head, which is a carved
silhouette.
"""

from __future__ import annotations

from build123d import Axis, Edge, Face, Location, Solid, Vector, Wire, extrude, fillet

from common import part
from shapes import Profile, at

CHAPTER = "02_chessmen"
SETS = [("travel_set", 45.0), ("club_set", 75.0), ("tournament_set", 95.0)]

# Height and base diameter of each piece against the king's height.
PIECES = [
    ("Pawn", 0.53, 0.32),
    ("Rook", 0.58, 0.37),
    ("Knight", 0.68, 0.37),
    ("Bishop", 0.79, 0.37),
    ("Queen", 0.89, 0.40),
    ("King", 1.00, 0.42),
]


def base(p: Profile, b: float) -> float:
    """The shared ogee base, from the axis out and up; returns the height it ends at."""
    p.line(b, 0).line(b, 0.10 * b)
    p.spline((0.985 * b, 0.18 * b), (0.90 * b, 0.25 * b), (0.93 * b, 0.32 * b), (0.80 * b, 0.40 * b))
    return 0.40 * b


def collar(p: Profile, r_under: float, r: float, z: float, thick: float, r_over: float) -> None:
    """A crisp disc collar: in from the stem, out to r, up `thick`, back in."""
    p.line(r_under, z).line(r, z).line(r, z + thick).line(r_over, z + thick)


def pawn(H: float, h: float, b: float) -> Solid:
    p = Profile(0, 0)
    z = base(p, b)
    zc = 0.52 * h
    p.spline((0.55 * b, z + 0.10 * h), (0.40 * b, zc - 0.05 * h), (0.42 * b, zc))
    collar(p, 0.42 * b, 0.66 * b, zc, 0.035 * h, 0.34 * b)
    p.ball(0.46 * b, h, 0.34 * b)
    return p.turn()


def rook(H: float, h: float, b: float) -> Solid:
    p = Profile(0, 0)
    z = base(p, b)
    zt = 0.70 * h
    p.spline((0.66 * b, z + 0.12 * h), (0.54 * b, 0.45 * h), (0.58 * b, zt - 0.06 * h), (0.70 * b, zt))
    collar(p, 0.70 * b, 0.74 * b, zt, 0.03 * h, 0.70 * b)
    p.line(0.70 * b, h).line(0, h)
    shape = p.turn()
    # The tower's hollow top and its four crenels.
    shape = shape - Solid.make_cylinder(0.50 * b, 0.14 * h).moved(Location((0, 0, h - 0.10 * h)))
    slot = Solid.make_box(0.24 * b, 2.0 * b, 0.10 * h).moved(Location((-0.12 * b, -1.0 * b, h - 0.09 * h)))
    for angle in (0, 90):
        shape = shape - at(slot, rz=angle)
    return shape


def bishop(H: float, h: float, b: float) -> Solid:
    p = Profile(0, 0)
    z = base(p, b)
    zc = 0.50 * h
    p.spline((0.58 * b, z + 0.10 * h), (0.38 * b, zc - 0.06 * h), (0.40 * b, zc))
    collar(p, 0.40 * b, 0.70 * b, zc, 0.03 * h, 0.40 * b)
    collar(p, 0.40 * b, 0.56 * b, zc + 0.05 * h, 0.025 * h, 0.36 * b)
    # The mitre: an egg rising to a point, then a small ball.
    top = h - 0.11 * b * 2
    p.spline((0.52 * b, zc + 0.16 * h), (0.50 * b, zc + 0.26 * h), (0.30 * b, top - 0.06 * h),
             (0.10 * b, top))
    p.ball(0.13 * b, h, 0.06 * b)
    shape = p.turn()
    # The mitre's slot, cut at a slant.
    cut = Solid.make_box(0.07 * b, 2.0 * b, 0.40 * h).moved(Location((-0.035 * b, -1.0 * b, 0)))
    cut = cut.moved(Location((0, 0, 0), (0, 1, 0), 35)).moved(Location((0.10 * b, 0, zc + 0.30 * h)))
    return shape - cut


def queen(H: float, h: float, b: float) -> Solid:
    p = Profile(0, 0)
    z = base(p, b)
    zc = 0.60 * h
    p.spline((0.58 * b, z + 0.12 * h), (0.36 * b, zc - 0.10 * h), (0.40 * b, zc))
    collar(p, 0.40 * b, 0.70 * b, zc, 0.03 * h, 0.38 * b)
    collar(p, 0.38 * b, 0.54 * b, zc + 0.05 * h, 0.02 * h, 0.34 * b)
    zr = h - 0.17 * h
    p.spline((0.40 * b, zc + 0.12 * h), (0.62 * b, zr))
    p.line(0.62 * b, zr + 0.02 * h).line(0.36 * b, zr + 0.04 * h)
    R = 0.15 * b
    p.spline((0.22 * b, zr + 0.08 * h), (0.07 * b, h - 1.8 * R))
    p.ball(R, h, 0.06 * b)
    shape = p.turn()
    # The coronet: eight balls round the crown's rim.
    ball = Solid.make_sphere(0.08 * b).moved(Location((0.60 * b, 0, zr + 0.03 * h)))
    for i in range(8):
        shape = shape + at(ball, rz=i * 45)
    return shape


def king(H: float, h: float, b: float) -> Solid:
    p = Profile(0, 0)
    z = base(p, b)
    zc = 0.62 * h
    p.spline((0.58 * b, z + 0.12 * h), (0.36 * b, zc - 0.10 * h), (0.42 * b, zc))
    collar(p, 0.42 * b, 0.72 * b, zc, 0.03 * h, 0.40 * b)
    collar(p, 0.40 * b, 0.56 * b, zc + 0.05 * h, 0.02 * h, 0.36 * b)
    zr = 0.86 * h
    p.spline((0.42 * b, zc + 0.12 * h), (0.60 * b, zr))
    p.line(0.60 * b, zr + 0.015 * h)
    p.spline((0.40 * b, zr + 0.05 * h), (0.10 * b, zr + 0.065 * h))
    p.line(0.10 * b, zr + 0.065 * h)
    top = zr + 0.065 * h
    shape = p.turn()
    # The cross, faced to the front.
    arm = 0.17 * b
    upright = Solid.make_box(arm, arm, h - top).moved(Location((-arm / 2, -arm / 2, top)))
    bar = Solid.make_box(0.50 * b, arm, arm).moved(Location((-0.25 * b, -arm / 2, h - 0.075 * h)))
    return shape + upright + bar


# The knight's head in side view, facing -X, in units of the base radius from where it sits: runs
# of smooth curve ("S") or straight line ("L"), corners where one run meets the next.
KNIGHT = [
    ("S", [(0.60, 0.00), (0.72, 0.30), (0.74, 0.62), (0.64, 0.98), (0.48, 1.30), (0.38, 1.56)]),
    ("L", [(0.38, 1.56), (0.30, 1.84), (0.16, 1.62)]),
    ("S", [(0.16, 1.62), (0.02, 1.57), (-0.30, 1.44), (-0.58, 1.24), (-0.82, 1.04),
           (-0.90, 0.92)]),
    ("S", [(-0.90, 0.92), (-0.88, 0.82), (-0.80, 0.76)]),
    ("L", [(-0.80, 0.76), (-0.60, 0.74), (-0.76, 0.68)]),
    ("S", [(-0.76, 0.68), (-0.70, 0.60), (-0.48, 0.60), (-0.28, 0.54)]),
    ("S", [(-0.28, 0.54), (-0.20, 0.40), (-0.30, 0.22), (-0.46, 0.08), (-0.52, 0.00)]),
]


def knight(H: float, h: float, b: float) -> Solid:
    p = Profile(0, 0)
    z = base(p, b)
    p.spline((0.66 * b, z + 0.06 * h), (0.60 * b, z + 0.10 * h))
    p.line(0, z + 0.10 * h)
    shape = p.turn()
    seat = z + 0.08 * h
    scale = (h - seat) / 1.84
    t = 0.56 * b
    def v(x, zz):
        return Vector(x * scale, -t / 2, seat + zz * scale)
    edges = []
    for kind, run in KNIGHT:
        pts = [v(*q) for q in run]
        if kind == "S":
            edges.append(Edge.make_spline(pts))
        else:
            edges += [Edge.make_line(a, b2) for a, b2 in zip(pts, pts[1:])]
    edges.append(Edge.make_line(v(*KNIGHT[-1][1][-1]), v(*KNIGHT[0][1][0])))
    head = extrude(Face(Wire(edges)), t, dir=(0, 1, 0))
    try:
        sides = (head.edges().filter_by_position(Axis.Y, -t / 2 - 1e-6, -t / 2 + 1e-6)
                 + head.edges().filter_by_position(Axis.Y, t / 2 - 1e-6, t / 2 + 1e-6))
        head = fillet(sides, 0.22 * t)
    except Exception:
        pass
    # An eye through the head, and a nostril.
    eye = Solid.make_cylinder(0.07 * b, 2 * t).moved(
        Location((-0.30 * scale, -t, seat + 1.30 * scale), (1, 0, 0), -90))
    return shape + head - eye


MAKERS = {"Pawn": pawn, "Rook": rook, "Knight": knight, "Bishop": bishop, "Queen": queen, "King": king}


def files():
    """(path in the library, shape) for every file in the chapter."""
    for folder, H in SETS:
        for i, (name, height, base_d) in enumerate(PIECES):
            shape = MAKERS[name](H, height * H, base_d * H / 2)
            label = f"{name.lower()}_{int(H)}mm"
            yield f"{CHAPTER}/{folder}/{i + 1:02d} {name}.step", part(shape, label)
