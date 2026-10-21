"""Run in UE's PythonScript commandlet against an isolated GASP project.

VL_GASP_AUDIT_OUTPUT must name a new directory outside the project. This is an
inspection/export tool, not a VL animation importer or an AnimBlueprint runner.
"""

from __future__ import annotations

import collections
import json
import math
import os
from pathlib import Path
import traceback

import unreal


ROOT = "/Game/Characters/UEFN_Mannequin"
ANIMS = ROOT + "/Animations"
MOTION = ANIMS + "/MotionMatchingData"
MESH = ROOT + "/Meshes/SKM_UEFN_Mannequin"
CLIPS = (
    "Idle/M_Neutral_Stand_Idle_Loop",
    "Idle/M_Neutral_Crouch_Idle_Loop",
    "Idle/M_Neutral_Stand_Turn_180_L",
    "Idle/M_Neutral_Stand_Turn_180_R",
    "Walk/M_Neutral_Walk_Loop_F",
    "Walk/M_Neutral_Walk_Start_F_Lfoot",
    "Walk/M_Neutral_Walk_Stop_F_Lfoot",
    "Run/M_Neutral_Run_Loop_F",
    "Sprint/M_Neutral_Sprint_Loop_F",
    "Jump/M_Neutral_Jump_F_Start_Stand_Lfoot",
    "Jump/M_Neutral_Jump_Loop_Fall",
    "Jump/M_Neutral_Jump_F_Land_Stand_Light_Lfoot",
)
REFERENCE_ASSETS = (
    MOTION + "/Schemas/PSS_Default",
    MOTION + "/Schemas/PSS_Idle",
    MOTION + "/Schemas/PSS_Jump",
    MOTION + "/Schemas/PSS_Traversal",
    MOTION + "/Databases/Sparse/PSD_Sparse_Stand_Walk_Loops",
    MOTION + "/Databases/Sparse/PSD_Sparse_Stand_Walk_Starts",
    MOTION + "/Databases/Sparse/PSD_Sparse_Stand_Walk_Stops",
    MOTION + "/CHT_PoseSearchDatabases",
    ANIMS + "/Traversal/CHT_TraversalMontages_CMC",
    "/Game/Blueprints/SandboxCharacter_CMC_ABP",
)


def write_json(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
        stream.write("\n")


def vector(value):
    return [float(value.x), float(value.y), float(value.z)]


def transform(value):
    rotation = value.rotation
    return {
        "translation": vector(value.translation),
        "rotationXyzw": [float(rotation.x), float(rotation.y),
                         float(rotation.z), float(rotation.w)],
        "scale": vector(value.scale3d),
    }


def serialize(value):
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, unreal.Object):
        return {"path": value.get_path_name(), "class": value.get_class().get_name()}
    if isinstance(value, unreal.StructBase):
        return {"ueStructText": value.export_text()}
    if isinstance(value, (unreal.Array, list, tuple)):
        return [serialize(item) for item in value]
    return str(value)


def properties(obj, names):
    result = {}
    for name in names:
        try:
            result[name] = serialize(obj.get_editor_property(name))
        except Exception as error:
            result[name] = {"unavailable": str(error)}
    return result


def export_file(asset, path, exporter, options=None):
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = str(path)
    task.automated = True
    task.replace_identical = False
    task.prompt = False
    task.exporter = exporter
    if options is not None:
        task.options = options
    success = bool(unreal.Exporter.run_asset_export_task(task))
    size = path.stat().st_size if path.exists() else 0
    return {"file": path.name, "success": success and size > 0, "bytes": size,
            "errors": [str(error) for error in task.errors]}


