#!/usr/bin/env python3
"""Compares where the Mac app and the web viewer put their chrome, from the Mac app's
--chrome-layout report and Tools/web_chrome.mjs's, both for the same window size and file.

  compare_chrome.py <mac.json> <web.json> [--tolerance 1]

Boxes are compared on their left edge, top, width and height; text, whose boxes each toolkit pads
its own way, on its left edge and vertical centre only. The masthead's left edge is left out: the
Mac's sits beside its window buttons. Exits 1 if anything differs by more than the tolerance.
"""

import argparse
import json
import sys

TEXT = {"masthead.title", "masthead.subtitle", "tree.name", "tree.count", "status.heading", "status.body"}
PLATFORM_X = {"masthead.title", "masthead.subtitle", "masthead.progress"}


def boxes(value):
    return value if isinstance(value, list) else [value]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mac")
    parser.add_argument("web")
    parser.add_argument("--tolerance", type=float, default=1.0)
    args = parser.parse_args()
    mac = json.load(open(args.mac))
    web = json.load(open(args.web))
    worst = 0.0
    # An empty list is nothing shown.
    mac = {k: v for k, v in mac.items() if v != []}
    web = {k: v for k, v in web.items() if v != []}
    for name in sorted(set(mac) | set(web)):
        if name not in mac or name not in web:
            print(f"{name:20s} only on the {'Mac' if name in mac else 'web'}")
            worst = max(worst, 99)
            continue
        a, b = boxes(mac[name]), boxes(web[name])
        if len(a) != len(b):
            print(f"{name:20s} {len(a)} on the Mac, {len(b)} on the web; comparing the first {min(len(a), len(b))}")
        for index, (m, w) in enumerate(zip(a, b)):
            if name in TEXT:
                keys = {"x": (m["x"], w["x"]), "cy": (m["y"] + m["h"] / 2, w["y"] + w["h"] / 2)}
            else:
                keys = {k: (m[k], w[k]) for k in ("x", "y", "w", "h")}
            if name in PLATFORM_X:
                keys.pop("x", None)
            off = {k: round(w_ - m_, 1) for k, (m_, w_) in keys.items() if abs(w_ - m_) > args.tolerance}
            worst = max([worst] + [abs(v) for v in off.values()])
            label = f"{name}[{index}]" if isinstance(mac[name], list) else name
            print(f"{label:20s} {'ok' if not off else 'web - mac: ' + ', '.join(f'{k} {v:+g}' for k, v in off.items())}")
    print(f"largest difference: {worst:g}")
    sys.exit(1 if worst > args.tolerance else 0)


if __name__ == "__main__":
    main()
