#!/usr/bin/env python3
"""Turn the 3ds Max export into VulkanLearn mesh/material/texture assets."""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageOps


CLOSURES = {
    "satin": "M_clothAshikhminSatin",
    "velvet": "M_clothAshikhminVelvet",
    "slanted_velvet": "M_clothAshikhminSlantedVelvet",
}

ROLE_BINDINGS = {
    "albedo": ("albedoMap", "USE_ALBEDO_MAP"),
    "normal": ("normalMap", "USE_NORMAL_MAP"),
    "pbr": ("pbrParamMap", "USE_PBR_MAP"),
    "emission": ("emissionMap", "USE_EMISSION_MAP"),
    "alpha": ("alphaMap", "USE_ALPHA_MAP"),
}


@dataclass
class MaterialRecord:
    group_id: str
    material_index: str
    material_name: str
    normalized_name: str
    diffuse_color: list[float] | None = None
    textures: list[dict[str, str]] = field(default_factory=list)
    generated_textures: list[dict[str, str]] = field(default_factory=list)


@dataclass
class GroupRecord:
    group_id: str
    root_name: str
    kind: str
    object_count: int
    material_count: int
    fbx_asset_path: str
    materials: list[MaterialRecord] = field(default_factory=list)
    mesh_rel_path: str = ""


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def read_csv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise SystemExit(f"missing required export manifest: {path}")
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def asset_path(assets_root: Path, relative: str) -> str:
    return (assets_root / relative).resolve().relative_to(assets_root.resolve()).as_posix()


def resolve_fbx(assets_root: Path, value: str) -> Path:
    path = Path(value)
    if path.is_absolute():
        return path
    return assets_root / path


def safe_token(value: str, fallback: str) -> str:
    cleaned = []
    for char in value.strip().lower():
        if char.isalnum():
            cleaned.append(char)
        elif char in "-_":
            cleaned.append(char)
        else:
            cleaned.append("_")
    result = "".join(cleaned).strip("_")
    return result or fallback


def srgb_to_linear(value: float) -> float:
    value = max(0.0, min(1.0, value))
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


def role_for_texture(slot_name: str, source_name: str) -> str | None:
    text = f"{slot_name} {source_name}".lower()
    if any(token in text for token in ("normal", "nrm", "tangent")):
        return "normal"
    if any(token in text for token in ("emissive", "emission", "selfillum", "self_illum")):
        return "emission"
    if any(token in text for token in ("opacity", "alpha", "cutout", "coverage", "mask")):
        return "alpha"
    if any(
        token in text
        for token in (
            "roughness",
            "glossiness",
            "metalness",
            "metallic",
            "ao",
            "occlusion",
            "orm",
        )
    ):
        return "pbr"
    if any(
        token in text
        for token in ("diffuse", "albedo", "basecolor", "base_color", "color", "colour")
    ):
        return "albedo"
    return None


def resolve_texture(showcase_root: Path, normalized: str) -> Path:
    path = Path(normalized)
    if path.is_absolute():
        return path
    candidate = showcase_root / "Source" / "Textures" / path.name
    if candidate.is_file():
        return candidate
    candidate = showcase_root / path
    if candidate.is_file():
        return candidate
    raise SystemExit(f"texture listed in textures.csv is missing: {normalized}")


def texture_json_payload(
    name: str,
    source_asset: str,
    color_space: str,
    channels: dict[str, str] | None = None,
) -> dict[str, object]:
    payload: dict[str, object] = {
        "name": name,
        "type": "texture",
        "source": source_asset,
        "colorSpace": color_space,
        "mipmaps": True,
        "filter": "linear",
        "wrapMode": "repeat",
    }
    if channels:
        payload["channelsDescription"] = channels
    return payload


def flip_normal_green(source: Path, target: Path) -> Path:
    with Image.open(source) as image:
        normal = image.convert("RGB")
        red, green, blue = normal.split()
        normal = Image.merge("RGB", (red, ImageOps.invert(green), blue))
        target.parent.mkdir(parents=True, exist_ok=True)
        normal.save(target)
    return target