def fbx_options():
    options = unreal.FbxExportOption()
    # 根轨道保持在骨骼内；不额外把 Actor 位移映射到根，也不导出预览网格。
    for name, value in {
        "ascii": False, "force_front_x_axis": False, "vertex_color": True,
        "level_of_detail": False, "export_morph_targets": False,
        "export_preview_mesh": False, "map_skeletal_motion_to_root": False,
    }.items():
        options.set_editor_property(name, value)
    # 材质由 VL 重建，预检显式关闭烘焙。骨骼网格导出自身仍需要 MeshObject，
    # 实测 NullRHI 即使关闭烘焙也断言，须以 AllowCommandletRendering 运行。
    options.set_editor_property("bake_material_inputs", unreal.FbxMaterialBakeMode.DISABLED)
    return options


def evaluated_pose(animation, time, mesh):
    options = unreal.AnimPoseEvaluationOptions()
    options.set_editor_property("evaluation_type", unreal.AnimDataEvalType.RAW)
    options.set_editor_property("should_retarget", False)
    options.set_editor_property("extract_root_motion", False)
    options.set_editor_property("incorporate_root_motion_into_pose", True)
    options.set_editor_property("optional_skeletal_mesh", mesh)
    pose = unreal.AnimPoseExtensions.get_anim_pose_at_time(animation, time, options)
    if not unreal.AnimPoseExtensions.is_valid(pose):
        raise RuntimeError("UE returned an invalid pose")
    return pose


def skeleton_record(mesh, animation):
    pose = evaluated_pose(animation, 0.0, mesh)
    names = unreal.AnimPoseExtensions.get_bone_names(pose)
    bones = []
    for name in names:
        parent = unreal.SkeletalMeshEditorSubsystem.get_bone_parent(mesh, name)
        bones.append({
            "name": str(name), "parent": str(parent),
            "referenceLocal": transform(unreal.AnimPoseExtensions.get_ref_bone_pose(
                pose, name, unreal.AnimPoseSpaces.LOCAL)),
            # UE 的 World 在此 API 中指 component/model 空间，不是关卡世界。
            "referenceModel": transform(unreal.AnimPoseExtensions.get_ref_bone_pose(
                pose, name, unreal.AnimPoseSpaces.WORLD)),
        })
    return {
        "mesh": mesh.get_path_name(),
        "skeleton": serialize(mesh.get_editor_property("skeleton")),
        "units": "UE centimeters; unconverted",
        "bones": bones,
        "materials": serialize(mesh.get_editor_property("materials")),
    }


def clip_record(animation, mesh):
    library = unreal.AnimationLibrary
    duration = float(library.get_sequence_length(animation))
    curves = []
    for name in library.get_animation_curve_names(animation, unreal.RawCurveTrackTypes.RCT_FLOAT):
        times, values = library.get_float_keys(animation, name)
        curves.append({"name": str(name), "times": list(times), "values": list(values)})
    events = []
    # UE 5.8 的 Notifies 是 protected；使用公开的按轨道查询接口，保留轨道身份。
    for track in library.get_animation_notify_track_names(animation):
        for event in library.get_animation_notify_events_for_track(animation, track):
            events.append({
                "track": str(track),
                "time": float(library.get_anim_notify_event_trigger_time(event)),
                "duration": float(library.get_anim_notify_event_duration(event)),
                "fields": serialize(event),
            })
    root_samples = []
    steps = max(1, math.ceil(duration * 60.0))
    for step in range(steps + 1):
        time = duration * step / steps
        root_samples.append({"time": time, "transform": transform(
            library.extract_root_track_transform(animation, time))})
    frames = []
    for ratio in (0.0, 0.25, 0.5, 0.75, 1.0):
        time = duration * ratio
        pose = evaluated_pose(animation, time, mesh)
        bones = []
        for name in unreal.AnimPoseExtensions.get_bone_names(pose):
            bones.append({
                "name": str(name),
                "local": transform(unreal.AnimPoseExtensions.get_bone_pose(
                    pose, name, unreal.AnimPoseSpaces.LOCAL)),
                "model": transform(unreal.AnimPoseExtensions.get_bone_pose(
                    pose, name, unreal.AnimPoseSpaces.WORLD)),
            })
        frames.append({"time": time, "bones": bones})
    return {
        "asset": animation.get_path_name(), "duration": duration,
        "numFrames": library.get_num_frames(animation),
        "numKeys": library.get_num_keys(animation),
        "rateScale": library.get_rate_scale(animation),
        "tracks": [str(name) for name in library.get_animation_track_names(animation)],
        "settings": properties(animation, (
            "skeleton", "enable_root_motion", "root_motion_root_lock",
            "force_root_lock", "additive_anim_type", "retarget_source")),
        "floatCurveKeys": curves,
        "curveLimit": "Time/value keys only; tangent/interpolation migration is not verified",
        "syncMarkers": serialize(library.get_animation_sync_markers(animation)),
        "notifies": events, "rawRootSamples": root_samples,
        "poseEvaluation": "RAW, retarget disabled, root incorporated, selected mesh",
        "referenceFrames": frames,
    }


