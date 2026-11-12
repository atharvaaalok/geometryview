"""TF-1: a two-spool turbofan on its stand, the inlet toward the viewer.

Built along Z, then laid along Y, so the usual cut (x0.5) opens the whole gas path: the fan, a
three-stage booster, a five-stage high-pressure compressor, the annular combustor, two
high-pressure and three low-pressure turbine stages, and the exhaust plug, on two nested shafts
and three bearings. Each blade row is one part of many lofted blades, so a row takes one colour
and the rows stripe the engine from front to back.
"""

from __future__ import annotations

import math

from build123d import Edge, Pos, Rot, Solid, Vector, Wire

from common import assembly, bodies, part
from shapes import Profile, at, ring

CUT = "x0.5"
LEVEL = 2
AXIS = 480.0  # height of the engine's axis above the floor


def section(r: float, chord: float, thick: float, stagger: float, z: float,
            camber: float = 0.05, n: int = 6) -> Wire:
    """An airfoil at radius r, its chord turned `stagger` degrees from the axis."""
    # Round at both edges (a sine's thickness), so the closed spline never folds at the trailing
    # edge, which a loft cannot pass through.
    pts = []
    for k in range(n + 1):
        s = k / n
        half = 0.5 * thick * math.sin(math.pi * s) ** 0.7
        pts.append(((s - 0.5) * chord, camber * chord * 4 * s * (1 - s) + half))
    for k in range(n - 1, 0, -1):
        s = k / n
        half = 0.5 * thick * math.sin(math.pi * s) ** 0.7
        pts.append(((s - 0.5) * chord, camber * chord * 4 * s * (1 - s) - half))
    st = math.radians(stagger)
    along = Vector(0, math.sin(st), math.cos(st))
    across = Vector(0, math.cos(st), -math.sin(st))
    points = [Vector(r, 0, z) + along * u + across * v for u, v in pts]
    return Wire([Edge.make_spline(points, periodic=True)])


def blade(z: float, r0: float, r1: float, c0: float, c1: float, t0: float, t1: float,
          s0: float, s1: float) -> Solid:
    """One blade, root sunk a little into the hub, lofted through three airfoils."""
    wires = [section(r0 - 3, c0, t0, s0, z),
             section((r0 + r1) / 2, (c0 + c1) / 2, (t0 + t1) / 2, (s0 + s1) / 2, z),
             section(r1, c1, t1, s1, z)]
    return Solid.make_loft(wires)


def row(name: str, count: int, z: float, r0: float, r1: float, chord: float, stator: bool = False,
        thick: float | None = None) -> Solid:
    """A row of blades (or vanes) round the axis, as one part of many bodies."""
    t = thick if thick is not None else max(1.6, 0.12 * chord)
    if stator:
        one = blade(z, r0, r1, chord, chord, t, t, -18, -12)
    else:
        # A core blade turns 8 degrees from root to tip: a loft through short airfoils twisted
        # much more folds on itself.
        one = blade(z, r0, r1, chord, chord * 1.1, t, 0.6 * t, 34, 42)
    return bodies(name, [at(one, rz=i * 360 / count) for i in range(count)])


def fan() -> list:
    spinner = Profile(0, -478).spline((40, -455), (78, -410), (96, -372)).line(96, -360).line(0, -360).turn()
    disk = Profile(0, -372).line(96, -372).line(100, -330).line(108, -300).line(40, -300).line(40, -280) \
        .line(0, -280).turn()
    blades = bodies("fan_blades", [at(blade(-352, 98, 300, 78, 118, 10, 3.5, 22, 62), rz=i * 18)
                                   for i in range(20)])
    ogv = row("outlet_guide_vanes", 36, -248, 206, 302, 42, stator=True, thick=4)
    return [part(spinner, "spinner"), blades, part(disk, "fan_disk"), ogv]


# Hub and tip radii of the core's gas path, front to back.
BOOSTER = [(-276, 112, 172), (-240, 114, 170), (-204, 117, 167)]
HPC = [(-150, 121, 161), (-110, 125, 158), (-70, 129, 155), (-30, 133, 152), (10, 137, 149)]
HPT = [(214, 136, 168), (248, 134, 172)]
LPT = [(300, 126, 184), (345, 122, 195), (392, 118, 206)]