def load_luma(path: Path, size: tuple[int, int], invert: bool = False) -> Image.Image:
    with Image.open(path) as image:
        luma = image.convert("L").resize(size, Image.Resampling.LANCZOS)
    if invert:
        luma = ImageOps.invert(luma)
    return luma


def pack_pbr(
    showcase_root: Path,
    materials: list[dict[str, str]],
    group_id: str,
    material_index: str,
    max_size: int,
) -> Path | None:
    if not materials:
        return None

    role_maps: dict[str, Path] = {}
    invert_roles: set[str] = set()
    for row in materials:
        slot = row["slot_name"].lower()
        source = resolve_texture(showcase_root, row["normalized"])
        if any(token in slot for token in ("orm", "pbr", "packed")):
            return source
        if "gloss" in slot:
            role_maps["roughness"] = source
            invert_roles.add("roughness")
        elif "rough" in slot:
            role_maps["roughness"] = source
        elif "metal" in slot:
            role_maps["metallic"] = source
        elif "ao" in slot or "occlusion" in slot:
            role_maps["ao"] = source

    if not role_maps:
        return None

    dimensions = []
    for source in role_maps.values():
        with Image.open(source) as image:
            dimensions.append(image.size)
    width = min(max(width for width, _ in dimensions), max_size)
    height = min(max(height for _, height in dimensions), max_size)
    size = (width, height)

    roughness = (
        load_luma(role_maps["roughness"], size, invert="roughness" in invert_roles)
        if "roughness" in role_maps
        else Image.new("L", size, 128)
    )
    metallic = (
        load_luma(role_maps["metallic"], size)
        if "metallic" in role_maps
        else Image.new("L", size, 0)
    )
    ambient_occlusion = (
        load_luma(role_maps["ao"], size)
        if "ao" in role_maps
        else Image.new("L", size, 255)
    )

    target = (
        showcase_root
        / "Source"
        / "Textures"
        / f"T_cloth_{group_id}_m{material_index}_pbr_01.png"
    )
    packed = Image.merge("RGB", (roughness, metallic, ambient_occlusion))
    packed.save(target)
    return target


def build_texture_asset(
    assets_root: Path,
    showcase_root: Path,
    texture_json_dir: Path,
    role: str,
    source: Path,
    stem: str,
    flip_green: bool,
) -> str:
    target_source = source
    if role == "normal" and flip_green:
        target_source = flip_normal_green(
            source,
            showcase_root / "Source" / "Textures" / f"{stem}_ng.png",
        )

    channels = None
    color_space = "linear"
    if role == "albedo":
        color_space = "srgb"
    elif role == "emission":
        color_space = "srgb"
    elif role == "normal":
        channels = {"r": "tangent x", "g": "tangent y", "b": "tangent z", "a": "unused"}
    elif role == "pbr":
        channels = {"r": "roughness", "g": "metallic", "b": "ambient occlusion", "a": "unused"}
    elif role == "alpha":
        channels = {"r": "coverage", "g": "unused", "b": "unused", "a": "unused"}

    source_asset = asset_path(assets_root, target_source)
    payload = texture_json_payload(stem, source_asset, color_space, channels)
    write_json(texture_json_dir / f"{stem}.json", payload)
    return asset_path(assets_root, texture_json_dir / f"{stem}.json")


def load_closure_map(path: Path | None) -> dict[str, str]:
    if path is None:
        return {}
    if not path.is_file():
        raise SystemExit(f"closure map not found: {path}")
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise SystemExit("closure map must be a JSON object")
    result = {}
    for key, value in payload.items():
        if value not in CLOSURES:
            raise SystemExit(f"unknown closure '{value}' for material '{key}'")
        result[str(key)] = value
    return result


