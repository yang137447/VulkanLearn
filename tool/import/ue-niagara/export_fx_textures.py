"""Probe Niagara FX materials and export every referenced Texture2D to PNG.

Run with UnrealEditor-Cmd:
  UnrealEditor-Cmd.exe <effect.uproject> -run=pythonscript -script=<this file>
    -EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities
    -AllowCommandletRendering -RenderOffscreen
    -unattended -nosound -nop4 -nosplash
"""

from __future__ import annotations

import json
from pathlib import Path

import unreal


RESOURCE_ROOT = Path(
    r"D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/niagara-particle-system/"
    r"VukanLearnResources"
)
UE_ROOT = "/Game/NiagaraExamples"
OUTPUT_ROOT = RESOURCE_ROOT / "Common/Source/UE/NiagaraExamples/Textures/FX"
MANIFEST_DIR = RESOURCE_ROOT / "Generated/Import/ue_niagara_assets"
MAX_EXPORT_DIMENSION = 4096
HARD_SKIP_NAMES = {"T_ExplosionWide_EOO", "T_ExplosionWide_Normals"}
HDR_FORMAT_TOKENS = (
    "RGBA16", "RGBA32", "BGRE8", "R16F", "R32F", "RG16", "RG32",
    "RGB16", "RGB32", "G16", "G32",
)


def is_target_texture(ue_path: str) -> bool:
    return (ue_path.startswith(UE_ROOT + "/")
            and "/FX_Fog/" not in ue_path)

MATERIAL_ROOTS = (
    UE_ROOT + "/FX_Explosions",
    UE_ROOT + "/FX_Footstep",
    UE_ROOT + "/FX_Misc",
    UE_ROOT + "/FX_NDC",
    UE_ROOT + "/FX_PickUp",
    UE_ROOT + "/FX_Player",
    UE_ROOT + "/FX_Ribbons",
    UE_ROOT + "/FX_SkeletalMesh",
    UE_ROOT + "/FX_Smoke",
    UE_ROOT + "/FX_Sparks",
    UE_ROOT + "/FX_Weapons",
    UE_ROOT + "/Materials",
    UE_ROOT + "/Gallery/Materials",
)

TEXTURE_ROOTS = (
    UE_ROOT + "/FX_Footstep/Textures",
    UE_ROOT + "/FX_Weapons/MuzzleFlashes/Textures",
    UE_ROOT + "/Textures/Decals",
    UE_ROOT + "/Textures/Sprites",
    UE_ROOT + "/Textures/Noise",
    UE_ROOT + "/Textures/Utils",
)

