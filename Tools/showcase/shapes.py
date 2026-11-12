"""Shapes the showcase builds from: turned profiles, thin-walled vessels, gears.

A turned part is a profile in the XZ plane, radius against height, revolved about Z. Curves are
splines rather than many short lines, so a curved surface is one smooth face: GeometryView draws
an ink line at every edge between faces, and a profile of short lines would draw rings.
"""

from __future__ import annotations

import math

from build123d import Axis, Edge, Face, Location, Pos, Rot, Solid, Vector, Wire, extrude


def P(r: float, z: float) -> Vector:
    return Vector(r, 0, z)


class Profile:
    """A closed profile for turning: starts on the axis, ends back on it.

        Profile(0, 0).line(10, 0).spline((6, 6), (4, 12), (5, 16)).line(0, 16).turn()
    """

    def __init__(self, r: float, z: float):
        self.start = P(r, z)
        self.at = self.start
        self.edges: list[Edge] = []

    def line(self, r: float, z: float) -> "Profile":
        end = P(r, z)
        if (end - self.at).length > 1e-9:
            self.edges.append(Edge.make_line(self.at, end))
        self.at = end
        return self

    def spline(self, *points: tuple[float, float], start_tangent=None, end_tangent=None) -> "Profile":
        """A smooth curve through the points, from where the profile is."""
        pts = [self.at] + [P(r, z) for r, z in points]
        if start_tangent is not None or end_tangent is not None:
            tangents = [P(*start_tangent) if start_tangent else (pts[1] - pts[0]),
                        P(*end_tangent) if end_tangent else (pts[-1] - pts[-2])]
            self.edges.append(Edge.make_spline(pts, tangents=tangents))
        else:
            self.edges.append(Edge.make_spline(pts))
        self.at = pts[-1]
        return self

    def arc(self, through: tuple[float, float], end: tuple[float, float]) -> "Profile":
        """A circular arc through one point to another."""
        stop = P(*end)
        self.edges.append(Edge.make_three_point_arc(self.at, P(*through), stop))
        self.at = stop
        return self

    def ball(self, radius: float, top: float, neck: float) -> "Profile":
        """A ball of `radius` whose top is at `top`, joined where its surface is `neck` from the
        axis: a line to that point, then the arc over the top to the axis."""
        centre = top - radius
        neck = min(neck, radius * 0.98)
        z = centre - (radius * radius - neck * neck) ** 0.5
        return self.line(neck, z).arc((radius, centre), (0, top))

    def face(self) -> Face:
        if (self.at - self.start).length > 1e-9:
            self.edges.append(Edge.make_line(self.at, self.start))
            self.at = self.start
        return Face(Wire(self.edges))

    def turn(self, angle: float = 360) -> Solid:
        return Solid.revolve(self.face(), angle, Axis.Z)


def offset_points(points: list[tuple[float, float]], t: float) -> list[tuple[float, float]]:
    """A curve's points moved t inward (toward the axis side of its direction of travel)."""
    out = []
    for i, (r, z) in enumerate(points):
        a = points[max(i - 1, 0)]
        b = points[min(i + 1, len(points) - 1)]
        dr, dz = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dr, dz) or 1.0
        # Travelling up the outside of a wall, the inside is to the left: (-dz, dr) rotated.
        nr, nz = dz / length, -dr / length
        out.append((r - nr * t, z - nz * t))
    return out


def vessel(wall: list[tuple[float, float]], t: float, floor: float, rim: float | None = None) -> Solid:
    """A thin-walled vessel turned from its outside: `wall` runs from the foot's edge, on z = 0, up
    to the rim. The inside is the wall moved in by t over a floor `floor` thick, open at the top.
    A rim bead of radius `rim` rounds the lip."""
    outer = Profile(0, 0).line(*wall[0]).spline(*wall[1:]).line(0, wall[-1][1]).turn()
    inner_wall = offset_points(wall, t)
    top = wall[-1][1]
    inner_pts = [(r, z) for r, z in inner_wall if z > floor + t * 0.5]
    first = inner_pts[0]
    inner = (Profile(0, floor).line(first[0], max(first[1], floor))
             .spline(*inner_pts[1:]).line(inner_pts[-1][0], top + 5).line(0, top + 5).turn())
    shape = outer - inner
    if rim:
        r_top = (wall[-1][0] + inner_wall[-1][0]) / 2
        shape = shape + Solid.make_torus(r_top, rim).moved(Location((0, 0, top - rim * 0.4)))
    return shape