def closure_for(
    closure_map: dict[str, str],
    group_id: str,
    material_index: str,
    default_closure: str,
) -> str:
    return (
        closure_map.get(f"{group_id}:m{material_index}")
        or closure_map.get(f"{group_id}:{material_index}")
        or closure_map.get(group_id)
        or default_closure
    )


def build_material_instance(
    assets_root: Path,
    showcase_root: Path,
    texture_json_dir: Path,
    material: MaterialRecord,
    closure: str,
    flip_green: bool,
    pbr_max_size: int,
) -> str:
    selected: dict[str, dict[str, str]] = {}
    for row in material.textures:
        role = role_for_texture(row["slot_name"], row["normalized"])
        if role is None:
            continue
        if role == "pbr":
            selected.setdefault("pbr", row)
            continue
        current = selected.get(role)
        if current is None or resolve_texture(
            showcase_root, row["normalized"]
        ).stat().st_size > resolve_texture(
            showcase_root, current["normalized"]
        ).stat().st_size:
            selected[role] = row

    textures: dict[str, str] = {
        "clothLut": "Common/Textures/T_clothAshikhminLut.json",
    }
    macros: dict[str, int] = {}
    texture_records = []

    for role in ("albedo", "normal", "emission", "alpha", "pbr"):
        if role not in selected:
            continue
        binding, macro = ROLE_BINDINGS[role]
        row = selected[role]
        if role == "pbr":
            pbr_rows = [item for item in material.textures if role_for_texture(
                item["slot_name"], item["normalized"]
            ) == "pbr"]
            source = pack_pbr(
                showcase_root,
                pbr_rows,
                material.group_id,
                material.material_index,
                pbr_max_size,
            )
            if source is None:
                continue
            stem = (
                f"T_cloth_{material.group_id}_m{material.material_index}_pbr_01"
                if source.name.startswith("T_cloth_")
                else source.stem
            )
        else:
            source = resolve_texture(showcase_root, row["normalized"])
            stem = Path(row["normalized"]).stem

        texture_json = build_texture_asset(
            assets_root,
            showcase_root,
            texture_json_dir,
            role,
            source,
            stem,
            flip_green,
        )
        textures[binding] = texture_json
        macros[macro] = 1
        texture_records.append(
            {
                "role": role,
                "slot": row["slot_name"],
                "source": row["normalized"],
                "texture": texture_json,
            }
        )

    material_path = (
        showcase_root
        / "Materials"
        / f"MI_cloth_{material.group_id}_m{material.material_index}.json"
    )
    parameters = {}
    if "albedo" in selected:
        parameters["u_baseColor"] = [1.0, 1.0, 1.0, 1.0]
    elif material.diffuse_color is not None:
        parameters["u_baseColor"] = [
            srgb_to_linear(material.diffuse_color[0]),
            srgb_to_linear(material.diffuse_color[1]),
            srgb_to_linear(material.diffuse_color[2]),
            1.0,
        ]

    payload: dict[str, object] = {
        "name": (
            f"3d66 cloth {material.group_id}.{material.material_index} "
            f"{closure}"
        ),
        "type": "materialInstance",
        "material": f"shader/glsl/{CLOSURES[closure]}.json",
    }
    if macros:
        payload["macros"] = macros
    if textures:
        payload["textures"] = textures
    if parameters:
        payload["parameters"] = parameters

    write_json(material_path, payload)
    material.generated_textures = texture_records
    return asset_path(assets_root, material_path)


def build_mesh_asset(
    assets_root: Path,
    showcase_root: Path,
    group: GroupRecord,
    material_paths: list[str],
) -> str:
    slots = []
    for index, material_path in enumerate(material_paths, start=1):
        slots.append(
            {
                "name": f"m{index:02d}",
                "materialInstancePath": material_path,
            }
        )
    payload = {
        "name": f"3d66 cloth {group.group_id}",
        "type": "mesh",
        "modelDataPath": group.fbx_asset_path,
        "materialSlots": slots,
    }
    mesh_path = showcase_root / "Meshes" / f"SM_cloth_{group.group_id}.json"
    write_json(mesh_path, payload)
    return asset_path(assets_root, mesh_path)


