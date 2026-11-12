"""OR-1: a desk orrery, its planets turned by a motor and gear train hidden in the base.

The motor's shaft carries six pinions, each driving a wheel on one of six nested arbors: the sun's
shaft inside, then a tube for each planet, the outermost the shortest. Each tube ends in an arm
holding its planet. In see-through the base's case and lid turn to glass over the motor and the
gears; in material colours the coils, magnets, shafts, gears and circuit board each take their own.
"""

from __future__ import annotations

import math

from build123d import Pos, Rot, Solid

from common import assembly, bodies, part
from shapes import Profile, at, involute_gear, ring

CUT = ("x", 0.0)  # through the arbors' axis, wherever the arms put the bounds' middle
LEVEL = 2

BASE_R = 120.0
BASE_H = 100.0
CENTRES = 70.0  # from the arbors to the motor's shaft, which stands in front of them
MODULE = 2.0
GEAR_T = 5.0

# Planets, innermost arbor first: name, the tube's radii, wheel teeth, arm height, reach, angle,
# planet radius. Seen from the usual camera (azimuth 35, looking along 305 degrees), arms at 35 and
# 215 degrees lie across the picture and those near 305 point at the viewer, so the long arms
# reach sideways.
PLANETS = [
    ("mercury", 6.0, 8.0, 36, 200.0, 62.0, 330.0, 8.0),
    ("venus", 9.0, 11.0, 40, 180.0, 96.0, 260.0, 12.0),
    ("earth", 12.0, 14.0, 44, 160.0, 130.0, 125.0, 13.0),
    ("mars", 15.0, 17.0, 48, 140.0, 166.0, 200.0, 10.0),
    ("saturn", 18.0, 20.0, 52, 120.0, 206.0, 30.0, 20.0),
]
SUN_TEETH = 32
LAYER0 = 45.0  # the lowest gear's height; each next 8 mm up


def base() -> list:
    case = (Profile(BASE_R - 8, 0).line(BASE_R, 0).line(BASE_R, BASE_H - 6).line(BASE_R - 8, BASE_H - 6).turn())
    floor = Solid.make_cylinder(BASE_R - 8, 4)
    lid = (Profile(24, BASE_H - 6).line(BASE_R + 2, BASE_H - 6).line(BASE_R + 2, BASE_H - 2)
           .spline((BASE_R - 10, BASE_H + 2), (60, BASE_H + 5)).line(24, BASE_H + 5).turn())
    foot = Solid.make_cylinder(9, 6).moved(Pos(0, 0, -6))
    feet = bodies("feet", [foot.moved(Pos(92 * math.cos(math.radians(a)), 92 * math.sin(math.radians(a)), 0))
                           for a in (45, 135, 225, 315)])
    plate = (Solid.make_cylinder(BASE_R + 1.5, 26, angle=40) - Solid.make_cylinder(BASE_R, 30).moved(Pos(0, 0, -2)))
    plate = at(plate, z=28, rz=-110)
    return [part(case + floor, "base_case"), part(lid, "base_lid"), feet, part(plate, "name_plate")]


def drive_motor() -> list:
    """An outrunner under the gear train: stator and coils inside a bell of magnets."""
    y = -CENTRES  # in front of the arbors, on the plane the usual cut opens
    stator = Solid.make_cylinder(17, 22) - Solid.make_cylinder(5, 30).moved(Pos(0, 0, -2))
    tooth = Solid.make_box(8, 5, 22).moved(Pos(15, -2.5, 0))
    for i in range(9):
        stator = stator + at(tooth, rz=i * 40)
    coil = (Solid.make_box(7, 9, 18) - Solid.make_box(8.4, 5.4, 22).moved(Pos(-0.7, -2.7, -2))).moved(Pos(-3.5, -4.5, 0))
    coil = coil.moved(Pos(19, 0, 2))
    coils = bodies("coils", [at(coil, y=y, z=12, rz=i * 40) for i in range(9)])
    magnet = (Solid.make_cylinder(33, 20, angle=20) - Solid.make_cylinder(29, 24).moved(Pos(0, 0, -2)))
    magnets = bodies("magnets", [at(magnet, y=y, z=13, rz=i * 30) for i in range(12)])
    bell = (Profile(5, 8).line(38, 8).line(38, 36).line(33, 36).line(33, 12).line(5, 12).turn())
    shaft = Solid.make_cylinder(4, BASE_H - 6 - 6).moved(Pos(0, y, 6))
    pcb = ring(30, 6, 1.6, 6).moved(Pos(0, y, 0))
    return [part(at(stator, y=y, z=12), "stator_stack"), coils, magnets, part(at(bell, y=y), "rotor_bell"),
            part(shaft, "motor_shaft"), part(pcb, "motor_pcb")]


