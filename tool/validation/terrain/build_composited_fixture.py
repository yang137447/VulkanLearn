"""Generate the topic-local real-tile composited VT authoring fixture.

Run from any directory with Python 3. No third-party Python packages are needed.
The renderer's Cook command must subsequently populate each Cook manifest; this
script never invents composited material pixels or changes the source tiles.
"""

import argparse
import copy
import hashlib
import itertools
import json
import math
import os
import re
from pathlib import Path
import subprocess
import tempfile
from rvt_generation_identity import COOK_IDENTITY


FIXTURE = Path("Generated/Validation/TerrainCompositedVT")
SOURCE = Path("Generated/HoudiniPegasusIsland")
TILES = ("00", "01", "04", "05")
ARRAY_PLACEHOLDER_SLOTS = (
    "compositedPageTable", "compositedAlbedo", "compositedNormal",
    "compositedOrm", "terrainHeightAtlas",
)
DATA_PLACEHOLDER_SLOTS = ("terrainHeightLayerMap", "terrainLayerSourceMap")
V2_SCENE = "Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_CoverageAtlas/SC_composited_vt_4x4.json"
WEIGHT_SOURCES = ("CoverageAtlas", "LocalLayerArray")


def world_author_recipe(surface_ids, known_cells=None):
    ids = list(surface_ids)
    if not ids or len(set(ids)) != len(ids) or any(
            not re.fullmatch(r"terrain\|Landscape_\d{2}", name) for name in ids):
        raise ValueError("World surfaces require unique terrain|Landscape_XX identities")
    links = []
    if known_cells is not None:
        if set(known_cells.values()) != set(ids) or len(known_cells) != len(ids):
            raise ValueError("Known split topology differs from World surfaces")
        for (column, row), source in sorted(known_cells.items()):
            for neighbor, first, second, offset in (
                    ((column + 1, row), "maxU", "minU", [1, 0, -1, 0, 1, 0]),
                    ((column, row + 1), "maxV", "minV", [1, 0, 0, 0, 1, -1])):
                if neighbor not in known_cells:
                    continue
                target = known_cells[neighbor]
                for origin, destination, edge, other, transform in (
                        (source, target, first, second, offset),
                        (target, source, second, first,
                         [offset[0], offset[1], -offset[2], offset[3], offset[4], -offset[5]])):
                    links.append({"sourceSurfaceId": origin, "targetSurfaceId": destination,
                                  "sourceEdge": edge, "targetEdge": other,
                                  "sourceEdgeInterval": [0, 1], "targetEdgeInterval": [0, 1],
                                  "sourceUvToTargetUv": transform})
    return {"kind": "terrain-rvt-world-author", "version": 1, "surfaces": sorted(ids), "adjacency": links}