def camera_object(position: list[float], look_at: list[float], fov: float) -> dict:
    return {
        "name": "Camera",
        "type": "camera",
        "fov": fov,
        "near_clip": 1.0,
        "far_clip": 50000.0,
        "position": position,
        "look_at": look_at,
        "rotation": [0, 0, 0],
        "scale": [1, 1, 1],
        "exposure": {
            "mode": "ev100",
            "ev100": 9.0,
            "aperture": 4.0,
            "shutterSeconds": 0.008,
            "iso": 100.0,
            "compensation": 0.0,
            "autoMinEv100": -10.0,
            "autoMaxEv100": 20.0,
        },
    }


def directional_light(
    name: str,
    position: list[float],
    rotation: list[float],
    intensity: float,
    color: list[float],
) -> dict:
    return {
        "name": name,
        "type": "directionalLight",
        "position": position,
        "rotation": rotation,
        "color": color,
        "intensity": intensity,
        "intensityUnit": "lux",
        "shadow": {"dynamicShadowDistance": 6000.0},
    }


def stage_objects() -> list[dict]:
    return [
        directional_light(
            "KeyLight",
            [-250.0, 520.0, 520.0],
            [-25.0, -5.0, 0.0],
            5000.0,
            [1.0, 0.97, 0.92],
        ),
        directional_light(
            "FillLight",
            [420.0, 260.0, 320.0],
            [-22.0, 42.0, 0.0],
            1200.0,
            [0.72, 0.82, 1.0],
        ),
        directional_light(
            "RimLight",
            [0.0, 360.0, -520.0],
            [-52.0, 180.0, 0.0],
            2200.0,
            [1.0, 0.9, 0.78],
        ),
    ]


def studio_environment() -> dict:
    return {
        "name": "ClothStudioEnvironment",
        "type": "environment",
        "environment": {
            "type": "proceduralSky",
            "cubeSize": 128,
            "intensity": 0.45,
            "skyParameters": {
                "sunIntensity": 0.0,
                "sunColor": [1.0, 0.96, 0.9],
                "sunAngularRadius": 0.02,
                "zenithColor": [0.015, 0.02, 0.03],
                "horizonColor": [0.12, 0.13, 0.15],
                "groundColor": [0.008, 0.009, 0.012],
                "skyGradientExponent": 0.65,
                "groundGradientExponent": 0.5,
                "sunHaloExponent": 128.0,
                "sunHaloStrength": 0.0,
            },
        },
    }


def scene_payload(
    name: str,
    description: str,
    objects: list[dict],
) -> dict:
    return {
        "name": name,
        "type": "scene",
        "description": description,
        "objects": objects,
        "worldUnit": "cm",
    }