def gear_train() -> list:
    """A wheel on each arbor and its pinion on the motor's shaft, layer above layer."""
    out = []
    teeth = [SUN_TEETH] + [p[3] for p in PLANETS]
    names = ["sun"] + [p[0] for p in PLANETS]
    bores = [4.5] + [p[2] for p in PLANETS]
    for i, (n, name, bore) in enumerate(zip(teeth, names, bores)):
        z = LAYER0 + 8 * i
        wheel_r = MODULE * n / 2
        pinion_teeth = max(10, round(2 * (CENTRES - wheel_r) / MODULE))
        # The rim of teeth, a hub on the arbor, and four spokes between.
        wheel = involute_gear(n, MODULE, GEAR_T) - Solid.make_cylinder(wheel_r - 5, GEAR_T * 3).moved(
            Pos(0, 0, -GEAR_T))
        hub = bore + 5
        wheel = wheel + ring(hub, 0, GEAR_T)
        for k in range(4):
            wheel = wheel + at(Solid.make_box(wheel_r - 4, 4, GEAR_T).moved(Pos(0, -2, 0)), rz=45 + k * 90)
        wheel = wheel - Solid.make_cylinder(bore, GEAR_T * 3).moved(Pos(0, 0, -GEAR_T))
        # In mesh: a wheel's tooth points at the pinion (-90 degrees) where the pinion has a gap.
        out.append(part(at(wheel, z=z, rz=-90), f"{name}_gear"))
        pinion = involute_gear(pinion_teeth, MODULE, GEAR_T, bore=8)
        out.append(part(at(pinion, y=-CENTRES, z=z, rz=90 + 180 / pinion_teeth), f"pinion_{i + 1}"))
    return out


def arbors() -> list:
    """The sun's shaft and the planets' tubes, nested; each runs from its wheel to its arm."""
    out = [part(Solid.make_cylinder(4.5, 236 - LAYER0).moved(Pos(0, 0, LAYER0)), "sun_shaft")]
    for i, (name, r_in, r_out, _, arm_z, *_rest) in enumerate(PLANETS, 1):
        z0 = LAYER0 + 8 * i
        tube = ring(r_out, r_in, arm_z + 6 - z0, z0)
        out.append(part(tube, f"{name}_arbor"))
    return out


def arms_and_bodies() -> tuple[list, list]:
    arms, worlds = [], []
    for name, r_in, r_out, _, arm_z, reach, angle, radius in PLANETS:
        # A collar on the planet's own tube, and the bar out from the collar, clear of the tubes inside.
        collar = ring(r_out + 4, r_out, 8, arm_z - 1)
        bar = Solid.make_box(reach - r_out - 2, 7, 4).moved(Pos(r_out + 2, -3.5, arm_z + 1))
        post = Solid.make_cylinder(2.2, 22).moved(Pos(reach, 0, arm_z + 4))
        cup = Solid.make_cylinder(4, 2).moved(Pos(reach, 0, arm_z + 24))
        arm = collar + at(bar + post + cup, rz=angle)
        arms.append(part(arm, f"{name}_arm"))
        cx = reach * math.cos(math.radians(angle))
        cy = reach * math.sin(math.radians(angle))
        cz = arm_z + 26 + radius
        worlds.append(part(Solid.make_sphere(radius).moved(Pos(cx, cy, cz)), name))
        if name == "earth":
            # The moon's arm leaves the earth's post below the planet.
            moon_arm = Solid.make_box(26, 3, 2).moved(Pos(2.2, -1.5, 0))
            moon_arm = at(moon_arm + Solid.make_cylinder(1.2, 22).moved(Pos(28, 0, 0)), rz=angle + 50)
            arms.append(part(moon_arm.moved(Pos(cx, cy, arm_z + 12)), "moon_arm"))
            mx = cx + 28 * math.cos(math.radians(angle + 50))
            my = cy + 28 * math.sin(math.radians(angle + 50))
            worlds.append(part(Solid.make_sphere(3.5).moved(Pos(mx, my, arm_z + 12 + 22 + 3.5)), "moon"))
        if name == "saturn":
            rings = ring(radius * 1.9, radius * 1.3, 1.6, -0.8)
            worlds.append(part(rings.moved(Pos(0, 0, 0) * Rot(X=22)).moved(Pos(cx, cy, cz)), "saturn_rings"))
    worlds.insert(0, part(Solid.make_sphere(30).moved(Pos(0, 0, 236 + 30)), "sun"))
    return arms, worlds


def build():
    arms, worlds = arms_and_bodies()
    return assembly("orrery", [
        assembly("base", base()),
        assembly("bodies", worlds),
        assembly("arms", arms),
        assembly("arbors", arbors()),
        assembly("gear_train", gear_train()),
        assembly("drive_motor", drive_motor()),
    ])
