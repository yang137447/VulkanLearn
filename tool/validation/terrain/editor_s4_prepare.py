"""Copy a terrain fixture's reachable author/payload/Cook closure, never hardlink."""

import argparse
from pathlib import Path
import shutil

from run_composited_acceptance import digest, inside, read_json, write_json

DEFAULT_SCENE = ("Generated/Validation/TerrainCompositedVT/Variants/"
                 "Near100m_R1024_Default/SC_composited_vt_100m.json")
PROJECT = Path(__file__).resolve().parents[3]


def closure(root, scene, project=PROJECT):
    root = root.resolve(strict=True)
    pending = [inside(root, scene)]
    files = set()
    optional_outputs = set()

    def reference(owner, value, optional=False):
        if not isinstance(value, str) or not value or value.startswith("shader/"):
            return
        if ":" in value or "\\" in value or value.startswith("/") or ".." in Path(value).parts:
            raise ValueError(f"Noncanonical resource reference in {owner}: {value}")
        candidates = [inside(root, value), inside(root, str(owner.parent.relative_to(root) / value))]
        existing = next((path for path in candidates if path.is_file()), None)
        if existing:
            pending.append(existing)
        elif optional:
            optional_outputs.add(value)
        else:
            raise ValueError(f"Missing fixture reference in {owner}: {value}")

    def visit(owner, value, key=""):
        if isinstance(value, dict):
            if "root" in value and "path" in value:
                if value["root"] == "resource":
                    reference(owner, value["path"], value.get("mode") == "absent")
                return
            for name, item in value.items():
                visit(owner, item, name)
        elif isinstance(value, list):
            for item in value:
                visit(owner, item, key)
        elif isinstance(value, str):
            suffixes = (".json", ".png", ".jpg", ".jpeg", ".exr", ".dds", ".ktx2",
                        ".hdr", ".bin", ".vlpaint", ".vlcvt", ".vltha", ".vlheight",
                        ".vlcoverage", ".vltcv", ".wav", ".obj", ".gltf", ".glb")
            if value.lower().endswith(suffixes) or key in ("package", "payload", "payloadFile", "binary"):
                reference(owner, value, key in ("packagePath", "paintPath"))

    graph = project / "config/renderGraphConfig.json"
    if graph.is_file():
        visit(root / "_runtime_graph.json", read_json(graph))
    while pending:
        path = pending.pop()
        if path in files:
            continue
        files.add(path)
        if path.suffix.lower() == ".json":
            document = read_json(path)
            visit(path, document)
            if document.get("kind") == "terrain-composited-vt":
                if document.get("version") == 2:
                    continue
                for surface in document.get("surfaces", []):
                    dependency = surface["dependencyHash"]
                    for suffix in (".dependencies.json", ".coverage.json", ".height.json"):
                        sidecar = path.parent / (dependency + suffix)
                        if not sidecar.is_file():
                            raise ValueError(f"Cook manifest is missing its sidecar: {sidecar}")
                        pending.append(sidecar)
            if document.get("type") == "terrain" and document.get("paintPath"):
                metadata = inside(root, document["paintPath"] + ".meta.json")
                if metadata.exists():
                    pending.append(metadata)
    plan = inside(root, str(Path(scene).parent / "capture_plan.json"))
    if plan.is_file():
        files.add(plan)
    return sorted(files), sorted(optional_outputs)


def prepare(root, scene, destination, near=True, project=PROJECT):
    root = root.resolve(strict=True)
    destination = destination.resolve()
    if destination.is_relative_to(root) or root.is_relative_to(destination):
        raise ValueError("Prepared closure and source resources must be disjoint")
    files, optional = closure(root, scene, project)
    for path in files:
        if len(str(destination / path.relative_to(root))) >= 240:
            raise ValueError("Prepared path exceeds safe Windows length; use --output D:/s4/<short-name>")
    destination.mkdir(parents=True, exist_ok=False)
    (destination / "Maps").mkdir()
    entries = {}
    for source in files:
        relative = source.relative_to(root)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        entries[relative.as_posix()] = {"bytes": target.stat().st_size, "sha256": digest(target)}
    scene_path = inside(destination, scene)
    document = read_json(scene_path)
    terrain_objects = [item for item in document["objects"] if item.get("type") == "terrain"]
    if not terrain_objects:
        raise ValueError("No terrain in fixture")
    material_paths = set()
    for item in terrain_objects:
        terrain = read_json(inside(destination, item["terrainPath"]))
        if not terrain.get("compositedVT", {}).get("enabled"):
            raise ValueError("S4 requires compositedVT on every terrain placement")
        for name in ("materialInstancePath", "detailMaterialInstancePath"):
            if terrain.get(name):
                material_paths.add(terrain[name])
    modifications = []
    for name in sorted(material_paths):
        path = inside(destination, name)
        material = read_json(path)
        for parameter in ("u_heightBlendDistanceFade", "u_terrainLayerMode", "u_layerForceMip"):
            material.setdefault("parameters", {}).pop(parameter, None)
        write_json(path, material)
        modifications.append({"path": name, "sha256": digest(path),
                              "reason": "composite-only schema; retired authoring parameters removed; initial Cook is mandatory"})
    plan_path = scene_path.parent / "capture_plan.json"
    if near:
        plan = read_json(plan_path)
        shot = next(item for item in plan["shots"] if item["name"] == "near")
        cameras = [item for item in document["objects"] if item.get("type") == "camera"]
        if len(cameras) != 1:
            raise ValueError("Near capture requires exactly one fixture camera")
        cameras[0]["position"], cameras[0]["look_at"] = shot["position"], shot["look_at"]
        document["runtimeMode"] = "authoring"
        write_json(scene_path, document)
        modifications.append({"path": scene, "sha256": digest(scene_path),
                              "reason": "capture_plan near pose; authoring mode"})
    report = {"kind": "editor-s4-fixture-closure", "source": str(root), "scene": scene,
              "files": entries, "bytes": sum(item["bytes"] for item in entries.values()),
              "optionalMissingOutputs": optional, "modifications": modifications,
              "requiredEmptyDirectories": ["Maps"],
              "rendererStarted": False, "copyMethod": "copy2; no hardlinks"}
    write_json(destination / "editor-s4-closure.json", report)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--scene", default=DEFAULT_SCENE)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = prepare(args.resource_root, args.scene, args.output)
    print(f"Copied {len(result['files'])} reachable files / {result['bytes']} bytes; renderer not started")