EXTRA_TEXTURE_PATHS = (
    # Footstep / decal masks.
    UE_ROOT + "/FX_Footstep/Textures/T_Footprint_Mask",
    UE_ROOT + "/FX_Footstep/Textures/T_Footprint_Mask2",
    UE_ROOT + "/FX_Footstep/Textures/T_Footprint_Normals",
    UE_ROOT + "/Textures/Decals/BulletHole",
    UE_ROOT + "/Textures/Decals/BulletHole_Normals",
    UE_ROOT + "/Textures/Decals/ExplosionDecalMasks",
    # Muzzle / plasma / wide explosion.
    UE_ROOT + "/FX_Weapons/MuzzleFlashes/Textures/T_MuzzleFlash",
    UE_ROOT + "/FX_Weapons/MuzzleFlashes/Textures/T_MuzzleFlash_Side",
    UE_ROOT + "/FX_Weapons/MuzzleFlashes/Textures/T_MuzzleFlash_Sphere",
    UE_ROOT + "/FX_Weapons/MuzzleFlashes/Textures/T_ThinSmoke_FX",
    UE_ROOT + "/Textures/Sprites/T_Plasma_Wisps",
    UE_ROOT + "/Textures/Sprites/T_ExplosionWide_EOO",
    UE_ROOT + "/Textures/Sprites/T_ExplosionWide_Normals",
    # Sprite normal maps (documented even where the sprite shader cannot use them).
    UE_ROOT + "/Textures/Sprites/T_Explosion_Normals",
    UE_ROOT + "/Textures/Sprites/T_ExplosionBurst_Normals_Loop",
    UE_ROOT + "/Textures/Sprites/T_ExplosionRoil_Normals_Loop",
    UE_ROOT + "/Textures/Sprites/T_FireBall_Normals_Loop",
    UE_ROOT + "/Textures/Sprites/T_FireRoil_Normals_Loop",
    UE_ROOT + "/Textures/Sprites/T_SmokePuff_Normals_Loop",
    UE_ROOT + "/Textures/Sprites/T_SmokePuffLight_Normals_Loop",
    UE_ROOT + "/Textures/Sprites/T_SmokeRoil_Normals_Loop",
    UE_ROOT + "/Textures/Sprites/T_SmokeTrail_Normals_Loop",
    UE_ROOT + "/Textures/Sprites/T_Smoke_Wispy_Normals",
    # Debris / surface textures used by impact and explosion mesh particles.
    UE_ROOT + "/StaticMesh/Broken_Concrete_Slab_tmclbcpiw/T_Broken_Concrete_Slab_tmclbcpiw_1K_D",
    UE_ROOT + "/StaticMesh/Broken_Concrete_Slab_tmclbcpiw/T_Broken_Concrete_Slab_tmclbcpiw_1K_N",
    UE_ROOT + "/StaticMesh/Broken_Concrete_Slab_tmclbcpiw/T_BrokenConcreteSlab_tmclbcpiw_1K_DpR",
    UE_ROOT + "/StaticMesh/Broken_Concrete_Slab_tmjgderjw/T_Broken_Concrete_Slab_tmjgderjw_1K_D",
    UE_ROOT + "/StaticMesh/Broken_Concrete_Slab_tmjgderjw/T_Broken_Concrete_Slab_tmjgderjw_1K_N",
    UE_ROOT + "/StaticMesh/Broken_Concrete_Slab_tmjgderjw/T_BrokenConcreteSlab_tmjgderjw_1K_DpR",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1iccejw/T_Concrete_Rubble_uc1iccejw_1K_D",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1iccejw/T_Concrete_Rubble_uc1iccejw_1K_N",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1iccejw/T_ConcreteRubble_uc1iccejw_1K_DpR",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1jagbjw/T_Concrete_Rubble_uc1jagbjw_1K_D",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1jagbjw/T_Concrete_Rubble_uc1jagbjw_1K_N",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1jagbjw/T_ConcreteRubble_uc1jagbjw_1K_DpR",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1jagnjw/T_Concrete_Rubble_uc1jagnjw_1K_D",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1jagnjw/T_Concrete_Rubble_uc1jagnjw_1K_N",
    UE_ROOT + "/StaticMesh/Concrete_Rubble_uc1jagnjw/T_ConcreteRubble_uc1jagnjw_1K_DpR",
    UE_ROOT + "/StaticMesh/Rock_shopk/T_Rock_shopk_2K_D",
    UE_ROOT + "/StaticMesh/Rock_shopk/T_Rock_shopk_2K_N",
    UE_ROOT + "/StaticMesh/Rock_shopk/T_Rock_shopk_2K_DpR",
    UE_ROOT + "/StaticMesh/Wood_Debris_vb2mfeliw/T_Wood_Debris_vb2mfeliw_2K_D",
    UE_ROOT + "/StaticMesh/Wood_Debris_vb2mfeliw/T_Wood_Debris_vb2mfeliw_2K_N",
    UE_ROOT + "/StaticMesh/Wood_Debris_vb2mfeliw/T_WoodDebris_vb2mfeliw_2K_DpR",
    UE_ROOT + "/StaticMesh/Tree_Debris_wjtuagls/T_Tree_Debris_wjtuagls_2K_D",
    UE_ROOT + "/StaticMesh/Tree_Debris_wjtuagls/T_Tree_Debris_wjtuagls_2K_N",
    UE_ROOT + "/StaticMesh/Tree_Debris_wjtuagls/T_TreeDebris_wjtuagls_2K_DpR",
    # Target / range surfaces.
    UE_ROOT + "/Gallery/Megascans/Surfaces/Gravel_Ground_xbnefjm/Medium/xbnefjm_tier_2/Textures/T_xbnefjm_2K_B",
    UE_ROOT + "/Gallery/Megascans/Surfaces/Gravel_Ground_xbnefjm/Medium/xbnefjm_tier_2/Textures/T_xbnefjm_2K_N",
    UE_ROOT + "/Gallery/Megascans/Surfaces/Gravel_Ground_xbnefjm/Medium/xbnefjm_tier_2/Textures/T_xbnefjm_2K_ORM",
    UE_ROOT + "/Gallery/Textures/T_MannequinTarget",
)


def log(message: str) -> None:
    unreal.log("[VL FX Texture Export] " + message)


def asset_record(asset) -> dict | None:
    if asset is None:
        return None
    try:
        return {
            "name": asset.get_name(),
            "path": asset.get_path_name(),
            "class": asset.get_class().get_name(),
        }
    except Exception:
        return None


