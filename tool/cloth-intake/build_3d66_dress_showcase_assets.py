#!/usr/bin/env python3
"""Build the runtime scene for the selected 3d66 dress outfit."""

from __future__ import annotations

import argparse
import csv
import json
import shutil
from pathlib import Path


MATERIAL_ORDER = [
    "Material #118",
    "Material #117",
    "Material #116",
    "Material #119",
    "Material #120",
    "Material #122",
    "Material #121",
]

TEXTURES = [
    {
        "material": "Material #119",
        "role": "albedo",
        "source": "3d66Model-23736651-files-231.jpg",
        "normalized": "T_clothDress_m04_albedo_01.jpg",
        "colorSpace": "srgb",
    },
    {
        "material": "Material #119",
        "role": "normal",
        "source": "3d66Model-23736651-files-089.jpg",
        "normalized": "T_clothDress_m04_normal_01.jpg",
        "colorSpace": "linear",
    },
    {
        "material": "Material #121",
        "role": "albedo",
        "source": "3d66Model-23736651-files-204.jpg",
        "normalized": "T_clothDress_m07_albedo_01.jpg",
        "colorSpace": "srgb",
    },
]


def srgb_to_linear(value: float) -> float:
    value = max(0.0, min(1.0, value))
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def read_materials(path: Path):
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return {row["material_name"]: row for row in csv.DictReader(handle)}


