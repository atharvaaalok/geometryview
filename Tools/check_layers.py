#!/usr/bin/env python3
"""Fails if any first-party include points against the dependency direction.

Engine modules may include only modules at or below their own layer, so the dependency graph stays
acyclic. Import shares only `base` and `format` with the Engine.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Each module may depend on itself and on modules listed earlier.
ENGINE_LAYERS = [
    "base", "math", "format", "shaders", "scene", "model", "nav", "pick", "render", "ipc", "gpu",
]
IMPORT_ALLOWED = {"base", "format", "step", "stl", "mesh", "cache", "helper", "occt"}
INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s+"([a-z_0-9]+)/', re.MULTILINE)
SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".mm", ".metal", ".inl"}


def check_file(path: Path, module: str, allowed: set[str]) -> list[str]:
    text = path.read_text(encoding="utf-8")
    return [
        f"{path}: module '{module}' must not include '{target}/'"
        for target in INCLUDE.findall(text)
        if target not in allowed
    ]


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    errors: list[str] = []

    for index, module in enumerate(ENGINE_LAYERS):
        # `shared/` is the shaders module's own header directory.
        allowed = set(ENGINE_LAYERS[: index + 1]) | ({"shared"} if module == "shaders" else set())
        for directory in (root / "Engine" / module, root / "Import" / module):
            if not directory.is_dir():
                continue
            for path in directory.rglob("*"):
                if path.suffix in SOURCE_SUFFIXES:
                    errors.extend(check_file(path, module, allowed))

    for directory in (root / "Import").iterdir() if (root / "Import").is_dir() else ():
        if directory.is_dir() and directory.name != "format":
            for path in directory.rglob("*"):
                if path.suffix in SOURCE_SUFFIXES:
                    errors.extend(check_file(path, directory.name, IMPORT_ALLOWED))

    for error in errors:
        print(error, file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