def booster() -> list:
    out = []
    for i, (z, hub, tip) in enumerate(BOOSTER, 1):
        out.append(row(f"booster_blades_{i}", 40 + 4 * i, z, hub, tip, 20))
        out.append(row(f"booster_vanes_{i}", 50 + 4 * i, z + 17, hub + 2, tip, 16, stator=True))
    drum = Profile(84, -296).line(112, -296).line(117, -190).line(84, -190).turn()
    out.append(part(drum, "booster_drum"))
    return out


def hp_compressor() -> list:
    out = []
    for i, (z, hub, tip) in enumerate(HPC, 1):
        out.append(row(f"hpc_blades_{i}", 34 + 5 * i, z, hub, tip, 22))
        out.append(row(f"hpc_vanes_{i}", 42 + 5 * i, z + 20, hub + 2, tip, 18, stator=True))
    # The drum and a disc under each blade row.
    drum = Profile(108, -165).line(121, -165).line(138, 40).line(126, 40).turn()
    discs = [ring(hub - 2, 58, 9, z - 4.5) for z, hub, _ in HPC]
    for d in discs:
        drum = drum + d
    out.append(part(drum, "hpc_drum"))
    out.append(part(Profile(163, -300).line(176, -300).line(176, 48).line(152, 48).line(152, 40)
                    .line(161, -170).line(163, -300).turn(), "compressor_housing"))
    return out


def combustor() -> list:
    """The annular combustor: dome, outer and inner walls, eighteen fuel nozzles, two igniters."""
    outer = Profile(171, 58).line(176, 58).line(176, 192).line(171, 192).turn()
    inner = Profile(116, 58).line(121, 58).line(121, 192).line(116, 192).turn()
    dome = Profile(121, 58).spline((146, 50), (171, 58)).line(171, 63).spline((146, 56), (121, 63)).turn()
    nozzle = Solid.make_cylinder(4, 26).moved(Pos(146, 0, 34))
    nozzles = bodies("fuel_nozzles", [at(nozzle, rz=i * 20) for i in range(18)])
    igniter = Solid.make_cylinder(3.5, 46).moved(Pos(0, 0, 0) * Rot(Y=90)).moved(Pos(160, 0, 110))
    igniters = bodies("igniters", [at(igniter, rz=a) for a in (100, 260)])
    casing = Profile(178, 48).line(186, 48).line(188, 200).line(180, 200).turn()
    return [part(dome, "combustor_dome"), part(outer, "combustor_outer_wall"),
            part(inner, "combustor_inner_wall"), nozzles, igniters, part(casing, "combustor_casing")]


def hp_turbine() -> list:
    out = [row("hpt_nozzle_vanes", 40, 198, 134, 170, 18, stator=True)]
    for i, (z, hub, tip) in enumerate(HPT, 1):
        out.append(row(f"hpt_blades_{i}", 46 + 6 * i, z, hub, tip, 16))
        if i == 1:
            out.append(row("hpt_vanes_2", 44, 231, 134, 171, 15, stator=True))
        out.append(part(ring(hub - 1, 52, 14, z - 7) + ring(70, 52, 34, z - 17), f"hpt_disk_{i}"))
    out.append(part(Profile(172, 190).line(180, 190).line(186, 268).line(176, 268).turn(), "turbine_housing"))
    return out


def lp_turbine() -> list:
    out = []
    for i, (z, hub, tip) in enumerate(LPT, 1):
        out.append(row(f"lpt_vanes_{i}", 52 + 4 * i, z - 17, hub + 2, tip, 18, stator=True))
        out.append(row(f"lpt_blades_{i}", 58 + 6 * i, z, hub, tip, 20))
        out.append(part(ring(hub - 1, 36, 12, z - 6), f"lpt_disk_{i}"))
    out.append(part(Profile(186, 270).line(194, 270).line(214, 420).line(206, 420).turn(), "lpt_housing"))
    return out


