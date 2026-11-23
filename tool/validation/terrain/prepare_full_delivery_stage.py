"""Prepare isolated Full16 inputs; no renderer/Cook/export is ever launched."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil

from build_composited_fixture import (
    V2_SCENE, reference_path, require_topic_root, validate_author_world, published_world_files,
)

ROOT = Path(__file__).resolve().parents[3]
SCENE = V2_SCENE


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def author_sidecars(source, document):
    return [name + ".meta.json" for name in (
        document.get("heightmapPath"), document.get("paintPath"))
        if name and (source / (name + ".meta.json")).is_file()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", default=SCENE)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--private-resources", type=Path, required=True)
    parser.add_argument("--binary-directory", type=Path)
    parser.add_argument("--snapshot-lock", type=Path,
                        help="Euclid locked binary+GLSL+SPV inventory; mandatory for actual copying")
    parser.add_argument("--include-cook", action="store_true")
    parser.add_argument("--copy-inputs", action="store_true")
    parser.add_argument("--inventory", action="store_true",
                        help="Include exact source/private paths and rewritten TR documents in the dry report")
    options = parser.parse_args()
    snapshot_root = None
    snapshot_receipt = None
    if options.snapshot_lock:
        from rvt_snapshot_lock import validate_snapshot
        snapshot_root, snapshot_receipt = validate_snapshot(options.snapshot_lock)
        if options.binary_directory and options.binary_directory.resolve() != snapshot_root / "bin":
            raise ValueError("--binary-directory differs from locked snapshot")
        options.binary_directory = snapshot_root / "bin"
    if options.copy_inputs and snapshot_root is None:
        raise ValueError("--copy-inputs requires Euclid --snapshot-lock; shared live SPV/binaries cannot be frozen implicitly")
    source = require_topic_root(ROOT)
    world = validate_author_world(source, options.scene)
    stage = options.stage.resolve()
    private = options.private_resources.resolve()
    for target in (stage, private):
        if target.exists() or target.is_relative_to(source) or source.is_relative_to(target):
            raise ValueError(f"Output must be a fresh, disjoint directory: {target}")
        if target.is_relative_to(ROOT) or ROOT.is_relative_to(target):
            raise ValueError(f"Output cannot contain or be inside source project: {target}")
    if stage == private or stage.is_relative_to(private) or private.is_relative_to(stage):
        raise ValueError("Stage and private resources must be disjoint")
    if options.copy_inputs:
        destinations = snapshot_receipt.get("stageDestinations", {})
        selected = destinations.get(options.scene)
        if not selected or Path(selected["stage"]).resolve() != stage or (
                Path(selected["privateResources"]).resolve() != private):
            raise ValueError("Euclid snapshot lock must bind this exact new scene/stage/private destination")
    files = {}
    documents = {}
    visited = set()
    deferred = []
    cook_locations = []

    def add(name, destination=None, parse=True):
        path = reference_path(source, name)
        relative = Path(destination or name)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError(f"Unsafe destination: {relative}")
        existing = files.get(relative.as_posix())
        if existing and existing != path:
            raise ValueError(f"Conflicting destination: {relative}")
        files[relative.as_posix()] = path
        if path in visited:
            return
        visited.add(path)
        if not parse or path.suffix != ".json":
            return
        document = read(path)
        if document.get("type") == "terrain":
            for sidecar in author_sidecars(source, document):
                add(sidecar)
            manifest_name = document["compositedVT"]["packagePath"]
            manifest_relative = Path(manifest_name)
            short = (
                manifest_relative.parent if len(manifest_relative.parts) == 3 and manifest_relative.parts[0] == "VT"
                else Path("VT") / hashlib.sha256(manifest_name.encode()).hexdigest()[:16]
            )
            private_manifest = (short / Path(manifest_name).name).as_posix()
            if not cook_locations:
                cook_locations.append({"sourceLogicalManifest": manifest_name, "privateManifest": private_manifest})
                if options.include_cook:
                    manifest = reference_path(source, manifest_name)
                    data = read(manifest)
                    local_names = {manifest_name, (manifest_relative.parent / data["package"]).as_posix()}
                    local_names.update((manifest_relative.parent / entry["path"]).as_posix()
                                       for entry in data["production"]["objects"])
                    local_names.update((manifest_relative.parent / surface[field]).as_posix()
                                       for surface in data["production"]["surfaces"]
                                       for field in ("runtimeMaterialInstancePath", "runtimeDetailMaterialInstancePath")
                                       if surface.get(field))
                    for logical in published_world_files(source, manifest_name, world["surface_ids"]):
                        if logical in local_names:
                            relative_input = Path(logical).relative_to(manifest_relative.parent)
                            add(logical, short / relative_input, parse=False)
                        else:
                            add(logical)
                else:
                    deferred.append(manifest_name)
                add(world["recipe"])
            document["compositedVT"]["packagePath"] = private_manifest
            documents[relative.as_posix()] = document
            scan({key: value for key, value in document.items() if key != "compositedVT"})
        elif document.get("kind", "").startswith("terrain-"):
            return
        else:
            scan(document)

    def scan(value):
        if isinstance(value, dict):
            for child in value.values():
                scan(child)
        elif isinstance(value, list):
            for child in value:
                scan(child)
        elif isinstance(value, str):
            if value.startswith(("Common/", "Maps/", "Generated/")):
                add(value)
            elif value.startswith("shader/glsl/") and value.endswith(".json"):
                scan(read(ROOT / value))

    add(options.scene)
    add((Path(options.scene).parent / "capture_plan.json").as_posix())
    semantic_receipt = (Path(options.scene).parent / "paint_semantics.json").as_posix()
    if (source / semantic_receipt).is_file():
        add(semantic_receipt, parse=False)
    add("Common/Materials/MI_glass_crack.json")
    scan(read(ROOT / "config/renderGraphConfig.json"))
    scene = read(reference_path(source, options.scene))
    assert len([item for item in scene["objects"] if item["type"] == "terrain"]) == 16
    camera_plan = read(reference_path(source, (Path(options.scene).parent / "capture_plan.json").as_posix()))
    shots = camera_plan["shots"]
    by_name = {shot["name"]: shot for shot in shots}
    for name in ("far", "mid", "near", "tile00_near", "cross_tile_corner"):
        if name not in by_name:
            raise ValueError(f"Missing Full16 camera coverage: {name}")
    longest = max(len(str(private / name)) for name in files)
    if longest > 259:
        raise ValueError(f"Private closure exceeds Windows path limit: {longest}")
    report = {
        "scene": options.scene, "stage": str(stage), "privateResources": str(private),
        "files": len(files), "bytes": sum(path.stat().st_size for path in files.values()),
        "deferredCook": deferred, "cookPathFlattening": "VT/<16-id>/ original filenames and payload bytes",
        "cookOutputLocations": cook_locations,
        "longestCookMetadataPath": max(len(str(private / entry["privateManifest"])) -
            len("manifest.json") + 64 + len(".dependencies.json") for entry in cook_locations),
        "longestResourcePath": longest, "rendererStarted": False, "copied": False,
        "binaryDirectory": str(options.binary_directory) if options.binary_directory else None,
        "status": "prepared-only; installation validation and P0 admission pending",
        "publicationValidated": False,
        "runtimePlan": {
            "workerThreadCounts": [1, 2], "expectedSurfaces": 16,
            "budgetScope": "All caps must come from the accepted P0 global freeze; no per-TR budgets",
            "shots": shots,
            "continuousMovement": {
                "from": by_name["tile00_near"], "to": by_name["near"],
                "method": "bounded intermediate camera pose interpolation through the island, then settle",
            },
            "teleports": ["near", "tile00_near", "cross_tile_corner", "far"],
            "worldReloadCommand": "loadworld " + options.scene,
            "statisticsCommands": ["camera get", "terrainvt stats", "terrainchunk stats"],
            "convergenceGate": "All16 surfaces: queued/reading/cpuBytes/stale/readFailures zero; selected demand bounded, no steady IO loop",
            "required": [
                "CPU/GPU cache budget rejection before partial World commit",
                "Post-reload World generation advances, all16 surfaces and shared resources valid",
                "Normal LOD/TES screenshots include sea and at least three distinct terrain tiles plus shared corner",
                "Target PID CPU/WDDM sampling and paced frame percentiles with explicit GPU query coverage",
            ],
        },
    }
    from rvt_native_adapter import APPROVED_P0_SHA256, sha256
    frozen_path = ROOT / "config/terrainRvtP0Frozen.json"
    frozen_sidecar = ROOT / "config/terrainRvtP0Frozen.json.sha256"
    if sha256(frozen_path) != APPROVED_P0_SHA256 or frozen_sidecar.read_text().split()[0] != APPROVED_P0_SHA256:
        raise ValueError("Stage frozen config differs from independently approved bytes")
    report["frozenConfig"] = {"sha256": APPROVED_P0_SHA256, "files": [
        "config/terrainRvtP0Frozen.json", "config/terrainRvtP0Frozen.json.sha256"]}
    report["requiredBinaryInputs"] = ["main_game.exe", "main_editor.exe",
                                     "vl_export_game.exe", "vl_build_flavor.main_game.json"]
    report["pendingExecution"] = ["Freeze integrated binaries after CPU/runtime integration",
                                  "GPU owner grants Cook/render window", "Copy private inputs",
                                  "Cook World V2", "Warm game shaders", "Export and verify Game",
                                  "Execute two workers per weight mode"]
    report["paintSidecarInputs"] = [
        {"surfaceId": "terrain|" + row["placement"]["name"],
         "paintPath": row["terrain"].get("paintPath"),
         "sourceSidecarExists": (source / (row["terrain"]["paintPath"] + ".meta.json")).is_file(),
         "copySidecarPlanned": (row["terrain"]["paintPath"] + ".meta.json") in files}
        for row in world["terrains"] if row["terrain"].get("paintPath")]
    if options.binary_directory:
        binary_path = options.binary_directory.resolve(strict=True)
        report["binaryInventory"] = [
            {"path": str(binary_path / name), "exists": (binary_path / name).is_file(),
             "bytes": (binary_path / name).stat().st_size if (binary_path / name).is_file() else None}
            for name in report["requiredBinaryInputs"]]
        report["binaryIdentityFrozen"] = False
    if snapshot_root:
        report["snapshotLock"] = {"path": str(options.snapshot_lock.resolve()),
                                  "sha256": sha256(options.snapshot_lock), "owner": "Euclid",
                                  "root": str(snapshot_root),
                                  "scope": snapshot_receipt.get("scope"),
                                  "shaderPolicy": snapshot_receipt.get("shaderPolicy"),
                                  "warmShaderCacheEstablished": False,
                                  "formalAcceptancePassed": False}
        report["binaryIdentityFrozen"] = True
    if options.inventory:
        report["inputInventory"] = [
            {"source": str(path), "private": str(private / name), "bytes": path.stat().st_size,
             "rewrittenTerrain": documents.get(name)}
            for name, path in sorted(files.items())]
    if options.copy_inputs:
        binary = options.binary_directory.resolve(strict=True) if options.binary_directory else None
        if not binary:
            raise ValueError("--copy-inputs requires frozen --binary-directory")
        for name in report["requiredBinaryInputs"]:
            if not (binary / name).is_file():
                raise ValueError(f"Missing frozen binary input: {name}")
        source_hashes = {name: sha256(path) for name, path in files.items()}
        stage.mkdir(parents=True, exist_ok=False)
        private.mkdir(parents=True, exist_ok=False)
        for name, path in files.items():
            target = private / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if name in documents:
                target.write_text(json.dumps(documents[name], ensure_ascii=False, indent=2), encoding="utf-8")
            else:
                shutil.copyfile(path, target)
                if sha256(target) != source_hashes[name]:
                    raise ValueError(f"Private asset copy differs: {name}")
            if sha256(path) != source_hashes[name]:
                raise ValueError(f"Source asset changed during stage copy: {name}")
        report["sourceSha256"] = source_hashes
        (private / "Maps").mkdir(exist_ok=True)
        for name in ("config", "ui"):
            shutil.copytree(ROOT / name, stage / name)
        shutil.copytree(snapshot_root / "shader/glsl", stage / "shader/glsl")
        shutil.copytree(snapshot_root / "shader/spv", stage / "shader/spv")
        (stage / "bin").mkdir()
        names = ["main_game.exe", "vl_export_game.exe"]
        if (binary / "main_editor.exe").is_file():
            names.append("main_editor.exe")
        for path in list(binary.glob("*.dll")) + [binary / name for name in names]:
            shutil.copy2(path, stage / "bin" / path.name)
        shutil.copy2(binary / "vl_build_flavor.main_game.json", stage / "bin/vl_build_flavor.json")
        config = read(stage / "config/config.json")
        config.update({"resourcePath": private.as_posix(), "initScene": options.scene})
        config.setdefault("scripts", {})["enabled"] = False
        config.setdefault("ui", {})["enabled"] = False
        (stage / "config/config.json").write_text(json.dumps(config, indent=2), encoding="utf-8")
        report["copied"] = True
        from rvt_snapshot_lock import validate_snapshot
        validate_snapshot(options.snapshot_lock)
        for entry in snapshot_receipt["files"]:
            name = entry["path"]
            if name.startswith("shader/") or (name.startswith("bin/") and (
                    Path(name).suffix.lower() == ".dll" or Path(name).name in names)):
                copied_path = stage / name
                if not copied_path.is_file() or sha256(copied_path) != entry["sha256"]:
                    raise ValueError(f"Stage copy differs from locked snapshot: {name}")
        (stage / "full-delivery-stage.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
