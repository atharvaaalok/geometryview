"""01 Organ Pipes: a rank of metal flue pipes, C4 to C6, and a rank of wooden stopped pipes.

A flue pipe's speaking length halves every octave, while its diameter halves only every
seventeenth note (the organ builder's normal scale), so the treble pipes are slimmer for their
length. A stopped pipe sounds an octave below an open one of its length.
"""

from __future__ import annotations

from build123d import Location, Solid

from common import part
from shapes import Profile

CHAPTER = "01_organ_pipes"
NOTES = ["C", "D", "E", "F", "G", "A", "B"]
STEPS = [0, 2, 4, 5, 7, 9, 11]


def diatonic(first_octave: int, count: int) -> list[tuple[str, int]]:
    """Note names and semitones above the first C, up the white keys."""
    out = []
    for i in range(count):
        octave, degree = divmod(i, 7)
        out.append((f"{NOTES[degree]}{first_octave + octave}", 12 * octave + STEPS[degree]))
    return out


def flue_pipe(n: int) -> Solid:
    """A metal flue pipe: a conical foot, a cylindrical body, a flattened mouth."""
    body = 150.0 * 2 ** (-n / 12)
    foot = 46.0 * 2 ** (-n / 24)
    R = 12.5 * 2 ** (-n / 17)
    wall = max(0.45, 0.05 * R)
    toe = 0.28 * R
    top = foot + body
    outer = (Profile(0, 0).line(toe, 0).line(toe, 0.06 * foot).line(R, foot).line(R, top)
             .line(0, top).turn())
    inner = (Profile(0, -1).line(toe - wall, -1).line(toe - wall, 0.06 * foot)
             .line(R - wall, foot).line(R - wall, top + 1).line(0, top + 1).turn())
    pipe = outer - inner
    # The languid: a plate across the pipe at the mouth, the flue a slit at its front.
    languid = Solid.make_cylinder(R - wall, 1.4 * wall).moved(Location((0, 0, foot - 0.7 * wall)))
    flue = Solid.make_box(2 * R, 0.16 * R, 4 * wall).moved(Location((-R, -R, foot - 2 * wall)))
    pipe = pipe + (languid - flue)
    # The mouth: the front pressed flat, a window cut in the flat above the languid.
    width = 1.1 * R
    flat = (R * R - (width / 2) ** 2) ** 0.5
    press = Solid.make_box(2 * R, R, 1.9 * R).moved(Location((-R, -flat - R, foot - 0.7 * R)))
    window = Solid.make_box(width * 0.86, 3 * R, 0.30 * width).moved(
        Location((-width * 0.43, -2 * R, foot + 0.5 * wall)))
    return pipe - press - window


def stopped_pipe(n: int) -> Solid:
    """A wooden stopped pipe: a square body on a block, a bevelled mouth, a stopper's handle."""
    length = 112.0 * 2 ** (-n / 12)
    a = 26.0 * 2 ** (-n / 17)
    t = 0.13 * a
    block = 0.36 * a
    top = block + length
    body = Solid.make_box(a, a, top).moved(Location((-a / 2, -a / 2, 0)))
    bore = Solid.make_box(a - 2 * t, a - 2 * t, length).moved(Location((-a / 2 + t, -a / 2 + t, block)))
    pipe = body - bore
    # The mouth: a window in the front over the block, its upper lip bevelled.
    mouth_h = 0.30 * a
    window = Solid.make_box(0.70 * a, 3 * t, mouth_h).moved(Location((-0.35 * a, -a / 2 - t, block)))
    bevel = Solid.make_box(0.70 * a, 2 * t, 1.2 * mouth_h).moved(
        Location((0, 0, 0), (1, 0, 0), -35)).moved(Location((-0.35 * a, -a / 2 - 0.4 * t, block + mouth_h)))
    pipe = pipe - window - bevel
    # The stopper, pushed in at the top, and its turned handle.
    plug = Solid.make_box(a - 2 * t, a - 2 * t, 0.5 * a).moved(Location((-a / 2 + t, -a / 2 + t, top - 0.25 * a)))
    handle = (Profile(0, 0).line(0.16 * a, 0).spline((0.10 * a, 0.18 * a), (0.12 * a, 0.30 * a))
              .ball(0.20 * a, 0.62 * a, 0.10 * a).turn()).moved(Location((0, 0, top + 0.25 * a)))
    return pipe + plug + handle


def files():
    for i, (note, n) in enumerate(diatonic(4, 15)):
        yield f"{CHAPTER}/flue_pipes/{i + 1:02d} Flue pipe {note}.step", part(flue_pipe(n), f"flue_pipe_{note.lower()}")
    for i, (note, n) in enumerate(diatonic(4, 8)):
        yield f"{CHAPTER}/stopped_pipes/{i + 1:02d} Stopped pipe {note}.step", part(stopped_pipe(n), f"stopped_pipe_{note.lower()}")
