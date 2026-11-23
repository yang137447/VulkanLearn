"""Stamp gen3 Cook expectations on fresh uncooked V2 authors; preserve all inputs."""

import argparse
import json
import os
from pathlib import Path
import tempfile

from build_composited_fixture import read_json, reference_path, require_topic_root, validate_author_world
from rvt_delivery import artifact
from rvt_generation_identity import COOK_IDENTITY


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    if options.output.exists():
        raise ValueError("Preserve previous stamp receipt")
    root = require_topic_root(Path(__file__).resolve().parents[3])
    world = validate_author_world(root, options.scene)
    directory = reference_path(root, options.scene).parent
    if not directory.name.startswith("RVTv2_Full4x4_") or not world["manifest"].startswith("VT/worldV2/"):
        raise ValueError("Only fresh generated Full16 V2 namespaces may be stamped")
    if (root / world["manifest"]).exists():
        raise ValueError("Published namespace needs an explicit new publication; no in-place identity stamp")
    inputs = {row["terrain"][field] for row in world["terrains"]
              for field in ("heightmapPath", "paintPath") if row["terrain"].get(field)}
    inputs.update(row["placement"]["terrainPath"] for row in world["terrains"])
    inputs.add(world["recipe"])
    before = {name: artifact(reference_path(root, name)) for name in sorted(inputs)}
    updates = []
    capture_path = directory / "capture_plan.json"
    capture = read_json(capture_path)
    capture["requiredCookIdentity"] = COOK_IDENTITY
    for path, document in ((capture_path, capture), (directory / "cook_identity.json", COOK_IDENTITY)):
        previous = artifact(path) if path.exists() else None
        descriptor, temporary = tempfile.mkstemp(prefix=".gen3-", dir=directory)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
                stream.write(json.dumps(document, ensure_ascii=False, indent=2) + "\n")
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
        updates.append({"before": previous, "after": artifact(path)})
    if before != {name: artifact(reference_path(root, name)) for name in before}:
        raise ValueError("Author/TR/recipe changed during identity stamp")
    report = {"status": "gen3-cook-required", "requiredCookIdentity": COOK_IDENTITY,
              "scene": options.scene, "packagePath": world["manifest"], "updates": updates,
              "preservedInputs": before, "rendererStarted": False, "cookStarted": False,
              "acceptancePassed": False}
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("status", "scene", "packagePath", "requiredCookIdentity")}, indent=2))


if __name__ == "__main__":
    main()