def texture_metadata(texture) -> dict:
    record = {
        "size": None,
        "srgb": None,
        "compression": None,
        "lodGroup": None,
        "sourceFormat": None,
        "sourceValid": None,
    }
    for name in ("blueprint_get_size_x", "get_surface_width"):
        try:
            width = int(getattr(texture, name)())
            for other in ("blueprint_get_size_y", "get_surface_height"):
                try:
                    height = int(getattr(texture, other)())
                except Exception:
                    continue
                record["size"] = [width, height]
                break
            break
        except Exception:
            continue
    for key in ("srgb", "compression_settings", "lod_group"):
        try:
            value = texture.get_editor_property(key)
            record["srgb" if key == "srgb" else
                   "compression" if key == "compression_settings" else "lodGroup"] = str(value)
        except Exception:
            pass
    try:
        source = texture.get_editor_property("source")
        record["sourceFormat"] = str(source.get_editor_property("format"))
        record["sourceValid"] = str(source.get_editor_property("size_x")) != "0"
    except Exception:
        pass
    return record


def export_supported(record: dict) -> tuple[bool, str]:
    if record.get("name") in HARD_SKIP_NAMES:
        return False, "8K source skipped to avoid >4 GiB export"
    source_format = record.get("metadata", {}).get("sourceFormat") or ""
    if any(token in source_format.upper() for token in HDR_FORMAT_TOKENS):
        return False, "HDR source format %s cannot export as PNG" % source_format
    size = record.get("metadata", {}).get("size")
    if not size:
        return True, ""
    width, height = int(size[0]), int(size[1])
    if width > MAX_EXPORT_DIMENSION or height > MAX_EXPORT_DIMENSION:
        return False, "dimension %dx%d exceeds %d" % (
            width, height, MAX_EXPORT_DIMENSION)
    return True, ""


def expressions_of(asset) -> list:
    try:
        return list(unreal.MaterialEditingLibrary.get_material_expressions(asset))
    except Exception:
        return []


def texture_from_expression(expression) -> object | None:
    for key in ("texture", "texture_object", "texture_2d"):
        try:
            value = expression.get_editor_property(key)
            if isinstance(value, unreal.Texture2D):
                return value
        except Exception:
            continue
    return None


def parameter_name(expression) -> str:
    for key in ("parameter_name", "name"):
        try:
            value = expression.get_editor_property(key)
            if value:
                return str(value)
        except Exception:
            continue
    return ""


def collect_textures(asset, textures: dict, depth: int = 0) -> None:
    if asset is None or depth > 6:
        return
    for expression in expressions_of(asset):
        texture = texture_from_expression(expression)
        if texture is not None:
            record = asset_record(texture)
            if record:
                record["metadata"] = texture_metadata(texture)
                record["parameter"] = parameter_name(expression)
                textures.setdefault(record["path"], record)
        try:
            class_name = expression.get_class().get_name()
        except Exception:
            class_name = ""
        if class_name == "MaterialExpressionMaterialFunctionCall":
            try:
                function = expression.get_editor_property("material_function")
            except Exception:
                function = None
            if function is not None:
                collect_textures(function, textures, depth + 1)


def is_material(asset) -> bool:
    try:
        name = asset.get_class().get_name()
    except Exception:
        return False
    return name in ("Material", "MaterialInstanceConstant", "MaterialInstanceDynamic")


def list_assets(root: str) -> list[str]:
    try:
        return list(unreal.EditorAssetLibrary.list_assets(root, recursive=True,
                                                          include_folder=False))
    except Exception:
        return []


def normalize_package(path: str) -> str:
    return path.split(".")[0]


