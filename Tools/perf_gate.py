#!/usr/bin/env python3
"""Fails when importing the fixtures got slower than the recorded baseline.

Each fixture is imported five times with `gcli import`, cold, into a fresh cache file, and the
fastest of the reported totals is compared with Tools/perf_baseline.json. The fastest run is the
one least disturbed by everything else the machine does: back to back, medians moved by up to 7%
for the multi-process STEP imports, which a 5% gate cannot tell from a regression. A time more
than 5% over its baseline fails, unless it is within 10 ms, below which timer noise dominates.
Run it on AC power with nothing else busy.

The fixtures are in the repository's test-models folder, which git ignores, or under
$GEOMETRYVIEW_FIXTURES, laid out the same way. A missing fixture is skipped.

Usage: perf_gate.py <build-dir> [--update] [--runs N]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "Tools" / "perf_baseline.json"
DEFAULT_FIXTURES = ROOT / "test-models"
TOLERANCE = 0.05
NOISE_SECONDS = 0.010

FIXTURES = {
    "demo_drive": "models/demo_drive.step",
    "pack_13S4P": "models/pack_13S4P.step",
    "ple120": "models/ple120_gearbox.step",
    "hard_52mb": "models/hard_52mb.step",
    "stl_10m": "large/stl10m.stl",
}


def import_seconds(build: Path, source: Path) -> float:
    gcli = build / "Tools" / "gcli"
    worker = build / "Import" / "geometryview-occt-worker"
    with tempfile.TemporaryDirectory() as scratch:
        blob = Path(scratch) / "fixture.geoblob"
        result = subprocess.run(
            [str(gcli), "import", str(source), str(blob), str(worker)],
            capture_output=True, text=True, check=True)
    match = re.search(r"total (\d+\.\d+) s", result.stdout + result.stderr)
    if not match:
        raise RuntimeError(f"no total in gcli's output for {source}")
    return float(match.group(1))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("build", type=Path)
    parser.add_argument("--update", action="store_true", help="record these times as the baseline")
    parser.add_argument("--runs", type=int, default=5)
    args = parser.parse_args()

    root = Path(os.environ.get("GEOMETRYVIEW_FIXTURES", DEFAULT_FIXTURES))
    baseline = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    fastest_times: dict[str, float] = {}
    failed = False
    for name, relative in FIXTURES.items():
        source = root / relative
        if not source.exists():
            print(f"{name:12} skipped: {source} not found")
            continue
        times = [import_seconds(args.build, source) for _ in range(args.runs)]
        fastest = min(times)
        fastest_times[name] = fastest
        before = baseline.get(name)
        if before is None:
            print(f"{name:12} {fastest * 1000:8.1f} ms  (no baseline)")
            continue
        limit = max(before * (1 + TOLERANCE), before + NOISE_SECONDS)
        verdict = "ok" if fastest <= limit else "SLOWER"
        failed = failed or fastest > limit
        print(f"{name:12} {fastest * 1000:8.1f} ms  baseline {before * 1000:8.1f} ms  {verdict}")

    if args.update:
        BASELINE.write_text(json.dumps(fastest_times, indent=2) + "\n")
        print(f"wrote {BASELINE}")
        return 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