def spools() -> list:
    lp = Profile(22, -300).line(30, -300).line(30, 405).line(22, 405).turn()
    hp = Profile(40, -160).line(52, -160).line(52, 255).line(40, 255).turn()
    out = [part(lp, "lp_shaft"), part(hp, "hp_shaft")]
    for i, (z, r_in, r_out) in enumerate([(-312, 30, 48), (30, 52, 74), (412, 30, 48)], 1):
        ball_r = (r_out - r_in) * 0.22
        mid = (r_in + r_out) / 2
        inner_ring = ring(mid - ball_r * 0.6, r_in, 2 * ball_r, z - ball_r)
        outer_ring = ring(r_out, mid + ball_r * 0.6, 2 * ball_r, z - ball_r)
        ball = Solid.make_sphere(ball_r).moved(Pos(mid, 0, z))
        out.append(assembly(f"bearing_{i}", [part(outer_ring, "outer_ring"), part(inner_ring, "inner_ring"),
                                             bodies("balls", [at(ball, rz=k * 360 / 14) for k in range(14)])]))
    return out


def nacelle() -> list:
    """The nacelle round the fan and bypass duct, and the core cowl round the core."""
    shell = (Profile(298, -562).spline((318, -574), (334, -556), (340, -500))
             .spline((342, -380), (334, -150), (318, 250)).line(300, 250)
             .spline((292, 60), (300, -230)).line(304, -230).line(304, -470)
             .spline((296, -520), (298, -562)).turn())
    fan_case = Profile(304, -470).line(312, -470).line(312, -230).line(304, -230).turn()
    cowl = (Profile(178, -300).spline((196, -280), (206, -200)).spline((210, 0), (204, 260), (186, 430))
            .line(178, 430).spline((196, 260), (202, 0), (196, -200)).line(178, -300).turn())
    return [part(shell, "nacelle_shell"), part(fan_case, "fan_case"), part(cowl, "core_cowl_shell")]


def exhaust() -> list:
    plug = Profile(0, 420).line(118, 420).spline((108, 470), (70, 520), (0, 565)).turn()
    nozzle = Profile(204, 420).line(212, 420).line(190, 478).line(182, 478).turn()
    return [part(plug, "exhaust_cone"), part(nozzle, "core_nozzle_shell")]


def stand() -> list:
    """A cradle under the nacelle: two saddles on a base of rails, on four feet."""
    floor = -AXIS
    rail = Solid.make_box(40, 900, 40).moved(Pos(-20, -450, 0))
    base = (rail.moved(Pos(-210, 0, floor + 30)) + rail.moved(Pos(210, 0, floor + 30))
            + Solid.make_box(460, 40, 40).moved(Pos(-230, -300, floor + 30))
            + Solid.make_box(460, 40, 40).moved(Pos(-230, 260, floor + 30)))
    saddle = Solid.make_box(380, 50, AXIS - 200).moved(Pos(-190, -25, floor + 70))
    saddle = saddle - Solid.make_cylinder(345, 80).moved(Pos(0, 0, 0) * Rot(X=90)).moved(Pos(0, 40, 0))
    saddles = bodies("saddles", [saddle.moved(Pos(0, y, 0)) for y in (-280, 120)])
    foot = Solid.make_cylinder(26, 30).moved(Pos(0, 0, floor))
    feet = bodies("stand_feet", [foot.moved(Pos(x, y, 0)) for x in (-210, 210) for y in (-420, 420)])
    return [part(base, "stand_base"), saddles, feet]


def build():
    engine = [
        assembly("nacelle", nacelle()),
        assembly("fan", fan()),
        assembly("booster", booster()),
        assembly("hp_compressor", hp_compressor()),
        assembly("combustor", combustor()),
        assembly("hp_turbine", hp_turbine()),
        assembly("lp_turbine", lp_turbine()),
        assembly("spools", spools()),
        assembly("exhaust", exhaust()),
    ]
    # Laid along Y, the inlet at -Y, the axis AXIS above the floor.
    lay = Pos(0, 0, AXIS) * Rot(X=-90)
    placed = [_moved(a, lay) for a in engine]
    the_stand = assembly("engine_stand", [_moved(p, Pos(0, 0, AXIS)) for p in stand()])
    return assembly("tf1_turbofan", placed + [the_stand])


def _moved(shape, location):
    """A part or assembly moved, its tree and names kept: assemblies are rebuilt from their
    children moved, since a STEP file is written from the tree."""
    kids = getattr(shape, "children", None)
    if kids:
        return assembly(shape.label, [_moved(k, location) for k in kids])
    moved = shape.moved(location)
    moved.label = shape.label
    return moved
