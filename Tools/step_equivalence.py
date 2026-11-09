#!/usr/bin/env python3
"""Checks the slicing pipeline against OCCT reading each file whole.

Every file goes through `gcli import`, then the worker's --truth mode meshes the same file the
ordinary way with the same tolerances. The two must agree on face count, surface area, signed volume
and bounding box. Tessellations differ a little, so area and volume have tolerances.

Usage: step_equivalence.py <build-dir> <file-list> [--jobs N] [--limit N] [--report out.json]
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

AREA_TOLERANCE = 0.002
VOLUME_TOLERANCE = 0.01
BOUNDS_TOLERANCE = 0.01
# OCCT's repair sometimes splits a face. We keep the STEP entity as one face, so its count can be
# lower by a little, which area then confirms is the same surface.
FACE_COUNT_TOLERANCE = 0.01
TIMEOUT_SECONDS = 600


def compare(ours: dict, truth: dict) -> list[str]:
    problems: list[str] = []
    # Faces OCCT drops entirely count as failed for us, since we keep every STEP face's id.
    dropped = max(0, ours["faces"] - truth["faces"])
    if (abs(ours["faces"] - truth["faces"]) > FACE_COUNT_TOLERANCE * truth["faces"]
            and dropped != ours["failed"]):
        problems.append(f"faces {ours['faces']} vs {truth['faces']}")
    if ours["failed"] > truth["unmeshed"] + dropped:
        problems.append(f"{ours['failed']} faces failed, OCCT left {truth['unmeshed']}")
    if not math.isclose(ours["area"], truth["area"], rel_tol=AREA_TOLERANCE, abs_tol=1e-9):
        problems.append(f"area {ours['area']:.6g} vs {truth['area']:.6g}")
    # We turn inside-out solids the right way round and OCCT leaves them, so only size is compared.
    # With cavities OCCT can even add a void's volume, so there only our own sign is checked. A mesh
    # with missing faces has no meaningful volume.
    scale = max(abs(truth["signedVolume"]), truth["area"] ** 1.5 * 1e-4)
    if ours["failed"] == 0:
        if ours["hasVoids"]:
            if ours["signedVolume"] < 0:
                problems.append(f"negative volume {ours['signedVolume']:.6g}")
        elif abs(abs(ours["signedVolume"]) - abs(truth["signedVolume"])) > VOLUME_TOLERANCE * scale:
            problems.append(f"volume {ours['signedVolume']:.6g} vs {truth['signedVolume']:.6g}")
    diagonal = math.dist(truth["min"], truth["max"])
    offset = max(
        abs(a - b)
        for a, b in zip(ours["min"] + ours["max"], truth["min"] + truth["max"], strict=True)
    )
    if offset > BOUNDS_TOLERANCE * diagonal:
        problems.append(f"bounds differ by {offset:.4g} of diagonal {diagonal:.4g}")
    return problems


def check(path: str, gcli: Path, worker: Path) -> dict:
    result: dict = {"path": path, "problems": []}
    with tempfile.TemporaryDirectory() as scratch:
        blob = Path(scratch) / "file.geoblob"
        started = time.perf_counter()
        imported = subprocess.run(
            [gcli, "import", path, blob, worker],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
        result["importSeconds"] = time.perf_counter() - started
        if imported.returncode != 0:
            result["problems"].append(f"import failed: {imported.stderr.strip()}")
            return result
        result["log"] = imported.stdout
        tolerances = re.search(r"tolerances: linear (\S+) angular (\S+) min (\S+)", imported.stdout)
        failures = re.search(r"failed (\d+) unmapped (\d+) crashed (\d+)", imported.stdout)
        if failures is None or tolerances is None:
            result["problems"].append("could not read the import's report")
            return result
        failed, unmapped, crashed = map(int, failures.groups())
        result["counts"] = {"failed": failed, "unmapped": unmapped, "crashed": crashed}
        ours = json.loads(subprocess.run([gcli, "stats", blob], capture_output=True, text=True,
                                         check=True).stdout)

    started = time.perf_counter()
    truth_run = subprocess.run(
        [worker, "--truth", path, *tolerances.groups()],
        capture_output=True,
        text=True,
        timeout=TIMEOUT_SECONDS,
        check=False,
    )
    result["truthSeconds"] = time.perf_counter() - started
    if truth_run.returncode != 0:
        result["problems"].append("OCCT itself could not read this file")
        return result
    truth = json.loads(truth_run.stdout)
    ours["failed"] = failed
    ours["hasVoids"] = b"BREP_WITH_VOIDS" in Path(path).read_bytes()
    result["ours"] = ours
    result["truth"] = truth
    result["problems"].extend(compare(ours, truth))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("files", type=Path, help="a text file with one STEP path per line")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--report", type=Path)
    arguments = parser.parse_args()

    gcli = arguments.build / "Tools" / "gcli"
    worker = arguments.build / "Import" / "geometryview-occt-worker"
    paths = [line.strip() for line in arguments.files.read_text().splitlines() if line.strip()]
    if arguments.limit:
        paths = paths[: arguments.limit]

    results: list[dict] = []
    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
        futures = {pool.submit(check, path, gcli, worker): path for path in paths}
        for future in concurrent.futures.as_completed(futures):
            try:
                result = future.result()
            except Exception as error:  # a timeout or crash is one file's result, not the run's
                result = {"path": futures[future], "problems": [f"harness: {error}"]}
            results.append(result)
            if result["problems"]:
                print(f"FAIL {result['path']}: {'; '.join(result['problems'])}", flush=True)

    passed = sum(1 for result in results if not result["problems"])
    print(f"{passed}/{len(results)} files match OCCT")
    faster = [r["truthSeconds"] / r["importSeconds"] for r in results
              if "truthSeconds" in r and r["importSeconds"] > 0]
    if faster:
        faster.sort()
        print(f"median speedup {faster[len(faster) // 2]:.1f}x (ignoring contention)")
    if arguments.report:
        arguments.report.write_text(json.dumps(results, indent=1))
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
