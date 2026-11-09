#!/usr/bin/env python3
"""Checks the browser's STEP importer on a sample of files, against OCCT and the native importer.

  web_step_sweep.py stage <sample.tsv>
      Links each file of the sample (lines of "<set>\\t<path>") into build/fixtures/sweep, for the
      sweep page (Web/spike/sweep.html) to fetch, and writes its list and index there.
  web_step_sweep.py check <build-dir> <label> [--jobs N] [--report out.json]
      Reads the sweep page's report and cache files (build/fixtures/out/<label>-p7-sweep*), imports
      each file natively as step_equivalence.py does, and compares the browser's cache file with
      OCCT reading the file whole (the same tolerances) and with the native import.

The browser runs the same importer compiled to WebAssembly, so it should match the native import
closely: the same faces and failed faces, triangles within 0.5% and area within 1e-4.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import step_equivalence  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SWEEP = ROOT / "build" / "fixtures" / "sweep"
OUT = ROOT / "build" / "fixtures" / "out"
TRIANGLE_TOLERANCE = 0.005
AREA_TOLERANCE = 1e-4


def stage(sample: Path) -> int:
    SWEEP.mkdir(parents=True, exist_ok=True)
    index, names = [], []
    for line, entry in enumerate(row for row in sample.read_text().splitlines() if row.strip()):
        kind, path = entry.split("\t", 1)
        source = Path(path)
        if not source.is_file():
            print(f"missing: {path}", file=sys.stderr)
            continue
        link = SWEEP / f"{len(names):05d}{source.suffix.lower()}"
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(source)
        names.append(f"sweep/{link.name}")
        index.append(f"{len(names) - 1}\t{kind}\t{path}")
    (SWEEP / "list.txt").write_text("\n".join(names) + "\n")
    (SWEEP / "index.tsv").write_text("\n".join(index) + "\n")
    print(f"{len(names)} files staged in {SWEEP}")
    return 0


def versus_native(browser: dict, native: dict) -> list[str]:
    problems = []
    if browser["faces"] != native["faces"]:
        problems.append(f"faces {browser['faces']} vs native {native['faces']}")
    if browser["failed"] != native["failed"]:
        problems.append(f"failed faces {browser['failed']} vs native {native['failed']}")
    if abs(browser["triangles"] - native["triangles"]) > TRIANGLE_TOLERANCE * max(native["triangles"], 1):
        problems.append(f"triangles {browser['triangles']} vs native {native['triangles']}")
    if not math.isclose(browser["area"], native["area"], rel_tol=AREA_TOLERANCE, abs_tol=1e-9):
        problems.append(f"area {browser['area']:.8g} vs native {native['area']:.8g}")
    return problems


def check_one(entry: dict, result: dict, gcli: Path, worker: Path, label: str) -> dict:
    path = entry["path"]
    native = step_equivalence.check(path, gcli, worker)
    out = {"line": entry["line"], "set": entry["set"], "path": path,
           "nativeSeconds": native.get("importSeconds"), "browserMs": result.get("ms"),
           "native": native.get("problems", []), "browser": [], "versusNative": []}
    if not result.get("ok"):
        out["browser"].append(f"browser import failed: {result.get('error')}")
        return out
    blob = OUT / f"{label}-p7-sweep-{entry['line']}.geoblob"
    stats = subprocess.run([gcli, "stats", blob], capture_output=True, text=True, check=False)
    if stats.returncode != 0:
        out["browser"].append(f"the browser's cache file does not read: {stats.stderr.strip()[:200]}")
        return out
    ours = json.loads(stats.stdout)
    ours["failed"] = int(result.get("failedFaces", 0))
    ours["hasVoids"] = b"BREP_WITH_VOIDS" in Path(path).read_bytes()
    out["browserStats"] = ours
    if "truth" in native:
        out["browser"] = step_equivalence.compare(ours, native["truth"])
    if "ours" in native:
        out["versusNative"] = versus_native(ours, native["ours"])
    return out


def check(build: Path, label: str, jobs: int, report: Path | None) -> int:
    gcli = build / "Tools" / "gcli"
    worker = build / "Import" / "geometry-occt-worker"
    sweep = json.loads((OUT / f"{label}-p7-sweep.json").read_text())
    if "error" in sweep:
        print(f"the sweep failed: {sweep['error']}")
        return 1
    index = {}
    for row in (SWEEP / "index.tsv").read_text().splitlines():
        line, kind, path = row.split("\t", 2)
        index[f"sweep/{int(line):05d}{Path(path).suffix.lower()}"] = {"line": int(line), "set": kind,
                                                                      "path": path}
    results = []
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        futures = {pool.submit(check_one, index[r["path"]], r, gcli, worker, label): r
                   for r in sweep["results"]}
        for future in concurrent.futures.as_completed(futures):
            try:
                results.append(future.result())
            except Exception as error:  # one file's harness failure, not the run's
                r = futures[future]
                results.append({"line": r["line"], "path": index[r["path"]]["path"],
                                "set": index[r["path"]]["set"], "native": [], "versusNative": [],
                                "browser": [f"harness: {error}"]})
    results.sort(key=lambda r: r["line"])

    def count(key: str) -> int:
        return sum(1 for r in results if r[key])

    new = [r for r in results if r["browser"] and not r["native"]]
    for r in new:
        print(f"BROWSER ONLY {r['path']}: {'; '.join(r['browser'])}")
    for r in results:
        if r["versusNative"]:
            print(f"VS NATIVE {r['path']}: {'; '.join(r['versusNative'])}")
    print(f"{len(results)} files: OCCT disagrees with the native import on {count('native')}, with "
          f"the browser's on {count('browser')} ({len(new)} of them the browser's alone); the browser "
          f"differs from the native import on {count('versusNative')}")
    timed = [(r["browserMs"] / 1000, r["nativeSeconds"]) for r in results
             if r.get("browserMs") and r.get("nativeSeconds")]
    if timed:
        print(f"time in all: browser {sum(b for b, _ in timed):.0f} s, native {sum(n for _, n in timed):.0f} s "
              f"(native with {jobs} files at once)")
    if report:
        report.write_text(json.dumps({"label": label, "userAgent": sweep.get("userAgent"),
                                      "workers": sweep.get("workers"), "seconds": sweep.get("seconds"),
                                      "results": results}, indent=1) + "\n")
    return 0 if not new and not count("versusNative") else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    staging = commands.add_parser("stage")
    staging.add_argument("sample", type=Path)
    checking = commands.add_parser("check")
    checking.add_argument("build", type=Path)
    checking.add_argument("label")
    checking.add_argument("--jobs", type=int, default=8)
    checking.add_argument("--report", type=Path)
    arguments = parser.parse_args()
    if arguments.command == "stage":
        return stage(arguments.sample)
    return check(arguments.build, arguments.label, arguments.jobs, arguments.report)


if __name__ == "__main__":
    sys.exit(main())
