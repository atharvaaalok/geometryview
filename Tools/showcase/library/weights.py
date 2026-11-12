"""05 Weights: cast-iron bell weights, 500 g to 20 kg, and knob weights, 10 g to 500 g.

Weights of one shape grow as the cube root of their mass: a 20 kg bell weight is 40 times the
mass of a 500 g one and about 3.4 times as tall.
"""

from __future__ import annotations

from build123d import Location, Plane, Solid

from common import part
from shapes import Profile

CHAPTER = "05_weights"

BELL_WEIGHTS = [(0.5, "500 g"), (1, "1 kg"), (2, "2 kg"), (5, "5 kg"), (10, "10 kg"), (20, "20 kg")]
KNOB_WEIGHTS = [(10, "10 g"), (20, "20 g"), (50, "50 g"), (100, "100 g"), (200, "200 g"), (500, "500 g")]


def bell_weight(kg: float) -> Solid:
    """A domed body with a ring cast on top."""
    H = 190.0 * (kg / 20) ** (1 / 3)
    body_h = 0.62 * H
    R = 0.40 * H
    body = (Profile(0, 0).line(R, 0).line(R, 0.08 * body_h)
            .spline((0.95 * R, 0.40 * body_h), (0.72 * R, 0.80 * body_h), (0.36 * R, body_h))
            .line(0, body_h).turn())
    # The ring stands upright across the X axis, gripped above the dome.
    ring_r = 0.20 * H
    tube = 0.055 * H
    ring = Solid.make_torus(ring_r, tube, Plane.XZ).moved(Location((0, 0, body_h + ring_r - 0.4 * tube)))
    boss = Solid.make_cylinder(0.14 * H, 0.08 * H).moved(Location((0, 0, body_h - 0.03 * H)))
    return body + boss + ring


def knob_weight(g: float) -> Solid:
    """The usual laboratory knob weight: a cylinder, a neck and a knob."""
    H = 67.0 * (g / 500) ** (1 / 3)
    R = 0.29 * H
    body_h = 0.62 * H
    return (Profile(0, 0).line(R, 0).line(R, body_h - 0.05 * H)
            .spline((0.92 * R, body_h), (0.55 * R, body_h + 0.03 * H))
            .spline((0.36 * R, body_h + 0.10 * H), (0.40 * R, body_h + 0.16 * H))
            .ball(0.58 * R, H, 0.38 * R).turn())


def files():
    for i, (kg, label) in enumerate(BELL_WEIGHTS):
        yield (f"{CHAPTER}/bell_weights/{i + 1:02d} Bell weight {label}.step",
               part(bell_weight(kg), "bell_weight_" + label.replace(" ", "")))
    for i, (g, label) in enumerate(KNOB_WEIGHTS):
        yield (f"{CHAPTER}/knob_weights/{i + 1:02d} Knob weight {label}.step",
               part(knob_weight(g), "knob_weight_" + label.replace(" ", "")))