def validate_author_world(resource_root, scene_name, expected_surfaces=16):
    scene = read_json(reference_path(resource_root, scene_name))
    placements = [item for item in scene["objects"] if item.get("type") == "terrain"]
    ids = ["terrain|" + item["name"] for item in placements]
    world_author_recipe(ids)
    if len(ids) != expected_surfaces:
        raise ValueError(f"Expected {expected_surfaces} terrain placements")
    manifests, recipes, terrains = set(), set(), []
    for placement in placements:
        path = reference_path(resource_root, placement["terrainPath"])
        terrain = read_json(path)
        settings = terrain.get("compositedVT", {})
        if set(settings) != {"enabled", "packagePath", "productionSourcePath", "virtualResolution"}:
            raise ValueError(f"V2 requires explicit author schema; retired/unknown settings: {path}")
        if settings["enabled"] is not True:
            raise ValueError("Terrain compositedVT must be explicitly enabled")
        resolution = settings["virtualResolution"]
        if not isinstance(resolution, list) or len(resolution) != 2 or any(
                type(value) is not int or value < 1 or value > 1 << 24 for value in resolution):
            raise ValueError("Invalid V2 virtual resolution")
        for name in ("packagePath", "productionSourcePath"):
            value = settings[name]
            if not isinstance(value, str) or not value or "\\" in value or ":" in value or (
                    Path(value).is_absolute() or Path(value).root or ".." in Path(value).parts):
                raise ValueError(f"Unsafe World {name}")
        manifests.add(settings["packagePath"])
        recipes.add(settings["productionSourcePath"])
        terrains.append({"placement": placement, "terrain": terrain})
    if len(manifests) != 1 or len(recipes) != 1:
        raise ValueError("All TRs must select one shared World manifest and author recipe")
    recipe = read_json(reference_path(resource_root, next(iter(recipes))))
    if set(recipe) != {"kind", "version", "surfaces", "adjacency"} or (
            recipe["kind"] != "terrain-rvt-world-author" or type(recipe["version"]) is not int
            or recipe["version"] != 1 or sorted(recipe["surfaces"]) != sorted(ids)):
        raise ValueError("World author surface set/schema differs from scene")
    if not isinstance(recipe["adjacency"], list):
        raise ValueError("World adjacency must be explicit")
    for link in recipe["adjacency"]:
        if set(link) != {"sourceSurfaceId", "targetSurfaceId", "sourceEdge", "targetEdge",
                         "sourceEdgeInterval", "targetEdgeInterval", "sourceUvToTargetUv"}:
            raise ValueError("Invalid explicit adjacency fields")
        if link["sourceSurfaceId"] not in ids or link["targetSurfaceId"] not in ids:
            raise ValueError("Adjacency references an unknown surface")
        for field, length in (("sourceEdgeInterval", 2), ("targetEdgeInterval", 2),
                              ("sourceUvToTargetUv", 6)):
            values = link[field]
            if not isinstance(values, list) or len(values) != length or any(
                    type(value) not in (int, float) or not math.isfinite(value) for value in values):
                raise ValueError("Invalid adjacency interval/affine transform")
        if link["sourceEdge"] not in ("minU", "maxU", "minV", "maxV") or (
                link["targetEdge"] not in ("minU", "maxU", "minV", "maxV")):
            raise ValueError("Unknown adjacency edge")
        if link["sourceSurfaceId"] == link["targetSurfaceId"]:
            raise ValueError("Self adjacency is invalid")
        for field in ("sourceEdgeInterval", "targetEdgeInterval"):
            low, high = link[field]
            if not 0 <= low < high <= 1:
                raise ValueError("Invalid adjacency interval range")
        mapping = link["sourceUvToTargetUv"]
        determinant = mapping[0] * mapping[4] - mapping[1] * mapping[3]
        if abs(determinant) < 1e-12:
            raise ValueError("Singular adjacency transform")
        inverse = [mapping[4] / determinant, -mapping[1] / determinant,
                   (mapping[1] * mapping[5] - mapping[4] * mapping[2]) / determinant,
                   -mapping[3] / determinant, mapping[0] / determinant,
                   (mapping[3] * mapping[2] - mapping[0] * mapping[5]) / determinant]
        reciprocals = [other for other in recipe["adjacency"]
                       if other["sourceSurfaceId"] == link["targetSurfaceId"]
                       and other["targetSurfaceId"] == link["sourceSurfaceId"]
                       and other["sourceEdge"] == link["targetEdge"]
                       and other["targetEdge"] == link["sourceEdge"]
                       and other["sourceEdgeInterval"] == link["targetEdgeInterval"]
                       and other["targetEdgeInterval"] == link["sourceEdgeInterval"]
                       and len(other["sourceUvToTargetUv"]) == 6
                       and all(math.isclose(first, second, abs_tol=1e-9)
                               for first, second in zip(other["sourceUvToTargetUv"], inverse))]
        if len(reciprocals) != 1:
            raise ValueError("Adjacency needs exactly one inverse link")
        for other in recipe["adjacency"]:
            if other is link or other["sourceSurfaceId"] != link["sourceSurfaceId"] or (
                    other["sourceEdge"] != link["sourceEdge"]):
                continue
            if min(other["sourceEdgeInterval"][1], link["sourceEdgeInterval"][1]) > max(
                    other["sourceEdgeInterval"][0], link["sourceEdgeInterval"][0]):
                raise ValueError("Overlapping adjacency links")
    return {"scene": scene_name, "manifest": next(iter(manifests)), "recipe": next(iter(recipes)),
            "surface_ids": sorted(ids), "terrains": terrains, "author": recipe}


