"""LN-50: a 50 mm f/1.4 camera lens, standing on its mount, the front element up.

The optics are a double Gauss, seven elements in six groups, the iris between the two halves. Cut
along its axis it is the cross-section every lens maker prints; in see-through its housing turns
to glass over the helicoid and the iris.
"""

from __future__ import annotations

import math

from build123d import Axis, Location, Solid

from common import assembly, bodies, part
from shapes import Profile, at, ring

CUT = "x0.5"
LEVEL = 2

# Each element: name, semi-diameter, the edge band's bottom and top, and the sag of the back
# (rear-facing) and front surfaces at the edge: positive bulges outward, negative is hollowed.
ELEMENTS = [
    ("lens_element_1", 17.0, 48.5, 51.5, -1.2, 3.5),
    ("lens_element_2", 15.0, 42.5, 45.5, -1.0, 3.0),
    ("lens_element_3", 13.0, 37.0, 41.5, -4.0, 0.8),
    ("lens_element_4", 12.5, 25.0, 30.0, 0.5, -3.5),
    ("lens_element_5", 13.5, 19.5, 24.9, 3.0, -0.5),
    ("lens_element_6", 14.5, 11.0, 15.0, 2.0, 1.0),
    ("lens_element_7", 13.0, 5.0, 7.0, 0.0, 1.5),
]


def element(a: float, z0: float, z1: float, back: float, front: float) -> Solid:
    """A lens element turned from its section: two near-spherical faces and a ground edge."""
    p = Profile(0, z0 - back)
    if abs(back) > 1e-6:
        p.arc((0.6 * a, z0 - 0.64 * back), (a, z0))
    else:
        p.line(a, z0)
    p.line(a, z1)
    if abs(front) > 1e-6:
        p.arc((0.6 * a, z1 + 0.64 * front), (0, z1 + front))
    else:
        p.line(0, z1)
    return p.turn()


def optics() -> list:
    glass = [part(element(a, z0, z1, b, f), name) for name, a, z0, z1, b, f in ELEMENTS]
    spacers = bodies("element_spacers", [ring(15.6, 14.2, 1.2, 47.0), ring(14.6, 13.2, 3.2, 15.8),
                                         ring(13.3, 11.6, 2.4, 7.4)])
    retaining = bodies("retaining_rings", [ring(18.5, 15.4, 1.4, 52.4), ring(13.3, 10.6, 1.0, 3.8)])
    # The optics barrel: a stepped tube holding each element by its edge.
    cell = (Profile(19.0, 5.0).line(21.0, 5.0).line(21.0, 54.5).line(18.6, 54.5).line(18.6, 48.0)
            .line(17.2, 48.0).line(17.2, 42.0).line(15.4, 42.0).line(15.4, 36.5).line(14.8, 36.5)
            .line(14.8, 19.0).line(14.8, 10.5).line(13.4, 10.5).line(13.4, 6.0).line(19.0, 6.0)
            .face())
    barrel = Solid.revolve(cell, 360, Axis.Z)
    # Windows in the cell at the iris, where its blades pass.
    barrel = barrel - ring(22, 13, 3.6, 33.0)
    return glass + [spacers, retaining, part(barrel, "optics_barrel")]


def iris() -> list:
    """Nine curved blades between a cam ring and a base ring."""
    r_in, r_out = 6.5, 17.5
    # A crescent: a disc's edge with an offset disc taken out, kept to one sector.
    cutter = Solid.make_cylinder(r_out * 0.92, 0.6).moved(Location((-r_out * 0.55, r_out * 0.32, -0.2)))
    blade = (Solid.make_cylinder(r_out, 0.25) - cutter) & Solid.make_box(r_out, r_out * 0.62, 0.25)
    blade = blade - Solid.make_cylinder(r_in, 1).moved(Location((0, 0, -0.5)))
    blades = bodies("aperture_blades", [at(blade, z=34.2 + 0.12 * (i % 3), rz=i * 40) for i in range(9)])
    cam = part(ring(19.0, 15.0, 0.8, 33.2), "iris_cam_ring")
    base = part(ring(19.0, 15.0, 0.8, 35.6), "iris_base")
    return [blades, cam, base]