def build_scenes(
    showcase_root: Path,
    groups: list[GroupRecord],
    columns: int,
    spacing_x: float,
    spacing_z: float,
    model_scale: float,
) -> None:
    for group in groups:
        mesh_asset = (
            f"Maps/SC_cloth_showcase/Meshes/SM_cloth_{group.group_id}.json"
        )
        objects = stage_objects()
        objects.extend(
            [
                {
                    "name": f"Cloth_{group.group_id}",
                    "type": "mesh",
                    "modelPath": mesh_asset,
                    "position": [0.0, 0.0, 0.0],
                    "rotation": [0, 0, 0],
                    "scale": [model_scale, model_scale, model_scale],
                },
                camera_object([0.0, 105.0, 430.0], [0.0, 90.0, 0.0], 35.0),
                studio_environment(),
            ]
        )
        write_json(
            showcase_root / f"SC_cloth_{group.group_id}.json",
            scene_payload(
                f"Cloth {group.group_id}",
                (
                    "3d66 服装资产单件实时预览：使用 Ashikhmin、Premože 与 Shirley 2000 "
                    "forward-only 布料闭包，验证 3d66 贴图与服装网格的运行时接入；"
                    "不是论文像素复核场景。"
                ),
                objects,
            ),
        )

    rows = max(1, math.ceil(len(groups) / columns))
    objects = stage_objects()
    for index, group in enumerate(groups):
        row = index // columns
        column = index % columns
        objects.append(
            {
                "name": f"Cloth_{group.group_id}",
                "type": "mesh",
                "modelPath": f"Maps/SC_cloth_showcase/Meshes/SM_cloth_{group.group_id}.json",
                "position": [
                    (column - (columns - 1) * 0.5) * spacing_x,
                    0.0,
                    -(row - (rows - 1) * 0.5) * spacing_z,
                ],
                "rotation": [0, 0, 0],
                "scale": [model_scale, model_scale, model_scale],
            }
        )
    objects.extend(
        [
            camera_object(
                [0.0, max(1100.0, rows * spacing_z * 0.55), rows * spacing_z + 1800.0],
                [0.0, 90.0, 0.0],
                42.0,
            ),
            studio_environment(),
        ]
    )
    write_json(
        showcase_root / "SC_cloth_showcase.json",
        scene_payload(
            "3d66 Cloth Showcase",
            (
                "3d66 服装合集总览：每件衣服按 3 位编号排布，统一使用 "
                "Ashikhmin 2000 forward-only 布料材质；用于浏览材质与贴图接入效果，"
                "不代表论文像素级验证。"
            ),
            objects,
        ),
    )


