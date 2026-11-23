"""Publish one validated World V2 closure, switching the manifest last."""

import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile

from build_composited_fixture import (
    published_world_files, read_json, reference_path, require_topic_root, validate_author_world,
)
from rvt_native_adapter import inspect_package, sha256

ROOT = Path(__file__).resolve().parents[3]


def without_output(document):
    document = json.loads(json.dumps(document))
    document.get("compositedVT", {}).pop("packagePath", None)
    return document


def collect_publication(source, target, scene):
    before = validate_author_world(source, scene)
    after = validate_author_world(target, scene)
    if read_json(reference_path(source, scene)) != read_json(reference_path(target, scene)):
        raise ValueError("Source/private Full16 scene changed")
    if before["author"] != after["author"]:
        raise ValueError("World author recipe changed")
    for private, author in zip(before["terrains"], after["terrains"]):
        if without_output(private["terrain"]) != without_output(author["terrain"]):
            raise ValueError("Source/private author terrain changed")
    source_manifest = Path(before["manifest"])
    target_manifest = Path(after["manifest"])
    document = read_json(reference_path(source, before["manifest"]))
    local_names = {before["manifest"], (source_manifest.parent / document["package"]).as_posix()}
    local_names.update((source_manifest.parent / entry["path"]).as_posix()
                       for entry in document["production"]["objects"])
    local_names.update((source_manifest.parent / surface[field]).as_posix()
                       for surface in document["production"]["surfaces"]
                       for field in ("runtimeMaterialInstancePath", "runtimeDetailMaterialInstancePath")
                       if surface.get(field))
    result = []
    for name, path in published_world_files(source, before["manifest"], before["surface_ids"]).items():
        relative = (target_manifest.parent / Path(name).relative_to(source_manifest.parent)
                    if name in local_names else Path(name))
        destination = target / relative
        if not destination.resolve().is_relative_to(target.resolve()):
            raise ValueError("Publication escaped resource root")
        digest = sha256(path)
        is_manifest = name == before["manifest"]
        if destination.exists() and not is_manifest and sha256(destination) != digest:
            raise ValueError(f"Immutable destination differs: {destination}")
        result.append((path, destination, {"path": relative.as_posix(),
                       "bytes": path.stat().st_size, "sha256": digest, "manifest": is_manifest}))
    return sorted(result, key=lambda entry: entry[2]["manifest"]), document


def publish_files(files):
    for source, destination, entry in files:
        if sha256(source) != entry["sha256"]:
            raise ValueError(f"Source changed after validation: {source}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() and sha256(destination) == entry["sha256"]:
            continue
        descriptor, temporary = tempfile.mkstemp(prefix=".rvt-publish-", dir=destination.parent)
        try:
            with os.fdopen(descriptor, "wb") as output, source.open("rb") as stream:
                shutil.copyfileobj(stream, output, 1024 * 1024)
                output.flush()
                os.fsync(output.fileno())
            if sha256(Path(temporary)) != entry["sha256"]:
                raise ValueError("Publication copy identity mismatch")
            if destination.exists() and not entry["manifest"]:
                raise ValueError(f"Concurrent immutable destination publication: {destination}")
            os.replace(temporary, destination)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--cook-exit", type=Path, required=True)
    parser.add_argument("--verified-package", type=Path, required=True)
    parser.add_argument("--weight-source", choices=("CoverageAtlas", "LocalLayerArray"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--publish", action="store_true")
    options = parser.parse_args()
    if options.cook_exit.read_text(encoding="utf-8-sig").strip() != "0":
        raise ValueError("Cook not successful")
    stage = options.stage.resolve(strict=True)
    config = read_json(stage / "config/config.json")
    private = (stage / config["resourcePath"]).resolve(strict=True)
    target = require_topic_root(ROOT)
    if private == target or private.is_relative_to(target) or target.is_relative_to(private):
        raise ValueError("Publication requires isolated private resources")
    summary = inspect_package(options.verified_package, config["initScene"], options.weight_source)
    files, document = collect_publication(private, target, config["initScene"])
    installed_config = read_json(options.verified_package / "config/config.json")
    installed_resources = options.verified_package / installed_config["resourcePath"]
    installed_scene = read_json(reference_path(installed_resources, config["initScene"]))
    placement = next(item for item in installed_scene["objects"] if item.get("type") == "terrain")
    installed_tr = read_json(reference_path(installed_resources, placement["terrainPath"]))
    installed = read_json(reference_path(installed_resources, installed_tr["compositedVT"]["packagePath"]))
    if installed != document:
        raise ValueError("Verified installed World differs from private Cook")
    if options.publish:
        publish_files(files)
    report = {"status": "published" if options.publish else "prepared-only", "published": options.publish,
              "scene": config["initScene"], "world": summary,
              "files": [entry for _, _, entry in files], "rendererStarted": False,
              "acceptancePassed": False, "recooked": False}
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
