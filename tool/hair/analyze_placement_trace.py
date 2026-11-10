#!/usr/bin/env python3
"""Compare two VLHT v2 hair traces for placement inertia.

The sweep trace and the control trace must use the same scene, animation state,
guide selection and frame range. Roots should stay identical (the hair is pinned
to the source skeleton); tip differences measure the placement-induced local
shape/lag. This script is read-only and does not replace the physical acceptance
criteria; it reports numbers for the record.

Usage:
    py -3 tool/hair/analyze_placement_trace.py <sweep.vlht> <control.vlht>
"""

import argparse
import json
import math
import struct
import sys
from pathlib import Path

HEADER = struct.Struct("<4sIIIIIQ")
FRAME = struct.Struct("<QQIfII16f2f")
MAGIC = b"VLHT"
VERSION = 3
SAMPLE_FLOATS = 12  # current/previous/prevPrevious


class TraceError(ValueError):
    pass


def read_trace(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise TraceError("short trace header")
    magic, version, total, ppg, stride, selected, start = HEADER.unpack_from(data, 0)
    if magic != MAGIC or version != VERSION:
        raise TraceError(f"expected VLHT v{VERSION}, got {magic!r} v{version}")
    offset = HEADER.size + 4 * selected + 4 * selected * ppg
    frames = []
    while offset < len(data):
        if len(data) - offset < FRAME.size:
            raise TraceError("truncated frame header")
        values = FRAME.unpack_from(data, offset)
        offset += FRAME.size
        frame_index, reset_reason = values[0], values[4]
        model = values[6:]
        payload = selected * ppg * SAMPLE_FLOATS * 4
        if len(data) - offset < payload:
            raise TraceError("truncated frame payload")
        roots, tips = [], []
        for guide in range(selected):
            base = offset + guide * ppg * SAMPLE_FLOATS * 4
            roots.append(struct.unpack_from("<3f", data, base))
            tips.append(struct.unpack_from(
                "<3f", data, base + (ppg - 1) * SAMPLE_FLOATS * 4))
        offset += payload
        frames.append({
            "frame": frame_index,
            "reset": reset_reason,
            "translation": model[12:15],
            "roots": roots,
            "tips": tips,
        })
    if not frames:
        raise TraceError("trace has no frames")
    return {"selected": selected, "points": ppg, "start": start, "frames": frames}


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(q * (len(ordered) - 1)))))
    return ordered[index]


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("sweep", type=Path)
    parser.add_argument("control", type=Path)
    parser.add_argument("--json", type=Path, default=None)
    args = parser.parse_args(argv)

    sweep = read_trace(args.sweep)
    control = read_trace(args.control)
    if sweep["selected"] != control["selected"] or sweep["points"] != control["points"]:
        raise TraceError("sweep/control guide selection or pointsPerGuide differ")
    count = min(len(sweep["frames"]), len(control["frames"]))
    if count == 0:
        raise TraceError("no common frames")

    tip_diff = []
    root_diff = []
    for frame in range(count):
        a = sweep["frames"][frame]
        b = control["frames"][frame]
        for guide in range(sweep["selected"]):
            tip_diff.append(math.dist(a["tips"][guide], b["tips"][guide]))
            root_diff.append(math.dist(a["roots"][guide], b["roots"][guide]))

    tail = min(30, count)
    tail_tip = []
    for frame in range(count - tail, count):
        a = sweep["frames"][frame]
        b = control["frames"][frame]
        for guide in range(sweep["selected"]):
            tail_tip.append(math.dist(a["tips"][guide], b["tips"][guide]))

    translations = [frame["translation"] for frame in sweep["frames"]]
    travel = math.dist(translations[-1], translations[0])
    report = {
        "sweep": str(args.sweep),
        "control": str(args.control),
        "frames": count,
        "selectedGuides": sweep["selected"],
        "pointsPerGuide": sweep["points"],
        "modelTravelCm": travel,
        "resetFrames": [frame for frame in range(count)
                        if sweep["frames"][frame]["reset"] != 0],
        "tipDifferenceMaxCm": max(tip_diff),
        "tipDifferenceMeanCm": sum(tip_diff) / len(tip_diff),
        "tipDifferenceP99Cm": percentile(tip_diff, 0.99),
        "tipDifferenceTailMeanCm": sum(tail_tip) / len(tail_tip),
        "rootDifferenceMaxCm": max(root_diff),
    }
    text = json.dumps(report, indent=2)
    print(text)
    if args.json is not None:
        args.json.write_text(text + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except TraceError as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(2)
