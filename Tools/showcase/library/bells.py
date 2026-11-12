"""04 Bells: two octaves of handbells, C5 to C7, and a set of singing bowls.

A handbell is an assembly: the bell, its turned handle, and the clapper inside on its shaft, so a
cut or see-through shows how it rings. Bells of one shape sound lower as they grow; across two
octaves this set grows about 2.4 times.
"""

from __future__ import annotations

from build123d import Location, Solid

from common import assembly, part
from library.organ_pipes import diatonic
from shapes import Profile, at

CHAPTER = "04_bells"


def bell(Rm: float, Hb: float) -> Solid:
    """A bell standing on its mouth: thick at the sound bow, thin at the waist."""
    outer = (Profile(0, 0).line(Rm, 0)
             .spline((0.97 * Rm, 0.07 * Hb), (0.80 * Rm, 0.30 * Hb), (0.66 * Rm, 0.60 * Hb),
                     (0.62 * Rm, 0.84 * Hb), (0.46 * Rm, 0.98 * Hb), (0.30 * Rm, Hb))
             .line(0, Hb).turn())
    inner = (Profile(0, -1).line(0.90 * Rm, -1).line(0.90 * Rm, 0)
             .spline((0.86 * Rm, 0.07 * Hb), (0.74 * Rm, 0.30 * Hb), (0.62 * Rm, 0.60 * Hb),
                     (0.57 * Rm, 0.82 * Hb), (0.42 * Rm, 0.93 * Hb), (0.26 * Rm, 0.95 * Hb))
             .line(0, 0.95 * Hb).turn())
    return outer - inner


def handle(Rm: float, length: float) -> Solid:
    r = 0.16 * Rm
    return (Profile(0, 0).line(0.36 * Rm, 0).line(0.36 * Rm, 0.06 * length)
            .spline((0.20 * Rm, 0.16 * length), (0.15 * Rm, 0.30 * length))
            .spline((r, 0.60 * length), (1.15 * r, 0.80 * length))
            .ball(1.25 * r, length, 0.8 * r).turn())


def handbell(n: int) -> list:
    scale = 2 ** (-n / 19)  # 2.4 times across two octaves
    Rm = 48.0 * scale
    Hb = 1.0 * Rm * 1.35
    grip = 1.45 * Rm
    the_bell = part(bell(Rm, Hb), "bell")
    the_handle = part(at(handle(Rm, grip), z=Hb), "handle")
    shaft = part(Solid.make_cylinder(0.035 * Rm, 0.62 * Hb).moved(Location((0, 0, 0.30 * Hb))), "clapper_shaft")
    ball = part(Solid.make_sphere(0.15 * Rm).moved(Location((0, 0, 0.30 * Hb))), "clapper_ball")
    return [the_bell, the_handle, shaft, ball]


# Singing bowls, by rim diameter (mm).
BOWLS = [70, 82, 95, 110, 125, 140, 155, 170]


def bowl(d: float) -> Solid:
    """A singing bowl: a wide round body, its rim turned in, on a small flat foot."""
    R = d / 2
    h = 0.86 * R
    t = 0.04 * d
    outer = (Profile(0, 0).line(0.36 * R, 0)
             .spline((0.74 * R, 0.10 * h), (0.97 * R, 0.42 * h), (R, 0.70 * h), (0.93 * R, h))
             .line(0, h).turn())
    inner = (Profile(0, t).line(0.32 * R, t)
             .spline((0.70 * R, 0.10 * h + 0.7 * t), (0.92 * R, 0.43 * h), (0.95 * R - 0.2 * t, 0.71 * h),
                     (0.93 * R - t, h), (0.90 * R - t, h + 2))
             .line(0, h + 2).turn())
    return outer - inner


def files():
    for i, (note, n) in enumerate(diatonic(5, 15)):
        yield (f"{CHAPTER}/handbells/{i + 1:03d} Handbell {note}.step",
               assembly(f"handbell_{note.lower()}", handbell(n)))
    for i, d in enumerate(BOWLS):
        yield f"{CHAPTER}/singing_bowls/{i + 1:03d} Singing bowl {d} mm.step", part(bowl(d), f"singing_bowl_{d}mm")