def focus() -> list:
    """The helicoid as two tubes with interleaved threads, and the grooved grip over them."""
    inner = Solid.make_cylinder(24.0, 30.0) - Solid.make_cylinder(21.2, 32.0).moved(Location((0, 0, -1)))
    outer = Solid.make_cylinder(26.6, 30.0) - Solid.make_cylinder(24.3, 32.0).moved(Location((0, 0, -1)))
    for i in range(4):
        z = 4.0 + 6.4 * i
        inner = inner + ring(25.2, 23.5, 2.6, z)
        outer = outer - ring(25.35, 23.0, 3.2, z - 0.3)
    helicoid_inner = part(inner.moved(Location((0, 0, 14.0))), "focus_helicoid_inner")
    helicoid_outer = part(outer.moved(Location((0, 0, 14.0))), "focus_helicoid_outer")
    # The grip: a rubber band, chamfered top and bottom, its face cut with fine axial grooves.
    grip = (Profile(29.6, 28.6).line(31.6, 28.6).line(32.6, 29.6).line(32.6, 43.4).line(31.6, 44.4)
            .line(29.6, 44.4).face())
    grip = Solid.revolve(grip, 360, Axis.Z)
    groove = Solid.make_box(1.2, 1.0, 12.4).moved(Location((32.0, -0.5, 30.3)))
    for i in range(60):
        grip = grip - at(groove, rz=i * 6)
    return [part(grip, "focus_grip"), helicoid_outer, helicoid_inner]


def barrel() -> list:
    housing = (Profile(29.0, 8.0).line(31.0, 8.0).line(31.0, 28.6).line(29.6, 28.6).line(29.6, 44.4)
               .line(31.0, 44.4).line(31.0, 51.6).line(29.0, 51.6).face())
    the_housing = Solid.revolve(housing, 360, Axis.Z)
    aperture = (Profile(31.05, 12.0).line(31.9, 12.0).line(32.4, 12.5).line(32.4, 18.0)
                .line(31.9, 18.5).line(31.05, 18.5).face())
    aperture = Solid.revolve(aperture, 360, Axis.Z)
    notch = Solid.make_box(1.4, 1.6, 3.2).moved(Location((31.6, -0.8, 12.0)))
    for i in range(18):
        aperture = aperture - at(notch, rz=i * 20)
    bezel = (Profile(26.0, 51.6).line(31.0, 51.6).line(31.0, 56.4).line(30.0, 58.0).line(26.0, 58.0)
             .line(26.0, 56.8).line(19.0, 56.0).line(19.0, 54.6).line(26.0, 54.6).face())
    the_bezel = Solid.revolve(bezel, 360, Axis.Z)
    return [part(the_housing, "barrel_housing"), part(aperture, "aperture_ring"),
            part(the_bezel, "front_bezel")]


def mount() -> list:
    flange = ring(29.0, 19.5, 3.0, 2.0) + ring(23.5, 19.5, 2.0, 0.0)
    lug = Solid.make_cylinder(26.0, 1.6, angle=38) - Solid.make_cylinder(23.4, 3.0).moved(Location((0, 0, -0.5)))
    for i in range(3):
        flange = flange + at(lug, rz=i * 120)
    screw = (Profile(0, 0).line(1.4, 0).line(1.4, 0.6).arc((1.0, 1.0), (0, 1.15)).turn())
    screw = screw - Solid.make_box(2.8, 0.35, 1.0).moved(Location((-1.4, -0.175, 0.5)))
    screws = bodies("mount_screws", [at(screw, x=26.2 * math.cos(math.radians(a)),
                                         y=26.2 * math.sin(math.radians(a)), z=5.0, rz=a)
                                      for a in (20, 110, 200, 290)])
    # The contact board on the mount's face, its pins standing up from it.
    pcb = (Solid.make_cylinder(25.2, 0.8) - Solid.make_cylinder(22.4, 1.0).moved(Location((0, 0, -0.1))))
    pcb = pcb & Solid.make_box(12, 12, 0.8).moved(Location((-6, -26.0, 0)))
    pins = [Solid.make_cylinder(0.45, 1.2).moved(Location((23.8 * math.cos(math.radians(a)),
                                                            23.8 * math.sin(math.radians(a)), 0.8)))
            for a in range(-104, -76, 4)]
    for pin in pins:
        pcb = pcb + pin
    return [part(flange, "bayonet_mount"), screws, part(pcb.moved(Location((0, 0, 5.0))), "contact_pcb")]


def build():
    return assembly("lens_50mm_f1_4", [
        assembly("barrel", barrel()),
        assembly("focus", focus()),
        assembly("mount", mount()),
        assembly("iris", iris()),
        assembly("optics", optics()),
    ])
