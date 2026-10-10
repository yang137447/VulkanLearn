"""Export the UE lookdev calibration assets through an installed Unreal Editor.

This script is executed by UnrealEditor with -ExecutePythonScript. It deliberately
keeps the export list small: the source project is a UE 4.26 asset preview, while
the runtime scene only needs the calibration board and its reference objects.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import unreal


ASSET_ROOT = "/Game/TemplateLevels/Assets"
ASSETS = (
    "SM_ColorCalibrator",
    "Chrom_White",
    "Gray_NoSpec",
    "GrayPlate",
    "Color_checker_8bit",
    "Gradient",
    "EnviroDome",
    "EpicQuadPanorama_Gray",
)
ASSET_PATHS = {
    "EpicQuadPanorama_Gray": f"{ASSET_ROOT}/HDRI/EpicQuadPanorama_Gray",
}


def log(message: str) -> None:
    unreal.log("[VL Lookdev Export] " + message)


def asset_bounds(asset) -> dict | None:
    if not isinstance(asset, unreal.StaticMesh):
        return None
    bounds = asset.get_bounds()
    origin = bounds.origin
    extent = bounds.box_extent
    return {
        "origin": [float(origin.x), float(origin.y), float(origin.z)],
        "boxExtent": [float(extent.x), float(extent.y), float(extent.z)],
    }


def asset_materials(asset) -> list[dict]:
    if not isinstance(asset, unreal.StaticMesh):
        return []
    materials = []
    material_count = len(asset.get_editor_property("static_materials"))
    for index in range(material_count):
        material = asset.get_material(index)
        materials.append({
            "index": index,
            "name": material.get_name() if material else "",
            "path": material.get_path_name() if material else "",
            "class": material.get_class().get_name() if material else "",
            "sectionCountLod0": asset.get_num_sections(0),
        })
    return materials


def export_asset(asset, output_path: Path) -> dict:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = str(output_path)
    task.automated = True
    task.replace_identical = True
    task.prompt = False
    task.selected = False

    if output_path.suffix.lower() == ".fbx":
        task.exporter = unreal.StaticMeshExporterFBX()
        export_options = unreal.FbxExportOption()
        # UE 的 ColorCalibrator 将色卡颜色存放在顶点色中；VL 的 M_pbr
        # 会把该通道乘到 BaseColor，文章场景必须保留这部分 authored data。
        export_options.set_editor_property("vertex_color", True)
        task.options = export_options
    elif output_path.suffix.lower() == ".hdr" and hasattr(unreal, "TextureCubeExporterHDR"):
        task.exporter = unreal.TextureCubeExporterHDR()
    elif output_path.suffix.lower() == ".png" and hasattr(unreal, "TextureExporterPNG"):
        task.exporter = unreal.TextureExporterPNG()

    exported_ok = unreal.Exporter.run_asset_export_task(task)
    exported = output_path.exists() and output_path.stat().st_size > 0
    return {
        "path": str(output_path),
        "format": output_path.suffix.lower().lstrip("."),
        "exported": exported and bool(exported_ok),
        "size": output_path.stat().st_size if exported else 0,
        "taskErrors": list(getattr(task, "errors", [])),
    }


def main() -> None:
    output_root = Path(os.environ.get("VL_UE_EXPORT_ROOT", "Generated/Import/ue_assets_preview"))
    output_root = output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    manifest = {
        "source": "AssetsPreview UE project",
        "sourceAssetRoot": ASSET_ROOT,
        "editor": unreal.SystemLibrary.get_engine_version(),
        "assets": [],
    }

    for name in ASSETS:
        path = ASSET_PATHS.get(name, f"{ASSET_ROOT}/{name}")
        asset = unreal.load_asset(path)
        if asset is None:
            log(f"missing asset: {path}")
            manifest["assets"].append({"name": name, "loaded": False})
            continue

        record = {
            "name": name,
            "loaded": True,
            "class": asset.get_class().get_name(),
            "bounds": asset_bounds(asset),
            "materials": asset_materials(asset),
            "exports": [],
        }
        suffixes = (".fbx",) if isinstance(asset, unreal.StaticMesh) else ()
        if isinstance(asset, unreal.TextureCube):
            suffixes = (".hdr",)
        elif isinstance(asset, unreal.Texture2D):
            suffixes = (".png",)
        for suffix in suffixes:
            try:
                record["exports"].append(export_asset(asset, output_root / f"{name}{suffix}"))
            except Exception as error:
                log(f"export failed for {name}{suffix}: {error}")
                record["exports"].append({
                    "format": suffix.lstrip("."),
                    "exported": False,
                    "error": str(error),
                })
        manifest["assets"].append(record)
        log(f"loaded {name} ({record['class']})")

    (output_root / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    log(f"wrote manifest: {output_root / 'manifest.json'}")
    # 命令行导出是无交互流程；显式退出编辑器，避免转换脚本留下 UE 进程。
    unreal.SystemLibrary.quit_editor()


main()
