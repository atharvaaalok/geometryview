"""What every showcase model shares: where files go, how they are written, and the name check.

Models are written as STEP with a fixed timestamp, so the same script makes the same file. Names
matter in GeometryView: a part's name picks its material colour and whether it fades in
see-through, so `check_names` refuses the words that would mislead it.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from pathlib import Path

from build123d import Compound, Shape, export_step

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "showcase"
STEP = OUT / "step"
# The folder's own name is its title: posters print it as it is.
LIBRARY = OUT / "library" / "Things That Come in Sizes"
BLOBS = OUT / "blobs"
PREVIEW = OUT / "preview"

BUILD = ROOT / "build" / "release"
GCLI = BUILD / "Tools" / "gcli"
WORKER = BUILD / "Import" / "geometryview-occt-worker"
SHADERS = BUILD / "shaders" / "default.metallib"
FONT = ROOT / "ThirdParty" / "ibm-plex-mono" / "IBMPlexMono-Regular.ttf"

TIMESTAMP = "2026-11-11T00:00:00"

# Words a name must not hold, with what GeometryView would make of them: material colours come
# from words in names (Engine/render/style.cpp materialFromName), and so do shells that fade in
# see-through (Engine/core/shell.cpp).
BANNED = {
    "lead": "rubber",
    "cable": "rubber",
    "board": "circuit board",
    "hall": "circuit board",
    "paper": "insulation",
    "liner": "insulation",
    "showcase": "a shell (case)",
    "chessboard": "a shell (case) and a circuit board",
}


def check_names(names: list[str]) -> list[str]:
    """Problems with these part names: banned words, a `_core` suffix, or nothing to read."""
    problems = []
    for name in names:
        words = re.split(r"[_\s\-.]+", name.lower())
        for word in words:
            if word in BANNED:
                problems.append(f"{name}: '{word}' reads as {BANNED[word]}")
        if name.lower().endswith("_core"):
            problems.append(f"{name}: a _core suffix reads as lamination")
        if not name or name.upper() in {"SOLID", "BODY", "PART", "NONE", "UNNAMED"}:
            problems.append(f"{name!r}: a placeholder name")
    return problems


def names_in(shape: Shape) -> list[str]:
    """Every label in a shape's tree."""
    out = [shape.label] if shape.label else []
    for child in getattr(shape, "children", ()) or ():
        out += names_in(child)
    return out


def write_step(shape: Shape, path: Path) -> Path:
    """Writes a shape as STEP, after checking its names. Returns the path."""
    problems = check_names(names_in(shape))
    if problems:
        raise ValueError("bad names:\n  " + "\n  ".join(problems))
    path.parent.mkdir(parents=True, exist_ok=True)
    if not export_step(shape, str(path), timestamp=TIMESTAMP):
        raise RuntimeError(f"could not write {path}")
    return path


def part(shape: Shape, name: str) -> Shape:
    """A shape named, as one part."""
    shape.label = name
    return shape


def bodies(name: str, shapes: list[Shape]) -> Compound:
    """Repeated pieces as one part of many bodies, such as a rocket's fins: GeometryView numbers
    colours by part, so pieces placed as separate parts would each take the next colour."""
    shape = Compound(shapes)
    shape.label = name
    return shape


def assembly(name: str, children: list[Shape]) -> Compound:
    """An assembly of named parts and sub-assemblies."""
    return Compound(children=children, label=name)


def import_blob(step: Path, blob: Path, level: int = 1) -> Path:
    """Imports a STEP file into GeometryView's cache format, finely tessellated for close-ups."""
    blob.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(GCLI), "import", str(step), str(blob), str(WORKER), str(level)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return blob


def section(blob: Path, cut) -> str:
    """A section for gcli render: as given ("x0.5", a fraction of the bounds), or ("x", 0.0), a
    plane at that coordinate, turned into the fraction of this file's bounds."""
    if isinstance(cut, str):
        return cut
    axis, at = cut
    facts = json.loads(subprocess.run([str(GCLI), "facts", str(blob)], check=True, capture_output=True,
                                      text=True).stdout)
    i = "xyz".index(axis)
    lo, hi = facts["min"][i], facts["max"][i]
    return f"{axis}{(at - lo) / (hi - lo):.4f}"


def guard(path: Path) -> Path:
    """Refuses any path outside build/showcase, before anything is deleted there."""
    resolved = path.resolve()
    if OUT.resolve() not in resolved.parents and resolved != OUT.resolve():
        raise ValueError(f"{path} is outside {OUT}")
    if "designlib" in str(resolved):
        raise ValueError(f"{path} names designlib")
    return resolved


def environment() -> None:
    """Hash seeds fixed, so the same script writes the same file."""
    os.environ.setdefault("PYTHONHASHSEED", "0")
