#!/usr/bin/env python3
"""Runs the gallery's library over a folder with each number of imports at once (S) and workers each
(W), from an empty cache every time, and reports files a minute and the most memory the importing
processes held together. Chooses the gallery's defaults; see Docs/measurements/gallery/.

    Tools/gallery_sweep.py <folder> [--configs 1x3,2x3,4x3] [--limit N] [--out report.json]

Uses the release build's gcli, helper and shaders.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "release"
NAMES = ("gcli", "geometryview-import", "geometryview-occt-worker")


def footprint(pid: int) -> int:
    """Resident bytes of a process and everything under it."""
    listing = subprocess.run(["ps", "-axo", "pid=,ppid=,rss="], capture_output=True, text=True).stdout
    children: dict[int, list[int]] = {}
    rss: dict[int, int] = {}
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        p, parent, kb = (int(x) for x in parts)
        children.setdefault(parent, []).append(p)
        rss[p] = kb * 1024
    total, stack = 0, [pid]
    while stack:
        p = stack.pop()
        total += rss.get(p, 0)
        stack.extend(children.get(p, []))
    return total


def run(folder: Path, slots: int, workers: int, limit: int) -> dict:
    cache = Path(tempfile.mkdtemp(prefix="geosweep"))
    try:
        command = [str(BUILD / "Tools" / "gcli"), "library", str(BUILD / "shaders" / "default.metallib"),
                   str(BUILD / "Import" / "geometryview-import"), str(cache), str(folder),
                   "--slots", str(slots), "--workers", str(workers)]
        if limit:
            command += ["--limit", str(limit)]
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        peak = 0
        done = threading.Event()

        def watch() -> None:
            nonlocal peak
            while not done.is_set():
                peak = max(peak, footprint(process.pid))
                time.sleep(0.2)

        watcher = threading.Thread(target=watch)
        watcher.start()
        out, err = process.communicate()
        done.set()
        watcher.join()
        if process.returncode != 0:
            raise RuntimeError(err)
        report = json.loads(out.strip().splitlines()[-1])
        report["peak_mb"] = round(peak / (1 << 20), 1)
        report["blob_mb"] = round(sum(f.stat().st_size for f in cache.glob("*.geoblob")) / (1 << 20), 1)
        report["failed"] = [line for line in err.splitlines() if line.startswith("failed:")]
        return report
    finally:
        shutil.rmtree(cache, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("folder", type=Path)
    parser.add_argument("--configs", default="1x3,2x3,3x3,4x3,6x2,2x6,4x2,6x3")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    reports = []
    for config in args.configs.split(","):
        slots, workers = (int(x) for x in config.split("x"))
        report = run(args.folder, slots, workers, args.limit)
        reports.append(report)
        print(f"S={slots} W={workers}: {report['files']} files in {report['seconds']:.1f} s, "
              f"{report['files_per_minute']:.0f} a minute, peak {report['peak_mb']:.0f} MB, "
              f"{report['failures']} failed", flush=True)
    if args.out:
        args.out.write_text(json.dumps(reports, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
