"""03 Glassware: Griffin beakers and Erlenmeyer flasks, by what they hold.

Sizes follow the usual laboratory catalogue: a 250 ml beaker is about 70 mm across and 95 mm
tall; a 250 ml flask stands 145 mm on an 85 mm base.
"""

from __future__ import annotations

from build123d import Location, Plane, Solid

from common import part
from shapes import Profile

CHAPTER = "03_glassware"

# Capacity (ml), outside diameter, height (mm).
BEAKERS = [(25, 34, 50), (50, 42, 60), (100, 50, 70), (250, 70, 95), (400, 80, 110),
           (600, 90, 125), (1000, 105, 145)]
# Capacity (ml), base diameter, height, neck diameter (mm).
FLASKS = [(25, 42, 70, 22), (50, 51, 85, 22), (100, 64, 105, 22), (250, 85, 145, 34),
          (500, 105, 180, 34), (1000, 131, 220, 42)]


def beaker(d: float, h: float) -> Solid:
    R = d / 2
    t = max(1.0, 0.022 * d)
    c = 0.10 * R  # the foot's rounded edge
    outer = (Profile(0, 0).line(R - c, 0).arc((R - 0.29 * c, 0.29 * c), (R, c)).line(R, h)
             .line(0, h).turn())
    inner = (Profile(0, t).line(R - t - c, t).arc((R - t - 0.29 * c, t + 0.29 * c), (R - t, t + c))
             .line(R - t, h + 5).line(0, h + 5).turn())
    glass = outer - inner
    # A bead round the rim, and the spout: the rim drawn down to a lip at the front right.
    glass = glass + Solid.make_torus(R - t / 2, 0.7 * t).moved(Location((0, 0, h - 0.3 * t)))
    notch = Solid.make_cylinder(0.16 * R, 4 * t, Plane.YZ).moved(Location((R - 2 * t, 0, h + 0.05 * R)))
    return glass - notch


def flask(d: float, h: float, neck: float) -> Solid:
    """An Erlenmeyer flask: a straight cone from a rounded foot to a short shoulder, then the
    neck."""
    R, rn = d / 2, neck / 2
    t = max(1.0, 0.018 * d)
    c = 0.10 * R
    shoulder = h - 0.22 * h
    s = 0.07 * h  # the shoulder's curve

    def side(off: float) -> Profile:
        # The cone's line from the foot's curve to the shoulder's.
        x0, z0 = R - off, c + off * 0.3
        x1, z1 = rn - off + 0.10 * (R - rn), shoulder - s
        p = Profile(0, off).line(R - c - off, off).arc((R - 0.29 * c - off, 0.29 * c + off), (x0, z0))
        p.line(x1, z1).spline(((x1 + rn - off) / 2 + 0.01 * R, shoulder - 0.35 * s), (rn - off, shoulder),
                              end_tangent=(0, 1))
        return p

    outer = side(0).line(rn, h).line(0, h).turn()
    inner = side(t).line(rn - t, h + 5).line(0, h + 5).turn()
    glass = outer - inner
    return glass + Solid.make_torus(rn - t / 2 + 0.3 * t, 0.9 * t).moved(Location((0, 0, h - 0.4 * t)))


def files():
    for i, (ml, d, h) in enumerate(BEAKERS):
        yield f"{CHAPTER}/beakers/{i + 1:03d} Beaker {ml} ml.step", part(beaker(d, h), f"beaker_{ml}ml")
    for i, (ml, d, h, neck) in enumerate(FLASKS):
        yield (f"{CHAPTER}/erlenmeyer_flasks/{i + 1:03d} Erlenmeyer {ml} ml.step",
               part(flask(d, h, neck), f"erlenmeyer_{ml}ml"))