def channel_record(channel):
    class_name = channel.get_class().get_name()
    if class_name == "PoseSearchFeatureChannel_Group":
        names = ("sample_role", "debug_weight_group_id")
    elif class_name == "PoseSearchFeatureChannel_Trajectory":
        names = ("sample_role", "weight", "samples")
    else:
        names = (
            "bone", "origin_bone", "sample_role", "origin_role", "weight",
            "sample_time_offset", "origin_time_offset", "input_query_pose",
            "component_stripping", "normalization_group", "sampling_attribute_id")
        if class_name == "PoseSearchFeatureChannel_Velocity":
            names += ("use_character_space_velocities", "normalize")
        elif class_name == "PoseSearchFeatureChannel_Position":
            names += ("normalize_displacement",)
        elif class_name == "PoseSearchFeatureChannel_Heading":
            names += ("heading_axis",)
    result = {"object": serialize(channel), "properties": properties(channel, names)}
    if class_name == "PoseSearchFeatureChannel_Group":
        try:
            result["subChannels"] = [channel_record(child) for child in
                                     channel.get_editor_property("SubChannels")]
        except Exception as error:
            result["subChannels"] = {"unavailable": str(error),
                                    "fallback": "See schema UObject text export"}
    elif class_name == "PoseSearchFeatureChannel_Trajectory":
        # 此 transient 数组在本机 UE Python 未暴露；保留作者 Samples 与 UE 文本，
        # 不以空数组冒充“没有通道”，更不把展开后的缓存直接当 VL schema。
        result["generatedSubChannels"] = "Not exposed; see Samples and UObject text export"
    return result


def reference_record(asset, output):
    result = {"asset": serialize(asset)}
    class_name = asset.get_class().get_name()
    if class_name == "PoseSearchSchema":
        result["properties"] = properties(asset, (
            "sample_rate", "skeletons", "data_preprocessor",
            "number_of_permutations", "permutations_sample_rate", "permutations_time_offset"))
        result["channels"] = [channel_record(channel) for channel in
                              asset.get_editor_property("channels")]
    elif class_name == "PoseSearchDatabase":
        result["properties"] = properties(asset, (
            "schema", "database_animation_assets", "continuing_pose_cost_bias",
            "base_cost_bias", "looping_cost_bias", "normalization_set", "tags"))
    # 文本保留 UE 自己的属性表示，供人工核对；它不是 VL 可加载的格式。
    result["textExport"] = export_file(
        asset, output / (asset.get_name() + ".copy"), unreal.ObjectExporterT3D())
    return result