def published_world_files(resource_root, manifest_name, surface_ids):
    manifest = reference_path(resource_root, manifest_name)
    document = read_json(manifest)
    if document.get("kind") != "terrain-composited-vt" or document.get("version") != 2:
        raise ValueError("Only World V2 publications are accepted; V1 has no fallback")
    if sorted(item["surfaceId"] for item in document["surfaces"]) != sorted(surface_ids):
        raise ValueError("Published World surface set differs from placements")
    paths = [manifest, reference_path(manifest.parent, document["package"])]
    for entry in document["production"]["objects"]:
        paths.append(reference_path(manifest.parent, entry["path"]))
    for surface in document["production"]["surfaces"]:
        for field in ("runtimeMaterialInstancePath", "runtimeDetailMaterialInstancePath"):
            if surface.get(field):
                paths.append(reference_path(manifest.parent, surface[field]))
    return {path.relative_to(resource_root).as_posix(): path for path in paths}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def require_topic_root(code_root):
    # 只认代码工作树的同级资源工作树，并用分支名防止写进共享 master 或其他 topic；
    # 不写死某台机器的绝对路径，换机后同样的目录布局即可通过。
    resource_root = code_root.parent / "VukanLearnResources"
    if not resource_root.is_dir() or resource_root.is_symlink():
        raise ValueError("Fixture output must use the existing terrain-openworld resource worktree")
    branch = subprocess.check_output(
        ["git", "-C", str(resource_root), "branch", "--show-current"], text=True
    ).strip()
    if branch != "codex/terrain-openworld-assets":
        raise ValueError(f"Refusing resource branch {branch!r}")
    return resource_root.resolve()


def reference_path(resource_root, relative):
    resource_root = resource_root.resolve(strict=True)
    if not isinstance(relative, str) or not relative:
        raise ValueError("Empty resource reference")
    logical = Path(relative)
    if logical.is_absolute() or logical.drive or ".." in logical.parts:
        raise ValueError(f"Unsafe resource reference: {relative}")
    resolved = (resource_root / logical).resolve()
    if not resolved.is_relative_to(resource_root) or not resolved.is_file():
        raise ValueError(f"Missing or escaped source resource: {relative}")
    return resolved