def export_texture(texture, output: Path, exporter) -> dict:
    output.parent.mkdir(parents=True, exist_ok=True)
    task = unreal.AssetExportTask()
    task.object = texture
    task.filename = str(output)
    task.automated = True
    task.replace_identical = True
    task.prompt = False
    task.selected = False
    task.exporter = exporter
    ok = unreal.Exporter.run_asset_export_task(task)
    exists = output.is_file() and output.stat().st_size > 0
    return {
        "path": str(output.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
        "exported": bool(ok and exists),
        "size": output.stat().st_size if exists else 0,
        "errors": list(getattr(task, "errors", [])),
    }


def output_path_for(ue_path: str, name: str, extension: str = ".png") -> Path:
    package = normalize_package(ue_path)
    prefix = UE_ROOT + "/"
    relative = package[len(prefix):] if package.startswith(prefix) else package.lstrip("/")
    return OUTPUT_ROOT / relative / (name + extension)


def main() -> None:
    OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    MANIFEST_DIR.mkdir(parents=True, exist_ok=True)
    override_path = MANIFEST_DIR / "fx_texture_overrides.json"
    overrides = {"exr": [], "skip": []}
    if override_path.is_file():
        try:
            overrides.update(json.loads(override_path.read_text(encoding="utf-8")))
        except Exception:
            pass
    exr_names = set(overrides.get("exr", []))
    skip_names = set(overrides.get("skip", []))

    materials: dict[str, dict] = {}
    textures: dict[str, dict] = {}

    for root in MATERIAL_ROOTS:
        for package in list_assets(root):
            asset = unreal.load_asset(normalize_package(package))
            if asset is None or not is_material(asset):
                continue
            record = asset_record(asset)
            if record is None:
                continue
            record["textures"] = []
            local: dict[str, dict] = {}
            parent = None
            try:
                parent = asset.get_editor_property("parent")
            except Exception:
                parent = asset
            collect_textures(parent, local)
            record["textures"] = sorted(local.values(), key=lambda item: item["path"])
            materials[record["path"]] = record
            textures.update(local)

    for root in TEXTURE_ROOTS:
        for package in list_assets(root):
            asset = unreal.load_asset(normalize_package(package))
            if isinstance(asset, unreal.Texture2D):
                record = asset_record(asset)
                if record:
                    record["metadata"] = texture_metadata(asset)
                    textures.setdefault(record["path"], record)

    for path in EXTRA_TEXTURE_PATHS:
        asset = unreal.load_asset(path)
        if isinstance(asset, unreal.Texture2D):
            record = asset_record(asset)
            if record:
                record["metadata"] = texture_metadata(asset)
                textures.setdefault(record["path"], record)
        else:
            log("missing texture asset: " + path)

    exports = []
    failures = []
    candidates_path = MANIFEST_DIR / "fx_texture_candidates.json"
    candidates_path.write_text(
        json.dumps({
            "materials": sorted(materials.values(), key=lambda item: item["path"]),
            "textures": sorted(textures.values(), key=lambda item: item["path"]),
        }, indent=2, default=str) + "\n",
        encoding="utf-8")
    log("wrote candidates " + str(candidates_path))
    progress_path = MANIFEST_DIR / "fx_texture_progress.txt"
    progress_path.write_text("", encoding="utf-8")
    for ue_path, record in sorted(textures.items()):
        if not is_target_texture(ue_path):
            continue
        if record["name"] in skip_names:
            with progress_path.open("a", encoding="utf-8") as handle:
                handle.write("SKIP %s override\n" % record["name"])
            exports.append({
                "uePath": ue_path,
                "name": record["name"],
                "metadata": record.get("metadata"),
                "export": {"exported": False, "skipped": True,
                           "reason": "override skip"},
            })
            continue
        supported, reason = export_supported(record)
        if not supported:
            log("skip %s: %s" % (record["name"], reason))
            with progress_path.open("a", encoding="utf-8") as handle:
                handle.write("SKIP %s %s\n" % (record["name"], reason))
            exports.append({
                "uePath": ue_path,
                "name": record["name"],
                "metadata": record.get("metadata"),
                "export": {"exported": False, "skipped": True, "reason": reason},
            })
            continue
        asset = unreal.load_asset(normalize_package(ue_path))
        if not isinstance(asset, unreal.Texture2D):
            failures.append({"asset": ue_path, "error": "not a Texture2D"})
            continue
        with progress_path.open("a", encoding="utf-8") as handle:
            handle.write("BEGIN %s %s\n" % (record["name"], ue_path))
        use_exr = record["name"] in exr_names
        exporter = (unreal.TextureExporterEXR() if use_exr
                    else unreal.TextureExporterPNG())
        extension = ".exr" if use_exr else ".png"
        result = export_texture(
            asset, output_path_for(ue_path, record["name"], extension), exporter)
        result["format"] = "exr" if use_exr else "png"
        with progress_path.open("a", encoding="utf-8") as handle:
            handle.write("DONE %s %s\n" % (record["name"], result["exported"]))
        exports.append({"uePath": ue_path, "name": record["name"], "export": result})
        if not result["exported"]:
            failures.append({"asset": ue_path, "error": "export failed",
                             "errors": result["errors"]})

    manifest = {
        "engineVersion": unreal.SystemLibrary.get_engine_version(),
        "project": str(Path(unreal.Paths.project_dir()).resolve()).replace("\\", "/"),
        "materials": sorted(materials.values(), key=lambda item: item["path"]),
        "textures": sorted(textures.values(), key=lambda item: item["path"]),
        "exports": exports,
        "failures": failures,
    }
    manifest_path = MANIFEST_DIR / "fx_texture_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, default=str) + "\n",
                             encoding="utf-8")
    log("wrote " + str(manifest_path))
    log("materials=%d textures=%d exports=%d failures=%d" % (
        len(materials), len(textures), len(exports), len(failures)))
    unreal.SystemLibrary.quit_editor()


main()
