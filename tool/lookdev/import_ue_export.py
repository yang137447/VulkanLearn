"""Stage UE lookdev exports into the VulkanLearn resource root.

The UE project is an input artifact, not a runtime dependency. This importer
copies the editor-exported FBX and reference textures into the scene Source
area, records the UE version/asset manifest, and writes a normal VL mesh
descriptor. Material authoring stays explicit in VL so the imported mesh does
not silently inherit UE's shading model.
"""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCENE = "Maps/SC_lookdev"


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export-root", type=Path, required=True)
    parser.add_argument("--resource-root", type=Path)
    args = parser.parse_args()

    export_root = args.export_root.resolve()
    resource_root = (args.resource_root or Path(
        json.loads((ROOT / "config/config.json").read_text(encoding="utf-8"))["resourcePath"]
    )).resolve()
    manifest_path = export_root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    source_root = resource_root / SCENE / "Source" / "UE_AssetsPreview"
    source_root.mkdir(parents=True, exist_ok=True)
    copied = []
    for name in (
        "SM_ColorCalibrator.fbx",
        "EnviroDome.fbx",
        "Color_checker_8bit.png",
        "Gradient.png",
        "EpicQuadPanorama_Gray.hdr",
    ):
        source = export_root / name
        if not source.is_file():
            continue
        target = source_root / name
        shutil.copy2(source, target)
        copied.append(str(target.relative_to(resource_root)).replace("\\", "/"))

    texture_path = resource_root / SCENE / "Textures" / "T_ue_color_checker_8bit.json"
    write_json(texture_path, {
        "name": "UE Color Checker 8bit source",
        "type": "texture",
        "source": f"{SCENE}/Source/UE_AssetsPreview/Color_checker_8bit.png",
        "colorSpace": "srgb",
        "mipmaps": True,
        "filter": "linear",
        "wrapMode": "clamp",
    })

    material_specs = {
        "Gray_MI": {
            "tint": [0.18, 0.18, 0.18],
            "roughness": 0.55,
            "metallic": 0.0,
            "specular": 0.0,
        },
        "Chrom_White": {
            "tint": [0.95, 0.95, 0.95],
            "roughness": 0.025,
            "metallic": 1.0,
            "specular": 0.5,
        },
        "M_SkinBall": {
            "tint": [0.35, 0.16, 0.10],
            "roughness": 0.42,
            "metallic": 0.0,
            "specular": 0.0,
        },
        "M_ColorGrid": {
            "tint": [1.0, 1.0, 1.0],
            "roughness": 0.75,
            "metallic": 0.0,
            "specular": 0.0,
            "texture": f"{SCENE}/Textures/T_ue_color_checker_8bit.json",
        },
        "M_ClearCoat": {
            "tint": [0.003, 0.003, 0.003],
            "roughness": 0.8,
            "metallic": 0.0,
            "specular": 0.0,
        },
    }
    material_paths = []
    for slot_name, spec in material_specs.items():
        material_path = resource_root / SCENE / "Materials" / f"MI_ue_color_calibrator_{slot_name}.json"
        material = {
            "name": f"UE Color Calibrator - {slot_name}",
            "type": "materialInstance",
            "material": "shader/glsl/M_pbr.json",
            "parameters": {},
        }
        tint = [*spec["tint"], 1.0]
        if tint != [1.0, 1.0, 1.0, 1.0]:
            material["parameters"]["u_tintColor"] = tint
        pbr_factors = [spec["roughness"], spec["metallic"], 1.0, 0.0]
        if pbr_factors != [0.5, 0.0, 1.0, 0.0]:
            material["parameters"]["u_pbrFactors"] = pbr_factors
        if spec["specular"] != 0.5:
            material["parameters"]["u_specular"] = spec["specular"]
        if not material["parameters"]:
            del material["parameters"]
        if "texture" in spec:
            material["macros"] = {}
            material["macros"]["USE_ALBEDO_MAP"] = 1
            material["textures"] = {"albedoMap": spec["texture"]}
        write_json(material_path, material)
        material_paths.append(str(material_path.relative_to(resource_root)).replace("\\", "/"))

    mesh_path = resource_root / SCENE / "Meshes" / "SM_ue_color_calibrator.json"
    write_json(mesh_path, {
        "name": "UE Color Calibrator",
        "type": "mesh",
        "modelDataPath": f"{SCENE}/Source/UE_AssetsPreview/SM_ColorCalibrator.fbx",
        "importOptions": {
            "generateSmoothNormals": True,
            "includedSectionNames": [
                "SM_ColorCalibrator",
            ],
        },
        "materialSlots": [
            {"name": name, "materialInstancePath": path}
            for name, path in zip(
                (
                    "Gray_MI",
                    "Chrom_White",
                    "M_SkinBall",
                    "M_ColorGrid",
                    "M_ClearCoat",
                ),
                material_paths,
            )
        ],
    })

    enviro_material_path = resource_root / SCENE / "Materials" / "MI_ue_enviro_dome.json"
    write_json(enviro_material_path, {
        "name": "UE Environment Dome",
        "type": "materialInstance",
        "material": "shader/glsl/M_pbr.json",
        "parameters": {
            "u_tintColor": [0.18, 0.18, 0.18, 1.0],
            "u_pbrFactors": [0.9, 0.0, 1.0, 0.0],
            "u_specular": 0.0,
        },
    })
    enviro_mesh_path = resource_root / SCENE / "Meshes" / "SM_ue_enviro_dome.json"
    write_json(enviro_mesh_path, {
        "name": "UE Environment Dome",
        "type": "mesh",
        "modelDataPath": f"{SCENE}/Source/UE_AssetsPreview/EnviroDome.fbx",
        "importOptions": {
            "generateSmoothNormals": True,
        },
        "materialSlots": [{
            "name": "Default",
            "materialInstancePath": str(
                enviro_material_path.relative_to(resource_root)
            ).replace("\\", "/"),
        }],
    })

    write_json(resource_root / "Generated" / "Import" / "ue_assets_preview_import.json", {
        "source": "AssetsPreview.zip",
        "editor": manifest.get("editor"),
        "sourceAssetRoot": manifest.get("sourceAssetRoot"),
        "copiedFiles": copied,
        "runtimeMeshes": [
            f"{SCENE}/Meshes/SM_ue_color_calibrator.json",
            f"{SCENE}/Meshes/SM_ue_enviro_dome.json",
        ],
        "materialSlots": [
            "Gray_MI",
            "Chrom_White",
            "M_SkinBall",
            "M_ColorGrid",
            "M_ClearCoat",
        ],
        "limitations": [
            "UE material graphs are rebuilt explicitly as VL material instances; the verified FBX slot order is preserved.",
            "The ColorChecker texture is retained as source evidence; the calibrated VL chart remains the validation reference.",
        ],
    })

    print(f"Imported UE source assets into {source_root}")
    print(f"Mesh descriptor: {mesh_path}")


if __name__ == "__main__":
    main()
