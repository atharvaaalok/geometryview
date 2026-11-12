"""07 Wheels and Pinions: a clockmaker's drawer of brass wheels and steel pinions.

Wheels are module 1, 24 to 68 teeth, crossed out with five spokes round a collet. Pinions have 6
to 18 leaves, cut on their arbors, with pivots at both ends.
"""

from __future__ import annotations

import math

from build123d import Circle, Location, Rot, Solid, SlotCenterToCenter, extrude

from common import part
from shapes import Profile, involute_gear

CHAPTER = "07_wheels_and_pinions"
MODULE = 1.0


def wheel(teeth: int) -> Solid:
    thick = 1.6
    gear = involute_gear(teeth, MODULE, thick)
    r_root = MODULE * teeth / 2 - 1.25 * MODULE
    r_rim = r_root - 1.6
    r_hub = max(3.2, 0.20 * r_root)
    # The crossing: five windows between spokes, rim and hub.
    window = Solid.make_cylinder(r_rim, thick * 3) - Solid.make_cylinder(r_hub + 1.2, thick * 3)
    spoke_w = max(1.2, 0.11 * r_root)
    for i in range(5):
        spoke = Solid.make_box(r_rim * 1.2, spoke_w, thick * 3).moved(Location((0, -spoke_w / 2, 0)))
        window = window - spoke.moved(Rot(Z=90 + i * 72))
    gear = gear - window.moved(Location((0, 0, -thick)))
    # The collet: a turned boss on one face, the arbor's hole through it.
    collet = (Profile(0, 0).line(r_hub, 0).line(r_hub, 0.6).spline((0.75 * r_hub, 1.6), (0.62 * r_hub, 3.0))
              .line(0, 3.0).turn()).moved(Location((0, 0, thick)))
    return gear + collet - Solid.make_cylinder(0.9, 20).moved(Location((0, 0, -5)))


def pinion(leaves: int) -> Solid:
    """Leaves with round ends on an arbor, the pinion a third of the way along."""
    pitch_r = MODULE * leaves / 2 * 1.05
    leaf_w = 0.95 * math.pi * pitch_r / leaves
    core = Circle(pitch_r - 1.2 * MODULE)
    shape = core
    for i in range(leaves):
        leaf = SlotCenterToCenter(1.3 * MODULE, leaf_w).moved(Location((pitch_r - 0.65 * MODULE, 0, 0)))
        shape = shape + leaf.moved(Rot(Z=i * 360 / leaves))
    length = 18.0 + 1.4 * leaves
    face_w = max(2.5, 0.22 * length)
    leaves_part = extrude(shape, face_w).moved(Location((0, 0, 0.30 * length)))
    arbor_r = max(0.9, 0.42 * (pitch_r - 1.2 * MODULE))
    arbor = (Profile(0, 0).line(0.45 * arbor_r, 0).line(0.45 * arbor_r, 0.10 * length)
             .line(arbor_r, 0.13 * length).line(arbor_r, 0.87 * length).line(0.45 * arbor_r, 0.90 * length)
             .line(0.45 * arbor_r, length).line(0, length).turn())
    # A shoulder where a wheel would sit, above the leaves.
    seat = Solid.make_cylinder(1.6 * arbor_r, 1.2).moved(Location((0, 0, 0.30 * length + face_w + 2.5)))
    return arbor + leaves_part + seat


WHEELS = [24 + 4 * i for i in range(12)]
PINIONS = [6, 7, 8, 10, 12, 14, 16, 18]


def files():
    for i, n in enumerate(PINIONS):
        yield f"{CHAPTER}/pinions/{i + 1:02d} Pinion {n} leaves.step", part(pinion(n), f"pinion_{n}")
    for i, n in enumerate(WHEELS):
        yield f"{CHAPTER}/wheels/{i + 1:02d} Wheel {n} teeth.step", part(wheel(n), f"wheel_{n}")