def main():
    project = Path(unreal.Paths.project_dir()).resolve()
    output = Path(os.environ["VL_GASP_AUDIT_OUTPUT"]).resolve()
    if output == project or project in output.parents:
        raise RuntimeError("Audit output must be outside the UE project")
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "format": "vl.gasp.ue-audit.v1",
        "project": str(project), "engine": unreal.SystemLibrary.get_engine_version(),
        "status": "incomplete", "errors": [], "clips": [], "referenceAssets": [],
        "scope": "Asset data only; no AnimBlueprint execution or VL rendering",
        "fbxPolicy": {
            "forceFrontXAxis": False, "mapSkeletalMotionToRoot": False,
            "exportPreviewMesh": False, "coordinateConversionVerified": False,
        },
    }
    try:
        registry = unreal.AssetRegistryHelpers.get_asset_registry()
        registry.search_all_assets(True)
        catalog = []
        for asset in registry.get_assets_by_path("/Game", recursive=True):
            catalog.append({"package": str(asset.package_name), "name": str(asset.asset_name),
                            "class": str(asset.asset_class_path.asset_name)})
        catalog.sort(key=lambda item: item["package"])
        write_json(output / "asset-registry.json", catalog)
        report["assetCount"] = len(catalog)
        report["classCounts"] = dict(sorted(collections.Counter(
            item["class"] for item in catalog).items()))
        mesh = unreal.load_asset(MESH)
        if not isinstance(mesh, unreal.SkeletalMesh):
            raise RuntimeError("Selected mesh is missing or not a SkeletalMesh")
        first = unreal.load_asset(ANIMS + "/" + CLIPS[0])
        skeleton = skeleton_record(mesh, first)
        write_json(output / "skeleton.json", skeleton)
        report["boneCount"] = len(skeleton["bones"])
        report["meshExport"] = export_file(
            mesh, output / (mesh.get_name() + ".fbx"),
            unreal.SkeletalMeshExporterFBX(), fbx_options())
        for relative in CLIPS:
            path = ANIMS + "/" + relative
            try:
                animation = unreal.load_asset(path)
                if not isinstance(animation, unreal.AnimSequence):
                    raise RuntimeError("Selected clip is missing or not an AnimSequence: " + path)
                record = clip_record(animation, mesh)
                record["fbx"] = export_file(
                    animation, output / (animation.get_name() + ".fbx"),
                    unreal.AnimSequenceExporterFBX(), fbx_options())
                # notify 结构里的对象引用不包含对象参数；额外保留 UObject 文本及
                # 内嵌 notify/state，避免只留下名字而丢失动作窗口等配置。
                record["textExport"] = export_file(
                    animation, output / (animation.get_name() + ".copy"),
                    unreal.ObjectExporterT3D())
                write_json(output / (animation.get_name() + ".json"), record)
                report["clips"].append({
                    "asset": record["asset"], "duration": record["duration"],
                    "tracks": len(record["tracks"]), "curves": len(record["floatCurveKeys"]),
                    "notifies": len(record["notifies"]), "fbx": record["fbx"],
                    "textExport": record["textExport"],
                })
                unreal.log("[VL GASP Audit] Exported " + path)
            except Exception:
                error = traceback.format_exc()
                report["errors"].append({"asset": path, "error": error})
                unreal.log_error("[VL GASP Audit] " + path + "\n" + error)
        for path in REFERENCE_ASSETS:
            try:
                asset = unreal.load_asset(path)
                if asset is None:
                    raise RuntimeError("Reference asset not found: " + path)
                report["referenceAssets"].append(reference_record(asset, output))
            except Exception:
                error = traceback.format_exc()
                report["errors"].append({"asset": path, "error": error})
                unreal.log_error("[VL GASP Audit] " + path + "\n" + error)
        if (not report["errors"] and report["meshExport"]["success"]
                and all(clip["fbx"]["success"] for clip in report["clips"])
                and all(clip["textExport"]["success"] for clip in report["clips"])
                and all(item["textExport"]["success"] for item in report["referenceAssets"])):
            report["status"] = "asset-export-complete"
    except Exception:
        report["errors"].append({"error": traceback.format_exc()})
    finally:
        write_json(output / "audit.json", report)
    if report["status"] != "asset-export-complete":
        raise RuntimeError("Audit incomplete; see " + str(output / "audit.json"))
    unreal.log("[VL GASP Audit] Complete: " + str(output))


if __name__ == "__main__":
    main()