def write_json(resource_root, relative, value):
    target = resource_root / relative
    allowed = (resource_root / FIXTURE).resolve()
    if not allowed.is_relative_to(resource_root) or not target.resolve().is_relative_to(allowed):
        raise ValueError(f"Escaped fixture output: {target}")
    target.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    if target.is_file() and target.read_text(encoding="utf-8") == text:
        return
    if target.exists():
        raise ValueError(f"Existing fixture JSON differs; generate a new V2 namespace: {target}")
    descriptor, temporary = tempfile.mkstemp(prefix=target.name + ".", dir=target.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
            output.write(text)
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def copy_author(resource_root, source_name, tile, reset, author_root=None):
    source = reference_path(resource_root, source_name)
    author_root = author_root or FIXTURE
    relative = author_root / f"Author/tile_{tile}" / source.name
    target = resource_root / relative
    allowed = (resource_root / author_root / "Author").resolve()
    if not allowed.is_relative_to(resource_root) or not target.resolve().is_relative_to(allowed):
        raise ValueError(f"Escaped author copy: {target}")
    if reset and not source.name.endswith(".meta.json") and not Path(str(source) + ".meta.json").is_file():
        target_meta = Path(str(target) + ".meta.json")
        if not target_meta.resolve().is_relative_to(allowed):
            raise ValueError(f"Escaped author sidecar reset: {target_meta}")
        if target_meta.exists():
            if not target_meta.is_file():
                raise ValueError(f"Author sidecar is not a file: {target_meta}")
            target_meta.unlink()
    source_bytes = source.read_bytes()
    if target.exists():
        if not target.is_file():
            raise ValueError(f"Author copy is not a file: {target}")
        if target.read_bytes() == source_bytes:
            return relative, hashlib.sha256(source_bytes).hexdigest()
        if not reset:
            raise ValueError(
                f"Author copy differs from source (edited data preserved): {target}; "
                "use --reset only to explicitly discard these author edits"
            )
    target.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=target.name + ".", dir=target.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(source_bytes)
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return relative, hashlib.sha256(source_bytes).hexdigest()


def build(resource_root, virtual_resolution=1024, height_blend=None, near_100m=False, reset=False,
          full_island=False, weight_source="CoverageAtlas", new_v2=False, author_scene=None):
    from rvt_author_semantics import author_parameter, remove_default_parameters, paint_semantics
    material_schema = read_json(Path(__file__).resolve().parents[3] / "shader/glsl/M_terrain.json")
    if not new_v2 or weight_source not in WEIGHT_SOURCES:
        raise ValueError("Explicit --new-v2 and a supported weight source are required")
    destination = FIXTURE
    if full_island and near_100m:
        raise ValueError("Full island cannot use the near-100m fixture")
    if full_island or virtual_resolution != 1024 or height_blend is not None or near_100m:
        mode = "Default" if height_blend is None else f"HeightBlend{height_blend:g}"
        extent = "Full4x4" if full_island else "Near100m" if near_100m else "Real2x2"
        destination /= f"Variants/RVTv2_{extent}_R{virtual_resolution}_{mode}_{weight_source}"
    else:
        destination /= f"Variants/RVTv2_Real2x2_R1024_Default_{weight_source}"
    manifest_path = Path("VT/worldV2") / destination.name / "manifest.json"
    recipe_path = destination / "world.author.json"
    if (resource_root / destination).exists():
        raise ValueError("V2 generation requires a fresh namespace; existing author copies are preserved")
    original = read_json(reference_path(resource_root, author_scene or
                                        (SOURCE / "SC_terrain_pegasus_4x4.json").as_posix()))
    outputs = {}
    array_placeholder = read_json(reference_path(
        resource_root, "Common/Textures/T_TerrainLayerArrayPlaceholder.json"))
    data_placeholder = read_json(reference_path(
        resource_root, "Common/Textures/T_TerrainLayerPageTablePlaceholder.json"))
    reference_path(resource_root, "Common/Source/Textures/terrain_layer_placeholder.bmp")
    if array_placeholder["type"] != "textureArray" or len(array_placeholder["sources"]) != 1:
        raise ValueError("Expected an existing single-layer array placeholder")
    if data_placeholder["type"] != "texture":
        raise ValueError("Expected an existing 2D data placeholder")
    placeholder_root = destination if full_island else FIXTURE
    array_path = placeholder_root / "Textures/T_composited_array_placeholder.json"
    data_path = placeholder_root / "Textures/T_composited_data_placeholder.json"
    array_placeholder["name"] = array_path.stem
    data_placeholder["name"] = data_path.stem
    outputs[array_path] = array_placeholder
    outputs[data_path] = data_placeholder
    objects = []
    source_records = []
    author_records = []
    sidecar_records = []
    bounds_min = [math.inf] * 3
    bounds_max = [-math.inf] * 3
    tiles = tuple(f"{index:02d}" for index in range(16)) if full_island else TILES[:1] if near_100m else TILES
    for tile in tiles:
        source_tr = (Path(next(item["terrainPath"] for item in original["objects"]
                              if item.get("type") == "terrain" and item["name"] == f"Landscape_{tile}"))
                     if author_scene else SOURCE / f"tile_{tile}/TR_terrain_houdini_island_tile_{tile}.json")
        terrain = read_json(reference_path(resource_root, source_tr.as_posix()))
        for field in ("heightmapPath", "paintPath", "materialInstancePath", "detailMaterialInstancePath"):
            if field in terrain:
                reference_path(resource_root, terrain[field])
        for field in ("heightmapPath", "paintPath"):
            if field not in terrain:
                continue
            source_name = terrain[field]
            copied, digest = copy_author(
                resource_root, source_name, tile, reset, destination)
            terrain[field] = copied.as_posix()
            author_records.append({"source": source_name, "copy": copied.as_posix(), "sha256": digest})
            sidecar = source_name + ".meta.json"
            sidecar_records.append({"field": field, "source": sidecar, "copy": copied.as_posix() + ".meta.json",
                                    "sourceExists": (resource_root / sidecar).is_file()})
            if (resource_root / sidecar).is_file():
                copied_meta, meta_digest = copy_author(
                    resource_root, sidecar, tile, reset, destination)
                author_records.append({"source": sidecar, "copy": copied_meta.as_posix(), "sha256": meta_digest})
        material = read_json(reference_path(resource_root, terrain["materialInstancePath"]))
        detail = read_json(reference_path(resource_root, terrain["detailMaterialInstancePath"]))
        for instance in (material, detail):
            author_parameter(instance, material_schema, "u_terrainWeightMapMode",
                             0 if weight_source == "CoverageAtlas" else 1)
            for parameter in (
                "u_heightBlendDistanceFade", "u_terrainLayerMode", "u_layerForceMip", "u_layerMaxMip",
                "u_layerPageResolution", "u_layerAtlasResolution", "u_layerPagesPerAtlasSlice",
                "u_layerTilesPerAtlasSide", "u_compositedEnabled", "u_terrainHeightAtlasEnabled",
            ):
                instance.setdefault("parameters", {}).pop(parameter, None)
            for slot in list(instance.get("textures", {})):
                if slot.startswith("layerPageTable") or slot in (
                    "layerAlbedoAtlas", "layerNormalAtlas", "layerPbrAtlas",
                ):
                    del instance["textures"][slot]
            for reference in instance.get("textures", {}).values():
                reference_path(resource_root, reference)
            textures = instance.setdefault("textures", {})
            for slot in ARRAY_PLACEHOLDER_SLOTS:
                textures[slot] = array_path.as_posix()
            for slot in DATA_PLACEHOLDER_SLOTS:
                textures[slot] = data_path.as_posix()
        if height_blend is not None:
            for instance in (material, detail):
                author_parameter(instance, material_schema, "u_heightBlendStrength", height_blend)
        for instance in (material, detail):
            remove_default_parameters(instance, material_schema)
        material_path = destination / f"MI_composited_tile_{tile}.json"
        detail_path = destination / f"MI_composited_tile_{tile}_detail.json"
        material["name"] = material_path.stem
        detail["name"] = detail_path.stem
        terrain["name"] = f"TR_composited_tile_{tile}"
        terrain["description"] = (
            f"真实 Pegasus tile {tile} 的合成 VT 学习验证夹具："
            "验证跨地块地址、材质合成、父 mip 回退与常驻 tiling 高度；"
            "复用原始高度、Paint 和纹理，不是论文数值验证场景。"
        )
        terrain["materialInstancePath"] = material_path.as_posix()
        terrain["detailMaterialInstancePath"] = detail_path.as_posix()
        if near_100m:
            ratio = 10000.0 / terrain["size"][0]
            terrain["size"] = [10000.0, 10000.0]
            terrain["heightScale"] *= ratio
            terrain["heightOffset"] *= ratio
            terrain["description"] += "独立100m缩尺质量夹具；不改变真实2×2地块尺寸。"
        terrain["compositedVT"] = {
            "enabled": True,
            "packagePath": manifest_path.as_posix(),
            "virtualResolution": [virtual_resolution, virtual_resolution],
            "productionSourcePath": recipe_path.as_posix(),
        }
        tr_path = destination / f"TR_composited_tile_{tile}.json"
        placement = copy.deepcopy(next(
            item for item in original["objects"]
            if item.get("type") == "terrain" and item.get("terrainPath") == source_tr.as_posix()
        ))
        placement["terrainPath"] = tr_path.as_posix()
        if near_100m:
            placement["position"] = [0.0, 0.0, 0.0]
            placement["name"] = "Landscape_00"
        if placement["rotation"] != [0.0, 0.0, 0.0] or placement["scale"] != [1.0, 1.0, 1.0]:
            raise ValueError("Camera bounds require the source tiles' unrotated unit placement")
        low_height = min(0.0, terrain["heightScale"]) + terrain["heightOffset"]
        high_height = max(0.0, terrain["heightScale"]) + terrain["heightOffset"]
        displacement_margin = max(abs(layer.get("displacementM", 0)) for layer in terrain["layers"]) * 100
        for axis, low, high in (
            (0, -terrain["size"][0] / 2, terrain["size"][0] / 2),
            (1, low_height - displacement_margin, high_height + displacement_margin),
            (2, -terrain["size"][1] / 2, terrain["size"][1] / 2),
        ):
            bounds_min[axis] = min(bounds_min[axis], placement["position"][axis] + low)
            bounds_max[axis] = max(bounds_max[axis], placement["position"][axis] + high)
        objects.append(placement)
        outputs[material_path] = material
        outputs[detail_path] = detail
        outputs[tr_path] = terrain
        source_records.append({
            "tile": tile,
            "sourceTerrain": source_tr.as_posix(),
            "terrain": tr_path.as_posix(),
            "manifest": manifest_path.as_posix(),
        })
    for item in original["objects"]:
        if item.get("type") in ("directionalLight", "environment"):
            objects.append(copy.deepcopy(item))
        elif full_island and item.get("type") == "mesh" and item.get("name") == "SeaSurface":
            reference_path(resource_root, item["modelPath"])
            objects.append(copy.deepcopy(item))
        elif item.get("type") == "camera":
            camera = copy.deepcopy(item)
            target = [(low + high) / 2 for low, high in zip(bounds_min, bounds_max)]
            radius = math.sqrt(sum(((high - low) / 2) ** 2 for low, high in zip(bounds_min, bounds_max)))
            horizontal_half_fov = math.radians(65.0 / 2)
            distance = 0.0
            for corner in itertools.product(*zip(bounds_min, bounds_max)):
                relative = [component - center for component, center in zip(corner, target)]
                camera_up = relative[1] * 0.8 - relative[2] * 0.6
                required_depth = max(abs(relative[0]), abs(camera_up) * (21 / 9))
                required_depth /= math.tan(horizontal_half_fov) * 0.82
                distance = max(distance, relative[1] * 0.6 + relative[2] * 0.8 + required_depth)
            camera["position"] = [
                round(target[0], 6),
                round(target[1] + distance * 0.6, 6),
                round(target[2] + distance * 0.8, 6),
            ]
            camera["look_at"] = [round(component, 6) for component in target]
            camera["fov"] = 65.0
            camera["near_clip"] = 100.0
            camera["far_clip"] = math.ceil(distance + radius * 2)
            objects.append(camera)
    scene_path = destination / (
        "SC_composited_vt_4x4.json" if full_island
        else "SC_composited_vt_100m.json" if near_100m else "SC_composited_vt_2x2.json")
    outputs[scene_path] = {
        "name": scene_path.stem,
        "type": "scene",
        "description": (
            "复用 Pegasus 真实 tile00/01/04/05 的最小 2×2 合成 VT 学习验证场景："
            "覆盖 X/Z 接缝与四块交点、Cook 页面包闭包、流式父页回退及 tiling 高度。"
            "这是工程学习验收夹具，不是论文数值验证；页面包须由共享 GPU 评价器 Cook，"
            "本脚本不生成替代材质像素。"
        ),
        "worldUnit": "cm",
        "objects": objects,
    }
    if near_100m:
        outputs[scene_path]["description"] = (
            "独立100m地形近场合成VT质量学习夹具，复用真实tile00高度和Paint并按比例缩尺高度；"
            "验证virtualResolution与texels/m的关系，不是论文数值验证，"
            "不能代替真实1km地块的近景画质验收，也不改变真实2×2资产。"
        )
    if full_island:
        outputs[scene_path]["name"] = "Pegasus Island — Full Composited VT"
        outputs[scene_path]["description"] = (
            "真实 Pegasus 完整4×4飞马岛标准地形配置与用户入口：16块原尺寸新合成VT地形、原海面、"
            "灯光与环境，初始镜头完整覆盖全岛；每块独立高度/Paint作者副本及基础/detail材质，"
            "全视距静态高度混合，不含C#角色。此场景作为后续地形标准配置，"
            "不是论文数值验证或临时probe；首次运行前须由引擎完成全部16块新身份Cook。"
        )
    from check_composited_fixture_camera import png16
    first_placement = next(item for item in objects if item["type"] == "terrain")
    first_terrain = outputs[Path(first_placement["terrainPath"])]
    height_rows = png16(resource_root / first_terrain["heightmapPath"])
    center = first_terrain["heightmapMargin"] + first_terrain["gridResolution"] // 2
    center_height = height_rows[center][center] / 65535 * first_terrain["heightScale"] + first_terrain["heightOffset"]
    target = [
        first_placement["position"][0],
        first_placement["position"][1] + center_height,
        first_placement["position"][2],
    ]
    overview = next(item for item in objects if item["type"] == "camera")
    shot_plan = [
        {"name": "far", "position": overview["position"], "look_at": overview["look_at"]},
        {"name": "mid", "position": [target[0], target[1] + 2500, target[2] + 5000], "look_at": target},
        {"name": "near", "position": [target[0], target[1] + 800, target[2] + 1200], "look_at": target},
    ]
    if full_island:
        centers = {}
        for tile in ("00", "05", "15"):
            placement = next(item for item in objects if item.get("name") == f"Landscape_{tile}")
            terrain = outputs[Path(placement["terrainPath"])]
            rows = png16(resource_root / terrain["heightmapPath"])
            center = terrain["heightmapMargin"] + terrain["gridResolution"] // 2
            height = rows[center][center] / 65535 * terrain["heightScale"] + terrain["heightOffset"]
            centers[tile] = [placement["position"][0], placement["position"][1] + height, placement["position"][2]]
        shot_plan = [shot_plan[0]]
        for label, tile, up, back in (
            ("mid", "05", 2500, 5000), ("near", "15", 800, 1200),
            ("tile00_near", "00", 800, 1200),
        ):
            center = centers[tile]
            shot_plan.append({
                "name": label, "tile": tile,
                "position": [center[0], center[1] + up, center[2] + back], "look_at": center,
            })
        terrain = outputs[destination / "TR_composited_tile_05.json"]
        rows = png16(resource_root / terrain["heightmapPath"])
        corner = terrain["heightmapMargin"] + terrain["gridResolution"]
        height = rows[corner][corner] / 65535 * terrain["heightScale"] + terrain["heightOffset"]
        target = [0.0, height, 0.0]
        shot_plan.append({
            "name": "cross_tile_corner", "tiles": ["05", "06", "09", "10"],
            "position": [0.0, height + 2500, 5000.0], "look_at": target,
        })
    pages = 0
    dimension = virtual_resolution
    while True:
        pages += math.ceil(dimension / 128) ** 2
        if dimension == 1:
            break
        dimension = max(1, dimension // 2)
    payload_bytes = pages * 136 * 136 * 12
    capture_path = destination / "capture_plan.json"
    outputs[capture_path] = {
        "requiredCookIdentity": COOK_IDENTITY,
        "kind": "terrain-composited-capture-plan",
        "compositedScene": scene_path.as_posix(),
        "terrainSizeCm": first_terrain["size"],
        "virtualResolution": [virtual_resolution, virtual_resolution],
        "metersPerTexel": first_terrain["size"][0] / 100 / virtual_resolution,
        "heightBlendOverride": height_blend,
        "fullMipPageCount": pages,
        "perSurfaceMaterialPayloadBytes": payload_bytes,
        "payloadScope": "Hypothetical full mip arithmetic only; V2 publishes sparse roots and inputs",
        "viewport": [1280, 720],
        "horizontalFov": overview["fov"],
        "settleFramesAtLeast": 120,
        "shots": shot_plan,
        "status": "Prepared; no renderer screenshots captured",
    }
    cells = {(int(tile) % 4, int(tile) // 4): f"terrain|Landscape_{tile}" for tile in tiles}
    outputs[recipe_path] = world_author_recipe(list(cells.values()), cells)
    outputs[destination / "cook_identity.json"] = COOK_IDENTITY
    outputs[destination / "paint_semantics.json"] = {
        "kind": "terrain-author-paint-semantics", "version": 1,
        "surfaces": [paint_semantics(resource_root, outputs[Path(item["terrainPath"])], item["terrainPath"])
                     for item in objects if item.get("type") == "terrain"],
        "scope": "Author input receipt; not runtime production closure or acceptance"}
    for relative, value in outputs.items():
        write_json(resource_root, relative, value)
    if full_island:
        from rvt_delivery import full16_camera_plan
        cameras = full16_camera_plan(resource_root, scene_path.as_posix(), resource_root / capture_path)
        outputs[capture_path]["shots"].extend(cameras["shots"])
        target = resource_root / capture_path
        target.write_text(json.dumps(outputs[capture_path], ensure_ascii=False, indent=2) + "\n",
                          encoding="utf-8")
    return {
        "scene": scene_path.as_posix(),
        "capturePlan": capture_path.as_posix(),
        "metersPerTexel": outputs[capture_path]["metersPerTexel"],
        "perSurfaceMaterialPayloadBytes": payload_bytes,
        "writtenOrUnchangedJsonFiles": len(outputs),
        "tiles": source_records,
        "authorCopies": author_records,
        "authorSidecarState": sidecar_records,
        "cookRequired": True,
        "protocol": 2, "productionSourcePath": recipe_path.as_posix(),
        "requiredCookIdentity": COOK_IDENTITY,
        "packagePath": manifest_path.as_posix(), "weightSource": weight_source,
        "fullIsland": full_island,
        "standardUserEntry": scene_path.as_posix() if full_island else None,
        "hypotheticalFullMipPayloadBytes": payload_bytes * len(tiles),
        "note": "Authoring fixture only; run the engine Cook entry point before loading enabled compositedVT.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--virtual-resolution", type=int, choices=(128, 256, 512, 1024, 2048, 4096, 8192))
    parser.add_argument("--height-blend", type=float, choices=(0.0, 1.0))
    parser.add_argument("--new-v2", action="store_true")
    parser.add_argument("--author-scene",
                        help="Preserve edited height/Paint by copying from this existing scene into a fresh V2 namespace")
    parser.add_argument("--dry-run", action="store_true",
                        help="Read Full16 author references and print new World V2 paths; never write resources")
    parser.add_argument("--weight-source", choices=WEIGHT_SOURCES, default="CoverageAtlas")
    parser.add_argument("--near-100m", action="store_true")
    parser.add_argument("--full-island", action="store_true",
        help="Generate the standard full-island terrain configuration: 16 tiles, sea/environment, independent author copies, no C# character")
    parser.add_argument("--reset", "--reset-author-copies", action="store_true",
        help="Explicitly discard edited fixture author height/Paint copies and restore source bytes")
    arguments = parser.parse_args()
    code_root = Path(__file__).resolve().parents[3]
    resource_root = require_topic_root(code_root)
    resolution = arguments.virtual_resolution or (8192 if arguments.full_island else 1024)
    if arguments.dry_run:
        if not arguments.full_island or not arguments.new_v2 or not arguments.author_scene:
            parser.error("--dry-run requires --full-island --new-v2 --author-scene")
        original = read_json(reference_path(resource_root, arguments.author_scene))
        placements = [item for item in original["objects"] if item.get("type") == "terrain"]
        ids = sorted("terrain|" + item["name"] for item in placements)
        expected = [f"terrain|Landscape_{index:02d}" for index in range(16)]
        if ids != expected:
            raise ValueError("Dry-run source must contain all Landscape_00..15 exactly once")
        mode = "Default" if arguments.height_blend is None else f"HeightBlend{arguments.height_blend:g}"
        namespace = f"RVTv2_Full4x4_R{resolution}_{mode}_{arguments.weight_source}"
        destination = FIXTURE / "Variants" / namespace
        authors = []
        for item in placements:
            terrain = read_json(reference_path(resource_root, item["terrainPath"]))
            authors.append({"surfaceId": "terrain|" + item["name"], "terrain": item["terrainPath"],
                            "preservedAuthorInputs": {
                                field: str(reference_path(resource_root, terrain[field]))
                                for field in ("heightmapPath", "paintPath") if field in terrain}})
        cells = {(index % 4, index // 4): name for index, name in enumerate(ids)}
        print(json.dumps({"status": "dry-run", "resourcesWritten": False, "rendererStarted": False,
                          "scene": (destination / "SC_composited_vt_4x4.json").as_posix(),
                          "packagePath": (Path("VT/worldV2") / namespace / "manifest.json").as_posix(),
                          "productionSourcePath": (destination / "world.author.json").as_posix(),
                          "recipe": world_author_recipe(ids, cells), "authorInputs": authors,
                          "defaultSceneCutover": "pending real offline Game/visual/performance acceptance",
                          "acceptancePassed": False}, ensure_ascii=False, indent=2))
        return
    print(json.dumps(build(resource_root, resolution, arguments.height_blend, arguments.near_100m,
        arguments.reset, arguments.full_island, arguments.weight_source, arguments.new_v2, arguments.author_scene),
        ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
