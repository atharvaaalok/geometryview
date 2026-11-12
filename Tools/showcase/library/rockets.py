"""08 Rockets: eight model rockets, 70 to 240 mm, each named for a bird.

Each is an assembly: a body tube, a nose cone, its fins (one part of three or four bodies), a
launch lug, and the motor inside, so a cut shows the motor in its mount.
"""

from __future__ import annotations

import math

from build123d import Edge, Face, Location, Solid, Vector, Wire, extrude

from common import assembly, bodies, part
from shapes import Profile, at

CHAPTER = "08_rockets"

# Name, length, diameter, nose (cone, ogive, parabolic), fins, fin shape.
ROCKETS = [
    ("Sparrow", 70, 13, "cone", 3, "trapezoid"),
    ("Finch", 95, 15, "ogive", 4, "swept"),
    ("Kestrel", 120, 18, "ogive", 3, "delta"),
    ("Heron", 145, 20, "parabolic", 4, "elliptic"),
    ("Osprey", 170, 24, "ogive", 4, "swept"),
    ("Albatross", 195, 25, "cone", 3, "trapezoid"),
    ("Condor", 220, 29, "parabolic", 4, "delta"),
    ("Pelican", 240, 33, "ogive", 4, "elliptic"),
]


def nose(kind: str, r: float, length: float) -> Solid:
    """A nose cone standing on its shoulder, which slips into the tube."""
    shoulder = 0.6 * r
    p = Profile(0, -shoulder).line(r * 0.92, -shoulder).line(r * 0.92, 0).line(r, 0)
    if kind == "cone":
        p.line(0, length)
    elif kind == "parabolic":
        p.spline((0.96 * r, 0.30 * length), (0.75 * r, 0.65 * length), (0.40 * r, 0.90 * length), (0, length),
                 end_tangent=(-1, 0))
    else:  # a tangent ogive
        p.spline((0.98 * r, 0.20 * length), (0.84 * r, 0.50 * length), (0.50 * r, 0.80 * length), (0, length))
    return p.line(0, length).turn()


def fin_outline(shape: str, root: float, span: float) -> list[tuple[float, float]]:
    """A fin in its own plane: x out from the tube, z along it, root on x = 0 from z = 0."""
    if shape == "trapezoid":
        return [(0, 0), (span, 0.05 * root), (span, 0.55 * root), (0, root)]
    if shape == "swept":
        return [(0, 0.10 * root), (span, -0.25 * root), (span, 0.20 * root), (0, root)]
    if shape == "delta":
        return [(0, 0), (span, -0.05 * root), (0.25 * span, root * 0.85), (0, root)]
    # elliptic: a run of points round a half ellipse
    pts = [(0, 0)]
    for i in range(1, 12):
        a = math.pi * i / 12
        pts.append((span * math.sin(a), root / 2 - root / 2 * math.cos(a)))
    pts.append((0, root))
    return pts


def fin(shape: str, root: float, span: float, thick: float) -> Solid:
    pts = [Vector(x, -thick / 2, z) for x, z in fin_outline(shape, root, span)]
    if shape == "elliptic":
        wire = Wire([Edge.make_spline(pts), Edge.make_line(pts[-1], pts[0])])
    else:
        wire = Wire([Edge.make_line(a, b) for a, b in zip(pts, pts[1:] + pts[:1])])
    return extrude(Face(wire), thick, dir=(0, 1, 0))


def rocket(name: str, length: float, d: float, nose_kind: str, count: int, shape: str) -> list:
    r = d / 2
    wall = max(0.5, 0.03 * d)
    nose_len = 3.0 * d if nose_kind != "cone" else 2.4 * d
    tube_len = length - nose_len
    root = 0.30 * tube_len if shape != "swept" else 0.24 * tube_len
    span = 1.05 * d
    lift = 0.25 * root if shape == "swept" else 0.0  # swept fins reach below the tube
    lift += 2.0
    tube = Solid.make_cylinder(r, tube_len) - Solid.make_cylinder(r - wall, tube_len + 2).moved(Location((0, 0, -1)))
    body_tube = part(tube.moved(Location((0, 0, lift))), "body_tube")
    cone = part(nose(nose_kind, r, nose_len).moved(Location((0, 0, lift + tube_len))), "nose_cone")
    one_fin = fin(shape, root, span, max(1.0, 0.05 * d)).moved(Location((r - 0.2, 0, lift)))
    fins = [bodies("fins", [at(one_fin, rz=90 + i * 360 / count) for i in range(count)])]
    lug_r = max(1.0, 0.11 * d)
    lug_angle = math.radians(90 + 180 / count)
    lug = Solid.make_cylinder(lug_r, 0.35 * tube_len) - Solid.make_cylinder(0.6 * lug_r, 0.4 * tube_len).moved(
        Location((0, 0, -0.02 * tube_len)))
    launch_lug = part(lug.moved(Location(((r + lug_r) * math.cos(lug_angle), (r + lug_r) * math.sin(lug_angle),
                                          lift + 0.40 * tube_len))), "launch_lug")
    motor_len = min(0.55 * tube_len, 70)
    motor_r = r - wall - 0.4
    the_motor = (Profile(0, 0).line(0.55 * motor_r, 0).line(0.70 * motor_r, 0.06 * motor_len)
                 .line(motor_r, 0.08 * motor_len).line(motor_r, motor_len).line(0, motor_len).turn())
    motor = part(the_motor.moved(Location((0, 0, lift - 0.04 * motor_len))), "motor")
    return [body_tube, cone] + fins + [launch_lug, motor]


def files():
    for i, spec in enumerate(ROCKETS):
        yield f"{CHAPTER}/model_rockets/{i + 1:03d} {spec[0]}.step", assembly(spec[0].lower(), rocket(*spec))
