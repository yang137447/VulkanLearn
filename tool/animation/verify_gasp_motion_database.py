"""Verify real GASP database rebuilding and cache repair in an isolated resource copy."""

import argparse
import json
import shutil
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tool", default="build/bin/vl_animation_probe.exe", type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("Choose a new verification directory")
    output.mkdir(parents=True)
    tool = args.tool.resolve()
    source = "Maps/SC_gasp/MotionMatching/DB_gasp.json"

    def build(root, label, expected=0):
        report = output / f"{label}.json"
        result = subprocess.run(
            [str(tool), "--build-motion-db", source, "--resource-root", str(root),
             "--output", str(report)], capture_output=True, text=True, encoding="utf-8")
        (output / f"{label}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        if result.returncode != expected:
            raise RuntimeError(f"{label}: unexpected exit {result.returncode}")
        return json.loads(report.read_text(encoding="utf-8")) if expected == 0 else None

    baseline = build(args.resources.resolve(), "baseline")
    isolated = output / "Resources"
    for item in baseline["inputs"]:
        target = isolated / item["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.resources / item["path"], target)
    cold = build(isolated, "cold")
    assert cold["identity"] == baseline["identity"]
    assert cold["payloadBlake3"] == baseline["payloadBlake3"]
    hit = build(isolated, "hit")
    assert hit["cacheStatus"] == "verified-hit"
    cache_path = isolated / cold["cache"]
    cache = json.loads(cache_path.read_text(encoding="utf-8"))
    cache["database"]["frames"][0]["features"][0] += 1
    cache_path.write_text(json.dumps(cache), encoding="utf-8")
    repaired = build(isolated, "repaired")
    assert repaired["cacheStatus"] == "invalid-cache-rebuilt"
    assert repaired["payloadBlake3"] == cold["payloadBlake3"]
    original = json.loads((isolated / source).read_text(encoding="utf-8"))
    invalid = dict(original, version=2)
    (isolated / source).write_text(json.dumps(invalid), encoding="utf-8")
    build(isolated, "unsupported-version", 1)
    (isolated / source).write_text(json.dumps(original), encoding="utf-8")
    schema_path = isolated / original["schema"]
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    schema["weights"]["posePosition"] = 2
    schema_path.write_text(json.dumps(schema), encoding="utf-8")
    weighted = build(isolated, "schema-changed")
    assert weighted["identity"] != cold["identity"]
    original["members"].reverse()
    (isolated / source).write_text(json.dumps(original), encoding="utf-8")
    reordered = build(isolated, "members-reordered")
    assert reordered["identity"] != weighted["identity"]
    assert reordered["clips"] == list(reversed(weighted["clips"]))
    report = {"passed": True, "frames": cold["frames"], "dimensions": cold["dimensions"],
              "identity": cold["identity"], "payloadBlake3": cold["payloadBlake3"],
              "checks": ["cold rebuild equals baseline", "verified hit", "corrupt payload repaired",
                         "source version rejected", "schema invalidation", "member order invalidation"]}
    (output / "verification.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