def parse_args() -> argparse.Namespace:
    code_root = Path(__file__).resolve().parents[2]
    default_assets = code_root.parent / "VukanLearnResources"
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--assets-root",
        type=Path,
        default=default_assets,
        help="VukanLearnResources checkout root",
    )
    parser.add_argument(
        "--showcase-root",
        type=Path,
        default=None,
        help="Maps/SC_cloth_showcase directory",
    )
    parser.add_argument(
        "--default-closure",
        choices=CLOSURES,
        default="satin",
        help="closure used when closure_map.json has no material override",
    )
    parser.add_argument(
        "--closure-map",
        type=Path,
        default=None,
        help='JSON object such as {"001:m01": "velvet"}',
    )
    parser.add_argument(
        "--flip-normal-green",
        action="store_true",
        help="invert normal-map green when the 3ds Max source uses DirectX Y-",
    )
    parser.add_argument(
        "--pbr-max-size",
        type=int,
        default=2048,
        help="maximum packed roughness/metallic/AO texture edge",
    )
    parser.add_argument("--columns", type=int, default=6)
    parser.add_argument("--spacing-x", type=float, default=240.0)
    parser.add_argument("--spacing-z", type=float, default=300.0)
    parser.add_argument(
        "--model-scale",
        type=float,
        default=100.0,
        help="scene scale applied to real-world garment FBX meshes (100 = metres to cm)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    assets_root = args.assets_root.resolve()
    showcase_root = (
        args.showcase_root.resolve()
        if args.showcase_root
        else assets_root / "Maps" / "SC_cloth_showcase"
    )
    source_dir = showcase_root / "Source"
    groups_csv = showcase_root / "groups.csv"
    materials_csv = showcase_root / "materials.csv"
    textures_csv = showcase_root / "textures.csv"

    for directory in (
        showcase_root,
        showcase_root / "Meshes",
        showcase_root / "Materials",
        showcase_root / "Textures",
        source_dir / "Models",
        source_dir / "Textures",
    ):
        directory.mkdir(parents=True, exist_ok=True)

    closure_map = load_closure_map(args.closure_map)
    group_rows = read_csv(groups_csv)
    material_rows = read_csv(materials_csv)
    texture_rows = read_csv(textures_csv)

    groups: dict[str, GroupRecord] = {}
    for row in group_rows:
        group_id = f"{int(row['id']):03d}"
        groups[group_id] = GroupRecord(
            group_id=group_id,
            root_name=row["root_name"],
            kind=row["kind"],
            object_count=int(row["object_count"]),
            material_count=int(row["material_count"]),
            fbx_asset_path=row["fbx"],
        )

    for row in material_rows:
        group_id = f"{int(row['id']):03d}"
        if group_id not in groups:
            raise SystemExit(f"materials.csv references unknown group {group_id}")
        material_index = f"{int(row['material_index']):02d}"
        diffuse_color = None
        if row.get("diffuse_r"):
            try:
                diffuse_color = [
                    float(row["diffuse_r"]),
                    float(row["diffuse_g"]),
                    float(row["diffuse_b"]),
                ]
            except ValueError:
                diffuse_color = None
        groups[group_id].materials.append(
            MaterialRecord(
                group_id=group_id,
                material_index=material_index,
                material_name=row["material_name"],
                normalized_name=row["normalized_name"],
                diffuse_color=diffuse_color,
            )
        )

    material_lookup = {}
    for group in groups.values():
        for material in group.materials:
            material_lookup[(material.group_id, material.material_index)] = material

    for row in texture_rows:
        group_id = f"{int(row['id']):03d}"
        material_index = f"{int(row['material_index']):02d}"
        material = material_lookup.get((group_id, material_index))
        if material is None:
            raise SystemExit(
                f"textures.csv references unknown material {group_id}:m{material_index}"
            )
        material.textures.append(row)

    ready_groups = []
    skipped = []
    for group in sorted(groups.values(), key=lambda item: item.group_id):
        fbx_path = resolve_fbx(assets_root, group.fbx_asset_path)
        if not fbx_path.is_file():
            skipped.append(group.group_id)
            continue
        if not group.materials:
            group.materials.append(
                MaterialRecord(
                    group_id=group.group_id,
                    material_index="01",
                    material_name="Default",
                    normalized_name="Default",
                )
            )

        material_paths = []
        for material in group.materials:
            closure = closure_for(
                closure_map,
                group.group_id,
                material.material_index,
                args.default_closure,
            )
            material_paths.append(
                build_material_instance(
                    assets_root,
                    showcase_root,
                    showcase_root / "Textures",
                    material,
                    closure,
                    args.flip_normal_green,
                    args.pbr_max_size,
                )
            )
        group.mesh_rel_path = build_mesh_asset(
            assets_root,
            showcase_root,
            group,
            material_paths,
        )
        ready_groups.append(group)

    if not ready_groups:
        print(
            "No FBX files found. Run export_3d66_groups.ms in 3ds Max first.",
            file=sys.stderr,
        )
        return 2

    build_scenes(
        showcase_root,
        ready_groups,
        max(1, args.columns),
        args.spacing_x,
        args.spacing_z,
        args.model_scale,
    )

    manifest = {
        "assetVersion": 1,
        "naming": {
            "mesh": "SM_cloth_<group>.json",
            "materialInstance": "MI_cloth_<group>_m<material>.json",
            "texture": "T_cloth_<group>_m<material>_<role>_<ordinal>.json",
            "singleScene": "SC_cloth_<group>.json",
            "overviewScene": "SC_cloth_showcase.json",
        },
        "groups": [
            {
                "id": group.group_id,
                "sourceGroup": group.root_name,
                "kind": group.kind,
                "objectCount": group.object_count,
                "fbx": group.fbx_asset_path,
                "mesh": group.mesh_rel_path,
                "materials": [
                    {
                        "index": material.material_index,
                        "sourceName": material.material_name,
                        "materialInstance": (
                            f"Maps/SC_cloth_showcase/Materials/"
                            f"MI_cloth_{group.group_id}_m{material.material_index}.json"
                        ),
                        "textures": material.generated_textures,
                    }
                    for material in group.materials
                ],
            }
            for group in ready_groups
        ],
    }
    write_json(showcase_root / "showcase_manifest.json", manifest)

    print(f"generated {len(ready_groups)} clothing groups")
    if skipped:
        print(f"skipped missing FBX groups: {', '.join(skipped)}")
    print(showcase_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
