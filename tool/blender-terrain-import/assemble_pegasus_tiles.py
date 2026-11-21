"""Convert and merge a split Pegasus terrain tile set into one scene."""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
from pathlib import Path
from typing import Any


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Assemble Pegasus terrain tiles")
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--tile-layout", type=Path, required=True)
    parser.add_argument("--output-scene", type=Path)
    parser.add_argument("--skip-convert", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--water-plane-mesh",
        help="SM_*.json path (relative to the resource root) for the SeaSurface "
        "mesh; when omitted the merged scene has no independent water plane",
    )
    parser.add_argument(
        "--water-plane-height-cm",
        type=float,
        default=100.0,
        help="world Y offset of the water plane in cm (default 100 = 1 m above "
        "sea level, so it never z-fights the terrain minimum)",
    )
    parser.add_argument(
        "--camera-near-clip-cm",
        type=float,
        default=10000.0,
        help="camera near clip in cm for merged scenes (default 10000; the old "
        "10 cm near plane loses depth precision at 4 km and z-fights the sea)",
    )
    parser.add_argument(
        "--camera-far-clip-cm",
        type=float,
        default=6000000.0,
        help="camera far clip in cm for merged scenes (default 60 km so the "
        "100 km sea plane reaches past the top of the frame)",
    )
    parser.add_argument(
        "--character-blueprint",
        default="Maps/SC_valley_echo/Blueprints/BP_echo_character.json",
        help="C# player character blueprint placed in the merged scene; "
        "empty disables the character",
    )
    parser.add_argument(
        "--character-position",
        type=float,
        nargs=3,
        default=[5000.0, 18660.9, 25000.0],
        metavar=("X", "Y", "Z"),
        help="character spawn position in cm",
    )
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"JSON root must be an object: {path}")
    return value


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def _load_converter():
    path = Path(__file__).resolve().parents[1] / "terrain-ue-import" / (
        "build_vulkanlearn_assets.py")
    spec = importlib.util.spec_from_file_location("build_vulkanlearn_assets", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load converter: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _prepare_composited_materials(
    resource_root: Path,
    tile_entries: list[dict[str, Any]],
    verbose: bool,
) -> None:
    material_paths: set[Path] = set()
    for tile in tile_entries:
        output_root = resource_root / tile["outputRelative"]
        terrain = _load_json(output_root / f"TR_terrain_{tile['assetName']}.json")
        if terrain.get("compositedVT", {}).get("enabled") is not True:
            raise RuntimeError(
                "Terrain assembly requires compositedVT.enabled=true; regenerate and Cook every tile")
        material_path = resource_root / terrain["materialInstancePath"]
        if material_path.resolve() in material_paths:
            raise RuntimeError("Composited surfaces require independent material instances; regenerate tiles")
        material_paths.add(material_path.resolve())
        material = _load_json(material_path)
        material.setdefault("textures", {}).update({
            "globalColour": "Generated/HoudiniPegasusIsland/Global/T_global_colour.json",
            "globalMasks": "Generated/HoudiniPegasusIsland/Global/T_global_masks.json",
        })
        _write_json(material_path, material)
        if verbose:
            print(f"independent composited terrain material -> {material_path}")


def main() -> int:
    args = _parse_args()
    resource_root = args.resource_root.resolve()
    layout_path = args.tile_layout.resolve()
    layout = _load_json(layout_path)
    raw_tiles = layout.get("tiles")
    if not isinstance(raw_tiles, list) or not raw_tiles:
        raise RuntimeError("tile layout has no tiles")

    tiles: list[dict[str, Any]] = []
    for entry in raw_tiles:
        if not isinstance(entry, dict):
            raise RuntimeError("tile layout entries must be objects")
        manifest_path = Path(entry["manifest"]).resolve()
        manifest = _load_json(manifest_path)
        asset_name = manifest.get("assetName")
        output_relative = manifest.get("outputRelative")
        if not isinstance(asset_name, str) or not asset_name:
            raise RuntimeError(f"tile manifest has no assetName: {manifest_path}")
        if not isinstance(output_relative, str) or not output_relative:
            raise RuntimeError(f"tile manifest has no outputRelative: {manifest_path}")
        tiles.append({
            "index": int(entry.get("index", len(tiles))),
            "gridX": int(entry.get("gridX", 0)),
            "gridZ": int(entry.get("gridZ", 0)),
            "manifestPath": manifest_path,
            "manifest": manifest,
            "assetName": asset_name,
            "outputRelative": output_relative,
        })

    if not args.skip_convert:
        converter = _load_converter()
        for tile in tiles:
            if args.verbose:
                print(f"convert {tile['manifestPath']}")
            converter._convert(
                tile["manifestPath"],
                resource_root,
                None,
                None,
                args.verbose,
            )

    _prepare_composited_materials(resource_root, tiles, args.verbose)

    scenes: list[dict[str, Any]] = []
    for tile in tiles:
        output_root = resource_root / tile["outputRelative"]
        scene_path = output_root / f"SC_terrain_{tile['assetName']}.json"
        scene = _load_json(scene_path)
        objects = scene.get("objects")
        if not isinstance(objects, list):
            raise RuntimeError(f"tile scene has no objects array: {scene_path}")
        terrain = next(
            (obj for obj in objects if isinstance(obj, dict) and
             obj.get("type") == "terrain"), None)
        if terrain is None:
            raise RuntimeError(f"tile scene has no terrain object: {scene_path}")
        terrain = dict(terrain)
        terrain["name"] = f"Landscape_{tile['index']:02d}"
        terrain["position"] = [
            float(value) for value in tile["manifest"]["terrainPosition"]]
        scenes.append({
            "scene": scene,
            "terrain": terrain,
        })

    output_scene = (args.output_scene or layout_path.parent / "SC_terrain_pegasus_4x4.json").resolve()
    recipe_directory = output_scene.parent
    if not recipe_directory.is_relative_to(resource_root):
        raise RuntimeError("World output escapes resources")
    validation_tools = Path(__file__).resolve().parents[1] / "validation/terrain"
    specification = importlib.util.spec_from_file_location(
        "terrain_world_authoring", validation_tools / "build_composited_fixture.py")
    authoring = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(authoring)
    cells = {(tile["gridX"], tile["gridZ"]): f"terrain|Landscape_{tile['index']:02d}"
             for tile in tiles}
    if len(cells) != len(tiles):
        raise RuntimeError("Duplicate tile grid coordinates")
    recipe = authoring.world_author_recipe(list(cells.values()), cells)
    recipe_name = (recipe_directory / "world.author.json").relative_to(resource_root).as_posix()
    package_name = (recipe_directory / "Published/manifest.json").relative_to(resource_root).as_posix()
    for entry in scenes:
        terrain_path = resource_root / entry["terrain"]["terrainPath"]
        descriptor = _load_json(terrain_path)
        settings = descriptor["compositedVT"]
        if set(settings) != {"enabled", "virtualResolution", "packagePath", "productionSourcePath"}:
            raise RuntimeError("Retired terrain schema; regenerate tiles before assembly")
        settings.update({"packagePath": package_name, "productionSourcePath": recipe_name})
        _write_json(terrain_path, descriptor)
    _write_json(resource_root / recipe_name, recipe)

    template = scenes[0]["scene"]
    merged_objects = [
        obj for obj in template.get("objects", [])
        if isinstance(obj, dict) and obj.get("type") != "terrain"
    ]
    merged_objects = [entry["terrain"] for entry in scenes] + merged_objects
    if args.character_blueprint:
        merged_objects.insert(
            len(scenes),
            {
                "name": "Echo Player",
                "type": "blueprintInstance",
                "id": "echo",
                "blueprintPath": args.character_blueprint,
                "position": [float(value) for value in args.character_position],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "properties": {},
            },
        )
    if args.water_plane_mesh:
        # 独立水面必须排在地形之后、灯光/相机之前，便于人读场景时保持地形块连续。
        merged_objects.insert(
            len(scenes),
            {
                "name": "SeaSurface",
                "type": "mesh",
                "modelPath": args.water_plane_mesh,
                "position": [0.0, args.water_plane_height_cm, 0.0],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            },
        )
    span_cm = float(layout.get("tileGrid", 4)) * float(
        layout.get("tileSizeCm", 100000.0))
    height_scale_cm = float(tiles[0]["manifest"].get("heightScale", 0.0))
    height_scale_m = height_scale_cm / 100.0
    for obj in merged_objects:
        if obj.get("type") == "camera":
            obj["near_clip"] = args.camera_near_clip_cm
            if height_scale_m < 250.0:
                obj["position"] = [0.0, span_cm * 0.25, span_cm * 0.8]
                obj["rotation"] = [-15.0, 0.0, 0.0]
            else:
                obj["position"] = [0.0, span_cm * 0.75, span_cm * 1.6]
                obj["rotation"] = [-25.0, 0.0, 0.0]
            obj["far_clip"] = max(
                float(obj.get("far_clip", 0.0)),
                span_cm * 3.0,
                args.camera_far_clip_cm)
    merged = {
        "name": "SC_terrain_pegasus_4x4",
        "type": "scene",
        "description": (
            "Pegasus PCG 4x4 地形切片：一张 4km 全局高度/河网/层权重场切成 "
            "16 个 1km tile，共享边界采样、layer key、纹理数组与 32x32 VT 页表；"
            "场景内放置 Echo 状态机角色用于地形跑图验收，不参与论文数值验证。"
        ),
        "worldUnit": "cm",
        "objects": merged_objects,
    }
    for key in ("runtimeMode", "projectile", "spawnAssets"):
        if key in template:
            merged[key] = template[key]

    output_scene = args.output_scene
    if output_scene is None:
        output_scene = layout_path.parent / "SC_terrain_pegasus_4x4.json"
    else:
        output_scene = output_scene.resolve()
    _write_json(output_scene, merged)
    print(json.dumps({
        "scene": str(output_scene),
        "tiles": len(tiles),
        "objects": len(merged_objects),
        "renderReady": False,
        "requiredCookIdentity": {
            "generatorVersion": 3,
            "filterRule": "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3",
            "encoding": "gpu-rvt-adjacency-area4-rgba8-v3",
        },
        "nextStep": "Cook the complete World recipe before 3D loading; budgets come from P0.",
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
