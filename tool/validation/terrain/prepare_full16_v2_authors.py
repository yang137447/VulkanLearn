"""Read-only Full16 source preflight, then optional fresh V2 author generation."""

import argparse
import hashlib
import json
from pathlib import Path

from build_composited_fixture import (
    FIXTURE, WEIGHT_SOURCES, build, read_json, reference_path, require_topic_root,
)
from check_composited_fixture_camera import png16
from rvt_delivery import artifact, full16_camera_plan

ROOT = Path(__file__).resolve().parents[3]


def source_preflight(resources, scene_name):
    scene_path = reference_path(resources, scene_name)
    scene = read_json(scene_path)
    placements = [row for row in scene["objects"] if row.get("type") == "terrain"]
    expected = [f"Landscape_{index:02d}" for index in range(16)]
    if sorted(row["name"] for row in placements) != expected:
        raise ValueError("Full16 source placement identities differ")
    files, frames, copies = {}, [], {}

    def add(name):
        path = reference_path(resources, name)
        if name in files:
            return
        files[name] = artifact(path)
        if path.suffix.lower() != ".json":
            return
        document = read_json(path)
        if document.get("type") == "terrain":
            for field in ("heightmapPath", "paintPath", "materialInstancePath", "detailMaterialInstancePath"):
                if document.get(field):
                    add(document[field])
            return
        if document.get("type") == "materialInstance":
            for value in document.get("textures", {}).values():
                add(value)
            return
        if document.get("type") in ("texture", "textureArray"):
            for field in ("source", "sources", "displacementSources"):
                values = document.get(field, [])
                for value in ([values] if isinstance(values, str) else values):
                    add(value)

    add(scene_name)
    for placement in placements:
        terrain_name = placement["terrainPath"]
        add(terrain_name)
        terrain = read_json(reference_path(resources, terrain_name))
        pixels = png16(reference_path(resources, terrain["heightmapPath"]))
        size = terrain["gridResolution"] + 1 + terrain.get("heightmapMargin", 0) * 2
        if len(pixels) != size or any(len(row) != size for row in pixels):
            raise ValueError(f"Height/frame input size differs: {terrain_name}")
        extent = terrain["gridResolution"] + 1
        frames.append({"surfaceId": "terrain|" + placement["name"], "width": extent, "height": extent,
                       "channels": ["localNormal", "localTangent"], "format": "rgba32-float",
                       "payloadBytes": extent * extent * 16 * 2,
                       "sourceHeightSha256": files[terrain["heightmapPath"]]["sha256"],
                       "scope": "Deterministic frame reconstruction inputs verified; frame pixels require engine Cook"})
        for field in ("heightmapPath", "paintPath"):
            if terrain.get(field):
                copies[terrain[field]] = files[terrain[field]]
                sidecar = terrain[field] + ".meta.json"
                if (resources / sidecar).is_file():
                    add(sidecar)
                    copies[sidecar] = files[sidecar]
        for field in ("materialInstancePath", "detailMaterialInstancePath"):
            material = read_json(reference_path(resources, terrain[field]))
            albedo = read_json(reference_path(resources, material["textures"]["layerAlbedo"]))
            if albedo["colorSpace"] != "srgb":
                raise ValueError("Source albedo must remain sRGB")
    cameras = full16_camera_plan(resources, scene_name, scene_path.parent / "capture_plan.json")
    if cameras["counts"] != {"center": 16, "edge": 24, "junction": 9, "outer": 4}:
        raise ValueError("Full16 camera coverage differs")
    proposed = {}
    for mode in WEIGHT_SOURCES:
        namespace = f"RVTv2_Full4x4_R8192_Default_{mode}"
        directory = FIXTURE / "Variants" / namespace
        manifest = Path("VT/worldV2") / namespace / "manifest.json"
        if (resources / directory).exists() or (resources / manifest.parent).exists():
            raise ValueError(f"Fresh V2 namespace already exists: {namespace}")
        paths = [(directory / "SC_composited_vt_4x4.json").as_posix(),
                 (directory / "world.author.json").as_posix(), manifest.as_posix()]
        paths.extend((directory / f"TR_composited_tile_{index:02d}.json").as_posix() for index in range(16))
        for index, placement in enumerate(sorted(placements, key=lambda row: row["name"])):
            terrain = read_json(reference_path(resources, placement["terrainPath"]))
            for field in ("heightmapPath", "paintPath"):
                if terrain.get(field):
                    paths.append((directory / f"Author/tile_{index:02d}" / Path(terrain[field]).name).as_posix())
        longest = max(len(str(resources / path)) for path in paths)
        if longest > 259:
            raise ValueError(f"Author path exceeds Windows bound: {longest}")
        proposed[mode] = {"namespace": namespace, "scene": paths[0], "recipe": paths[1],
                          "manifest": paths[2], "terrains": paths[3:19], "longestAuthorPath": longest}
    frozen = read_json(ROOT / "config/terrainRvtP0Frozen.json")
    return {"status": "cpu-preflight-passed", "sourceScene": scene_name, "surfaces": cameras["surface_ids"],
            "cameraCounts": cameras["counts"], "internalUndirectedEdges": 24,
            "directedRecipeLinks": 48, "frames": frames, "sourceFiles": files,
            "preservedAuthorInputs": copies, "outputs": proposed,
            "costs": {"sourceClosureDiskBytes": sum(row["bytes"] for row in files.values()),
                      "authorCopiesBytesPerMode": sum(row["bytes"] for row in copies.values()),
                      "framePayloadBytesPerMode": sum(row["payloadBytes"] for row in frames),
                      "coarseMip4To13PayloadBytes": 448 * 136 * 136 * 12,
                      "rootsPayloadBytes": 16 * 136 * 136 * 12,
                      "frozenMeasuredInputGpuEnvelopeBytes": frozen["measured"]["sharedInputMemoryRequirementsEnvelopeBytes"],
                      "frozenInputGpuCapBytes": frozen["steadyRvtCaps"]["inputGpuBytes"],
                      "scope": "Disk exact referenced bytes; frame/coarse layout arithmetic; GPU envelope historical P0, new actual allocations and compressed/dedup production closure not yet measured"},
            "rendererStarted": False, "cookStarted": False, "acceptancePassed": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--author-scene", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--generate", action="store_true")
    options = parser.parse_args()
    if options.output.exists():
        raise ValueError("Refusing to overwrite generation receipt")
    resources = require_topic_root(ROOT)
    report = source_preflight(resources, options.author_scene)
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    if options.generate:
        report["generation"] = {}
        for mode in WEIGHT_SOURCES:
            result = build(resources, 8192, full_island=True, weight_source=mode,
                           new_v2=True, author_scene=options.author_scene)
            for entry in result["authorCopies"]:
                source = reference_path(resources, entry["source"])
                copied = reference_path(resources, entry["copy"])
                if artifact(source)["sha256"] != entry["sha256"] or artifact(copied)["sha256"] != entry["sha256"]:
                    raise ValueError("Author input/copy changed during generation")
            result["files"] = [artifact(path) for path in sorted((resources / Path(result["scene"]).parent).rglob("*"))
                               if path.is_file()]
            report["generation"][mode] = result
            options.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        report["status"] = "author-assets-generated-not-cooked"
        options.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("status", "cameraCounts", "outputs", "costs")}, indent=2))


if __name__ == "__main__":
    main()