def involute_gear(teeth: int, module: float, thickness: float, bore: float = 0.0,
                  addendum: float = 1.0, dedendum: float = 1.25) -> Solid:
    """A spur gear standing on z = 0: involute flanks as splines, tips and roots as arcs."""
    pressure = math.radians(20)
    r_pitch = module * teeth / 2
    r_base = r_pitch * math.cos(pressure)
    r_tip = r_pitch + addendum * module
    r_root = max(r_pitch - dedendum * module, 0.5 * module)

    def involute(r: float) -> float:
        # The angle along the involute where it reaches radius r.
        if r <= r_base:
            return 0.0
        a = math.acos(r_base / r)
        return math.tan(a) - a

    # Half a tooth's angular width at the pitch circle, and the flank's angle there.
    half = math.pi / (2 * teeth)
    flank_pitch = involute(r_pitch)

    def flank(sign: int, centre: float) -> list[Vector]:
        pts = []
        r0 = max(r_base, r_root)
        for k in range(7):
            r = r0 + (r_tip - r0) * k / 6
            angle = centre + sign * (half + flank_pitch - involute(r))
            pts.append(Vector(r * math.cos(angle), r * math.sin(angle), 0))
        return pts

    edges: list[Edge] = []
    pitch = 2 * math.pi / teeth
    for i in range(teeth):
        c = i * pitch
        right = flank(-1, c)  # from root to tip
        left = list(reversed(flank(1, c)))  # from tip to root
        if r_root < r_base:
            a = math.atan2(right[0].Y, right[0].X)
            right.insert(0, Vector(r_root * math.cos(a), r_root * math.sin(a), 0))
            b = math.atan2(left[-1].Y, left[-1].X)
            left.append(Vector(r_root * math.cos(b), r_root * math.sin(b), 0))
        edges.append(Edge.make_spline(right))
        tip_mid = Vector(r_tip * math.cos(c), r_tip * math.sin(c), 0)
        edges.append(Edge.make_three_point_arc(right[-1], tip_mid, left[0]))
        edges.append(Edge.make_spline(left))
        # The root, to the next tooth's first point.
        nc = (i + 1) * pitch
        next_right = flank(-1, nc)
        if r_root < r_base:
            a = math.atan2(next_right[0].Y, next_right[0].X)
            next_start = Vector(r_root * math.cos(a), r_root * math.sin(a), 0)
        else:
            next_start = next_right[0]
        mid_angle = c + pitch / 2
        root_mid = Vector(r_root * math.cos(mid_angle), r_root * math.sin(mid_angle), 0)
        edges.append(Edge.make_three_point_arc(left[-1], root_mid, next_start))
    gear = extrude(Face(Wire(edges)), thickness)
    if bore > 0:
        gear = gear - Solid.make_cylinder(bore / 2, thickness * 3).moved(Location((0, 0, -thickness)))
    return gear


def ring(r_outer: float, r_inner: float, height: float, z: float = 0.0) -> Solid:
    """A flat ring, or a disc if r_inner is 0."""
    shape = Solid.make_cylinder(r_outer, height)
    if r_inner > 0:
        shape = shape - Solid.make_cylinder(r_inner, height * 3).moved(Location((0, 0, -height)))
    return shape.moved(Location((0, 0, z)))


def at(shape: Solid, x: float = 0, y: float = 0, z: float = 0, rx: float = 0, ry: float = 0,
       rz: float = 0) -> Solid:
    """A shape placed: turned about X, then Y, then Z, then moved. The copy shares its geometry,
    so a STEP file writes it once as an instance."""
    return shape.moved(Pos(x, y, z) * Rot(Z=rz) * Rot(Y=ry) * Rot(X=rx))