def main() -> int:
    code_root = Path(__file__).resolve().parents[2]
    assets_root = code_root.parent / "VukanLearnResources"
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets-root", type=Path, default=assets_root)
    parser.add_argument(
        "--texture-dir",
        type=Path,
        default=code_root / "artifacts" / "incoming" / "2_export" / "2",
    )
    parser.add_argument(
        "--source-materials",
        type=Path,
        default=code_root / "artifacts" / "incoming" / "fbx_index" / "materials.csv",
    )
    args = parser.parse_args()

    assets_root = args.assets_root.resolve()
    showcase = assets_root / "Maps" / "SC_cloth_showcase"
    textures = showcase / "Textures"
    source_textures = showcase / "Source" / "Textures"
    materials = showcase / "Materials"
    meshes = showcase / "Meshes"
    for directory in (textures, source_textures, materials, meshes):
        directory.mkdir(parents=True, exist_ok=True)

    source_material_rows = read_materials(args.source_materials)

    texture_refs: dict[str, dict[str, str]] = {}
    for texture in TEXTURES:
        source = args.texture_dir / texture["source"]
        if not source.is_file():
            raise SystemExit(f"missing source texture: {source}")
        target_source = source_textures / texture["normalized"]
        shutil.copyfile(source, target_source)
        asset_source = (
            Path("Maps") / "SC_cloth_showcase" / "Source" / "Textures" / texture["normalized"]
        ).as_posix()
        texture_name = Path(texture["normalized"]).stem
        payload = {
            "name": texture_name,
            "type": "texture",
            "source": asset_source,
            "colorSpace": texture["colorSpace"],
            "mipmaps": True,
            "filter": "linear",
            "wrapMode": "repeat",
        }
        if texture["role"] == "normal":
            payload["channelsDescription"] = {
                "r": "tangent x",
                "g": "tangent y",
                "b": "tangent z",
                "a": "unused",
            }
        write_json(textures / f"{texture_name}.json", payload)
        texture_refs[(texture["material"], texture["role"])] = (
            f"Maps/SC_cloth_showcase/Textures/{texture_name}.json"
        )

    for index, material_name in enumerate(MATERIAL_ORDER, start=1):
        runtime_tag = f"{index:02d}"
        source_material = source_material_rows.get(material_name)
        if source_material is None:
            raise SystemExit(f"missing source material: {material_name}")
        material_path = materials / f"MI_cloth_3d66Dress_m{runtime_tag}.json"
        is_cloth = material_name in {"Material #119", "Material #121"}
        texture_map = {
            role: texture_refs[(material_name, role)]
            for role in ("albedo", "normal")
            if (material_name, role) in texture_refs
        }
        macros = {}
        if "albedo" in texture_map:
            macros["USE_ALBEDO_MAP"] = 1
        if "normal" in texture_map:
            macros["USE_NORMAL_MAP"] = 1

        if is_cloth:
            payload = {
                "name": f"MI_cloth_3d66Dress_m{runtime_tag}",
                "type": "materialInstance",
                "material": "shader/glsl/M_clothAshikhminSatin.json",
                "macros": macros,
                "textures": {
                    "clothLut": "Common/Textures/T_clothAshikhminLut.json",
                    **{
                        "albedoMap": texture_map["albedo"]
                        for _ in [0]
                        if "albedo" in texture_map
                    },
                    **{
                        "normalMap": texture_map["normal"]
                        for _ in [0]
                        if "normal" in texture_map
                    },
                },
                "parameters": {"u_baseColor": [1.0, 1.0, 1.0, 1.0]},
            }
        else:
            diffuse = [
                float(source_material["diffuse_r"]),
                float(source_material["diffuse_g"]),
                float(source_material["diffuse_b"]),
            ]
            linear = [srgb_to_linear(value) for value in diffuse]
            if material_name in {"Material #116", "Material #120"}:
                linear = [0.35, 0.35, 0.35]
            elif max(linear) < 0.02:
                linear = [0.008, 0.008, 0.008]
            # #120 is the inner slip that shows through the dress opening. The
            # mannequin itself stays grey (#116); the undergarment reads as
            # black fabric so the cream dress has something dark behind it.
            if material_name == "Material #120":
                linear = [0.03, 0.03, 0.035]
            payload = {
                "name": f"MI_cloth_3d66Dress_m{runtime_tag}",
                "type": "materialInstance",
                "material": (
                    "shader/glsl/M_pbrDoubleSided.json"
                    if material_name in {"Material #116", "Material #120"}
                    else "shader/glsl/M_pbr.json"
                ),
                "parameters": {
                    "u_tintColor": [*linear, 1.0],
                    "u_pbrFactors": [
                        0.55 if material_name == "Material #120" else 0.65,
                        0.0,
                        1.0,
                        0.0,
                    ],
                    "u_specular": 0.45 if material_name == "Material #120" else 0.35,
                },
            }
        write_json(material_path, payload)

    mesh_payload = {
        "name": "3d66 selected dress outfit",
        "type": "mesh",
        "modelDataPath": (
            "Maps/SC_cloth_showcase/Source/Models/SM_cloth_3d66Dress.fbx"
        ),
        "materialSlots": [
            {
                "name": f"m{index:02d}",
                "materialInstancePath": (
                    f"Maps/SC_cloth_showcase/Materials/"
                    f"MI_cloth_3d66Dress_m{index:02d}.json"
                ),
            }
            for index in range(1, len(MATERIAL_ORDER) + 1)
        ],
    }
    write_json(meshes / "SM_cloth_3d66Dress.json", mesh_payload)

    scene_payload = {
        "name": "3d66 Cloth Dress",
        "type": "scene",
        "description": (
            "3d66 宽檐帽长裙的 VulkanLearn 布料展示：裙子和帽体使用 "
            "Ashikhmin、Premože 与 Shirley 2000 Satin forward-only 闭包，"
            "身体、鞋和配饰使用普通 PBR；用于展示真实服装网格与贴图接入，"
            "不是论文像素级验证场景。"
        ),
        "objects": [
            {
                "name": "3d66Dress",
                "type": "mesh",
                "modelPath": (
                    "Maps/SC_cloth_showcase/Meshes/SM_cloth_3d66Dress.json"
                ),
                "position": [0.0, 0.0, 0.0],
                "rotation": [0, 0, 0],
                "scale": [100.0, 100.0, 100.0],
            },
            {
                "name": "KeyLight",
                "type": "directionalLight",
                "position": [-250.0, 520.0, 520.0],
                "rotation": [-25.0, 175.0, 0.0],
                "color": [1.0, 0.97, 0.92],
                "intensity": 5000.0,
                "intensityUnit": "lux",
                "shadow": {"dynamicShadowDistance": 6000.0},
            },
            {
                "name": "FillLight",
                "type": "directionalLight",
                "position": [420.0, 260.0, 320.0],
                "rotation": [-22.0, 222.0, 0.0],
                "color": [0.72, 0.82, 1.0],
                "intensity": 1200.0,
                "intensityUnit": "lux",
                "shadow": {"dynamicShadowDistance": 6000.0},
            },
            {
                "name": "RimLight",
                "type": "directionalLight",
                "position": [0.0, 360.0, -520.0],
                "rotation": [-52.0, 0.0, 0.0],
                "color": [1.0, 0.9, 0.78],
                "intensity": 2200.0,
                "intensityUnit": "lux",
                "shadow": {"dynamicShadowDistance": 6000.0},
            },
            {
                "name": "FrontFill",
                "type": "pointLight",
                "position": [0.0, 165.0, 260.0],
                "rotation": [0, 0, 0],
                "color": [1.0, 0.98, 0.95],
                "intensity": 4000.0,
                "intensityUnit": "cd",
                "radius": 1500.0,
                "shadow": {"castShadows": False},
            },
            {
                "name": "FrontFillDirectional",
                "type": "directionalLight",
                "position": [0.0, 140.0, 600.0],
                "rotation": [-8.0, 175.0, 0.0],
                "color": [1.0, 0.98, 0.96],
                "intensity": 900.0,
                "intensityUnit": "lux",
                "shadow": {"castShadows": False},
            },
            {
                "name": "Camera",
                "type": "camera",
                "fov": 35.0,
                "near_clip": 1.0,
                "far_clip": 50000.0,
                "position": [0.0, 85.0, 700.0],
                "look_at": [0.0, 85.0, 0.0],
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
            },
            {
                "name": "ClothStudioEnvironment",
                "type": "environment",
                "environment": {
                    "type": "proceduralSky",
                    "cubeSize": 128,
                    "intensity": 0.8,
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
            },
        ],
        "worldUnit": "cm",
    }
    write_json(showcase / "SC_cloth_3d66_dress.json", scene_payload)
    print(f"generated {showcase / 'SC_cloth_3d66_dress.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
