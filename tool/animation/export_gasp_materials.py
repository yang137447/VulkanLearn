"""Read resolved DefaultLevel materials and export their source images/graphs.

Run only against the isolated project copy. Does not save UE assets or packages.
"""
import json
import os
from pathlib import Path
import traceback

import unreal


def color(value):
    return [float(value.r), float(value.g), float(value.b), float(value.a)]


def vector(value):
    return [float(value.x), float(value.y), float(value.z)]


def transform(value):
    q = value.rotation
    return {"position": vector(value.translation), "scale": vector(value.scale3d),
            "quaternion": [float(q.x), float(q.y), float(q.z), float(q.w)]}


def export(asset, filename, exporter=None):
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = str(filename)
    task.automated = True
    task.replace_identical = False
    task.prompt = False
    if exporter is not None:
        task.exporter = exporter
    ok = unreal.Exporter.run_asset_export_task(task)
    if not ok or not filename.is_file() or filename.stat().st_size == 0:
        raise RuntimeError(f"Failed exporting {asset.get_path_name()}: {list(task.errors)}")


def main():
    output = Path(os.environ["VL_GASP_MATERIAL_OUTPUT"]).resolve()
    project = Path(unreal.Paths.project_dir()).resolve()
    if output.is_relative_to(project) or output.exists():
        raise RuntimeError("Choose a new output outside the UE project")
    output.mkdir(parents=True)
    report = {"project": str(project), "level": "/Game/Levels/DefaultLevel",
              "materials": {}, "textures": {}, "graphs": {}, "components": [],
              "actors": [], "errors": []}
    library = unreal.MaterialEditingLibrary
    registry = unreal.AssetRegistryHelpers.get_asset_registry()

    def texture(asset):
        if asset is None or asset.get_path_name() in report["textures"]:
            return
        path = asset.get_path_name()
        record = {"class": asset.get_class().get_name(), "name": asset.get_name()}
        report["textures"][path] = record
        if isinstance(asset, unreal.TextureCube):
            suffix, exporter = ".hdr", unreal.TextureCubeExporterHDR()
        elif isinstance(asset, unreal.Texture2D):
            # Export original source, not the GPU's BC-compressed normal representation.
            suffix, exporter = ".png", unreal.TextureExporterPNG()
        else:
            raise RuntimeError("Unsupported texture class: " + path)
        record["file"] = f"texture_{len(report['textures']) - 1:03d}_{asset.get_name()}{suffix}"
        for prop in ("srgb", "compression_settings", "address_x", "address_y",
                     "filter", "lod_group"):
            try:
                record[prop] = str(asset.get_editor_property(prop))
            except Exception:
                pass
        export(asset, output / record["file"], exporter)

    def graph(asset):
        path = asset.get_path_name()
        if path in report["graphs"]:
            return
        filename = f"graph_{len(report['graphs']):03d}_{asset.get_name()}.copy"
        report["graphs"][path] = filename
        export(asset, output / filename)
        options = unreal.AssetRegistryDependencyOptions(
            include_hard_package_references=True, include_soft_package_references=True)
        for dependency in registry.get_dependencies(path.split(".")[0], options) or []:
            for data in registry.get_assets_by_package_name(dependency):
                if str(data.asset_class_path.asset_name) == "MaterialFunction":
                    graph(data.get_asset())

    def material(asset):
        if asset is None or asset.get_path_name() in report["materials"]:
            return
        path = asset.get_path_name()
        record = {"class": asset.get_class().get_name(), "scalars": {}, "vectors": {},
                  "textures": {}, "switches": {}}
        report["materials"][path] = record
        if isinstance(asset, unreal.MaterialInstance):
            parent = asset.get_editor_property("parent")
            record["parent"] = parent.get_path_name()
            material(parent)
        else:
            graph(asset)
            record["blendMode"] = str(asset.get_editor_property("blend_mode"))
            record["twoSided"] = bool(asset.get_editor_property("two_sided"))
        for kind in ("scalar", "vector", "texture"):
            names = getattr(library, f"get_{kind}_parameter_names")(asset)
            for name in names:
                if isinstance(asset, unreal.MaterialInstanceDynamic):
                    value = getattr(asset, f"get_{kind}_parameter_value")(name)
                elif isinstance(asset, unreal.MaterialInstanceConstant):
                    value = getattr(library, f"get_material_instance_{kind}_parameter_value")(asset, name)
                else:
                    value = getattr(library, f"get_material_default_{kind}_parameter_value")(asset, name)
                if kind == "texture":
                    record["textures"][str(name)] = value.get_path_name() if value else None
                    texture(value)
                elif kind == "vector":
                    record["vectors"][str(name)] = color(value)
                else:
                    record["scalars"][str(name)] = float(value)
        # Includes non-parameter texture samples embedded in the parent graph.
        used = library.get_material_used_textures(asset)
        record["usedTextures"] = [item.get_path_name() for item in used]
        for item in used:
            texture(item)
        for name in library.get_static_switch_parameter_names(asset):
            base = asset
            # MID 不能改静态开关；必须读取最近的 MIC 继承值，不能跳到母材质默认值。
            if isinstance(base, unreal.MaterialInstanceDynamic):
                base = base.get_editor_property("parent")
            if isinstance(base, unreal.MaterialInstanceConstant):
                value = library.get_material_instance_static_switch_parameter_value(base, name)
            else:
                value = library.get_material_default_static_switch_parameter_value(base, name)
            record["switches"][str(name)] = bool(value)

    try:
        levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if not levels.load_level(report["level"]):
            raise RuntimeError("Could not load DefaultLevel")
        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
        for actor in actors:
            record = {"name": actor.get_name(), "class": actor.get_class().get_name(),
                      "transform": transform(actor.get_actor_transform())}
            report["actors"].append(record)
            for component in actor.get_components_by_class(unreal.SkyLightComponent):
                cube = component.get_editor_property("cubemap")
                record["sky"] = {"intensity": component.get_editor_property("intensity"),
                                 "cubemap": cube.get_path_name() if cube else None}
                texture(cube)
            for component in actor.get_components_by_class(unreal.DirectionalLightComponent):
                record.setdefault("lights", []).append({
                    "intensity": component.get_editor_property("intensity"),
                    "color": str(component.get_editor_property("light_color")),
                    "transform": transform(component.get_world_transform())})
            if isinstance(actor, unreal.LandscapeProxy):
                item = actor.get_editor_property("landscape_material")
                material(item)
                record["landscapeMaterial"] = item.get_path_name() if item else None
                # 私有 HeightmapTexture 未暴露给 Python；使用引擎公开导出消除
                # subsection 重复接缝。此关卡地表轴对齐，范围由真实 actor bounds 得到。
                origin, extent = actor.get_actor_bounds(False)
                authored = record["transform"]
                if any(abs(v) > 1e-6 for v in authored["quaternion"][:3]):
                    raise RuntimeError("Landscape bounds export requires axis alignment")
                minimum = [round((getattr(origin, axis) - getattr(extent, axis) -
                                  authored["position"][i]) / authored["scale"][i])
                           for i, axis in enumerate(("x", "y"))]
                dimensions = [round(2 * getattr(extent, axis) / authored["scale"][i]) + 1
                              for i, axis in enumerate(("x", "y"))]
                target = unreal.RenderingLibrary.create_render_target2d(
                    actor, dimensions[0], dimensions[1],
                    unreal.TextureRenderTargetFormat.RTF_RGBA8)
                # 首次 commandlet 加载会异步编译这个编辑器材质；未等完成就绘制
                # 会返回 true 却只读到 clear 黑图。先完成本次所需的编译再回读。
                unreal.load_asset("/Engine/EditorLandscapeResources/Landscape_Heightmap_To_RenderTarget2D")
                unreal.SystemLibrary.execute_console_command(actor, "Editor.AsyncAssetCompilationFinishAll")
                if not actor.landscape_export_heightmap_to_render_target(target, True, True):
                    raise RuntimeError("Landscape render-target export failed")
                filename = f"landscape_{actor.get_name()}.png"
                unreal.RenderingLibrary.export_render_target(actor, target, str(output), filename)
                if not (output / filename).is_file():
                    raise RuntimeError("Landscape heightmap PNG was not written")
                record["landscapeHeightmap"] = {
                    "file": filename, "dimensions": dimensions, "sectionBase": minimum,
                    "componentCount": len(actor.get_components_by_class(unreal.LandscapeComponent)),
                    "boundsOrigin": vector(origin), "boundsExtent": vector(extent),
                    "encoding": "RG8 unsigned height; localZ=(R*256+G-32768)/128"}
            for component in actor.get_components_by_class(unreal.MeshComponent):
                mats = []
                for index in range(component.get_num_materials()):
                    item = component.get_material(index)
                    material(item)
                    mats.append(item.get_path_name() if item else "")
                report["components"].append({"actor": actor.get_name(), "component": component.get_name(),
                    "class": component.get_class().get_name(), "materials": mats,
                    "transform": transform(component.get_world_transform()),
                    "boundsOrigin": vector(unreal.SystemLibrary.get_component_bounds(component)[0])})
            for component in actor.get_components_by_class(unreal.DecalComponent):
                item = component.get_editor_property("decal_material")
                material(item)
                record.setdefault("decals", []).append({"component": component.get_name(),
                    "material": item.get_path_name(), "transform": str(component.get_world_transform()),
                    "placement": transform(component.get_world_transform()),
                    "size": vector(component.get_editor_property("decal_size")),
                    "color": color(component.get_editor_property("decal_color"))})
        # Audit all prototype textures, including alternate source maps not active in this level.
        for path in unreal.EditorAssetLibrary.list_assets("/Game/Levels/LevelPrototyping", recursive=True):
            data = unreal.EditorAssetLibrary.find_asset_data(path)
            cls = str(data.asset_class_path.asset_name)
            if cls in ("Texture2D", "TextureCube"):
                texture(data.get_asset())
            elif cls in ("Material", "MaterialFunction"):
                graph(data.get_asset())
        report["status"] = "complete"
    except Exception:
        report["status"] = "failed"
        report["errors"].append(traceback.format_exc())
    finally:
        (output / "materials.json").write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if report["errors"]:
        raise RuntimeError(report["errors"][-1])
    unreal.log(f"VL GASP materials: {len(report['materials'])}, textures: {len(report['textures'])}")


if __name__ == "__main__":
    main()
