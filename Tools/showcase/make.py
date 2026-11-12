"""Makes GeometryView's showcase models: a folder of things that come in sizes, for the gallery,
and assemblies to look inside. Every model is written here from scratch, MIT like the rest.

    PY=~/.pyenv/versions/3.12.10/bin/python3      # build123d 0.13 and OCP 8.0.1
    $PY Tools/showcase/make.py library [chapter ...] [--sheet]
    $PY Tools/showcase/make.py model <name ...> [--preview]
    $PY Tools/showcase/make.py manifest [--check]
    $PY Tools/showcase/make.py check [model ...]

The STEP files land in build/showcase (step/ for the assemblies, library/ for the folder), with
GeometryView cache files in blobs/ and preview sheets in preview/. They are not committed: the
scripts are, and manifest.json records what they make, so a change shows.

`check` intersects every pair of a model's parts and lists those that fill the same space: where two
parts share a surface the renderer cannot tell which is in front and draws stripes of both colours.
Parts of many bodies (blade rows, fins) are left out, since they are meant to sink into their hubs.

`library` writes the folder, "Things That Come in Sizes", one chapter after another; `--sheet`
draws each family on a contact sheet with `gcli sheet`. `model` writes an assembly; `--preview`
draws it from four sides and cut, in Illustration and Studio, on one sheet.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import platform
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import common  # noqa: E402
from common import BLOBS, FONT, GCLI, LIBRARY, PREVIEW, SHADERS, STEP  # noqa: E402

CHAPTERS = ["organ_pipes", "chessmen", "glassware", "bells", "weights", "dolls", "clockwork",
            "rockets"]
MODELS = ["lens", "turbofan", "orrery", "parade"]


def library(chapters: list[str], sheet: bool) -> None:
    for name in chapters:
        module = importlib.import_module(f"library.{name}")
        families: dict[str, list[tuple[Path, Path]]] = {}
        for relative, shape in module.files():
            step = common.write_step(shape, LIBRARY / relative)
            blob = common.import_blob(step, BLOBS / "library" / (relative + ".geoblob"))
            families.setdefault(str(Path(relative).parent), []).append((step, blob))
            print(f"  {relative}")
        if sheet:
            for family, files in families.items():
                draw_sheet(family, files)


def draw_sheet(family: str, files: list[tuple[Path, Path]]) -> None:
    PREVIEW.mkdir(parents=True, exist_ok=True)
    out = PREVIEW / (family.replace("/", "--") + ".png")
    tiles = "".join(json.dumps({"blob": str(blob), "label": step.stem[3:]}) + "\n"
                    for step, blob in files)
    subprocess.run([str(GCLI), "sheet", str(SHADERS), str(out), "--cell", "160",
                    "--columns", str(min(len(files), 8)), "--style", "illustration",
                    "--same-scale", "--font", str(FONT), "--title", family],
                   input=tiles, text=True, check=True, stdout=subprocess.DEVNULL)
    print(f"  sheet {out}")


def model(names: list[str], preview: bool) -> None:
    for name in names:
        module = importlib.import_module(f"models.{name}")
        step = STEP / f"{name}.step"
        if hasattr(module, "write"):
            module.write(step)  # a model written by hand, not through build123d
        else:
            common.write_step(module.build(), step)
        blob = common.import_blob(step, BLOBS / f"{name}.geoblob", getattr(module, "LEVEL", 1))
        print(f"  {step}")
        if preview:
            preview_model(name, blob, common.section(blob, getattr(module, "CUT", "x0.5")))


def preview_model(name: str, blob: Path, cut: str) -> None:
    """Four views and a cut, in Illustration and Studio, as one sheet."""
    from PIL import Image

    PREVIEW.mkdir(parents=True, exist_ok=True)
    frames = PREVIEW / "frames"
    frames.mkdir(exist_ok=True)
    views = [("iso", 35, 30, "none"), ("front", 0, 0, "none"), ("top", 0, 89.9, "none"),
             ("cut", 35, 30, cut)]
    shots = []
    for style in ("illustration", "studio"):
        for view, az, el, section in views:
            png = frames / f"{name}-{style}-{view}.png"
            shots.append(png)
            print(f'"{blob}" "{png}" --style {style} --az {az} --el {el} --section {section}',
                  file=open(frames / f"{name}.shots", "a" if len(shots) > 1 else "w"))
    subprocess.run([str(GCLI), "render", str(SHADERS), "--batch", "--size", "600x600",
                    "--supersample", "2"], stdin=open(frames / f"{name}.shots"), check=True)
    sheet = Image.new("RGB", (600 * 4, 600 * 2), "white")
    for i, png in enumerate(shots):
        sheet.paste(Image.open(png).convert("RGB"), ((i % 4) * 600, (i // 4) * 600))
    out = PREVIEW / f"{name}.png"
    sheet.save(out)
    print(f"  preview {out}")


def check(names: list[str]) -> int:
    """Lists pairs of parts that overlap, largest first. Returns the number of models with any."""
    import itertools

    def leaves(shape):
        kids = getattr(shape, "children", None)
        if kids:
            for k in kids:
                yield from leaves(k)
        else:
            yield shape

    failed = 0
    for name in names:
        module = importlib.import_module(f"models.{name}")
        if not hasattr(module, "build"):
            continue
        parts = [s for s in leaves(module.build()) if len(s.solids()) <= 6]
        boxes = [(s, s.bounding_box()) for s in parts]
        hits = []
        for (a, ba), (b, bb) in itertools.combinations(boxes, 2):
            if any(getattr(ba.max, k) < getattr(bb.min, k) or getattr(bb.max, k) < getattr(ba.min, k)
                   for k in "XYZ"):
                continue
            try:
                volume = (a & b).volume
            except Exception:
                continue
            if volume > 0.01:
                hits.append((volume, a.label, b.label))
        print(f"  {name}: {len(parts)} parts, {len(hits)} overlapping pairs")
        for volume, a, b in sorted(hits, reverse=True):
            print(f"    {volume:10.2f} mm3  {a} / {b}")
        failed += bool(hits)
    return failed


def manifest(check: bool) -> int:
    """Records each file's hash and the tools' versions; with --check, compares instead."""
    import build123d
    import OCP

    entries = {}
    for path in sorted(list(STEP.glob("*.step")) + list(LIBRARY.rglob("*.step"))):
        entries[str(path.relative_to(common.OUT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    record = {"build123d": build123d.__version__, "ocp": getattr(OCP, "__version__", "?"),
              "python": platform.python_version(), "files": entries}
    target = HERE / "manifest.json"
    if check:
        kept = json.loads(target.read_text())
        changed = [k for k in kept["files"] if kept["files"][k] != entries.get(k)]
        added = [k for k in entries if k not in kept["files"]]
        for k in changed:
            print(f"changed: {k}")
        for k in added:
            print(f"new: {k}")
        return 1 if changed or added else 0
    target.write_text(json.dumps(record, indent=1) + "\n")
    print(f"  {target}: {len(entries)} files")
    return 0


def main() -> int:
    common.environment()
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    lib = sub.add_parser("library")
    lib.add_argument("chapters", nargs="*", default=CHAPTERS)
    lib.add_argument("--sheet", action="store_true")
    mod = sub.add_parser("model")
    mod.add_argument("names", nargs="*", default=MODELS)
    mod.add_argument("--preview", action="store_true")
    man = sub.add_parser("manifest")
    man.add_argument("--check", action="store_true")
    chk = sub.add_parser("check")
    chk.add_argument("names", nargs="*", default=MODELS)
    args = parser.parse_args()
    if args.command == "library":
        library(args.chapters, args.sheet)
    elif args.command == "model":
        model(args.names, args.preview)
    elif args.command == "check":
        return 1 if check(args.names) else 0
    else:
        return manifest(args.check)
    return 0


if __name__ == "__main__":
    sys.exit(main())
