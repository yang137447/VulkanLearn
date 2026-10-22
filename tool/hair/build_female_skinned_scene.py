"""Author the real Female skin, explicit GASP retarget profile and review scenes."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil

from import_female_scene import MAP, write_json
from calibrate_female_retarget import calibrate_profile


BLUEPRINT_SOURCE = """using VulkanLearn;
namespace TressFx;

public sealed class FemaleSkinnedCharacterBlueprint : Blueprint
{
    [Expose(InstanceEditable = true)]
    public string Clip { get; set; } = "M_Neutral_Walk_Loop_F";

    protected override void BeginPlay()
    {
        // C# 只选择动作；源骨架采样、目标重定向和 GPU 蒙皮仍由原生播放器拥有。
        GetComponent<MeshComponent>("body").Animation.Play(Clip, inPlace: true);
    }
}
"""

SKIN_NOTICE = """# AMD Female_Outside saved skeleton and skin

Source: GPUOpenSoftware/UnrealEngine, TressFX5-5.4.4, revision 61f8dd9b51b88d41937ef8d73618aac778642f41.
Input: Asset/Maya2018/Female_Outside/Female_Outside.zip / Female_Outside.ma.
Source member SHA256: ee1e944273f4b27004e65b2915117f281a9b585ed757f1e0a7f40f27eb71ed65.

Female_Outside_Skinned.fbx uses saved Orig body/eyes geometry and skinCluster
weights with 104 source deformation joints plus the identity FemaleRoot.
All positive source weights are retained; 12 tiny negative residues are
discarded offline and recorded in the extraction audit. No Maya constraints,
blendshapes, tweak graph, Arnold or XGen evaluation is performed.
Body uses UV0, with missing face UVs explicitly filled with (0,0); eyes UV1
becomes runtime UV0. Smooth normals are generated, not recovered Maya normals.
FBX is centimeters/Z-up, converted back through the production VL importer.
Neutral preview materials are authored in VL; external source textures remain
unresolved. Scalp proxy and hair skinning are not exported.

Reproduce with extract_female_skin.py, vl_female_fbx and build_female_skinned_scene.py.
Extraction and FBX roundtrip audits: Generated/Import/SC_tressfx_female/skin-v1/.
GASP Idle/Walk dependencies retain their separate source asset identities;
the retarget mapping and review scene are VL authoring, not the original rig.
The source license below applies to the AMD-derived Female asset.

"""


def build(resource_root, source_resource_root):
    target = resource_root / MAP
    prefix = MAP.as_posix()
    source_sm = json.loads((source_resource_root / "Maps/SC_gasp/Meshes/SM_mannequin.json")
                           .read_text(encoding="utf-8"))
    source_sm["name"] = "GASP Idle and Walk retarget source"
    source_sm["skinning"]["clips"] = source_sm["skinning"]["clips"][:2]
    source_sm["skinning"].pop("montages", None)
    source_sk_path = source_sm["skinning"]["skeleton"]
    source_sk = json.loads((source_resource_root / source_sk_path).read_text(encoding="utf-8"))
    target_sk = json.loads((target / "Skeletons/SK_Female_Outside.json").read_text(encoding="utf-8"))
    if not (target / "Source/Female_Outside_Skinned.fbx").is_file():
        raise ValueError("Run extract_female_skin.py and vl_female_fbx first")

    pairs = [
        ("root", "FemaleRoot"), ("pelvis", "Root_M"),
        ("spine_01", "Spine1_M"), ("spine_03", "Spine2_M"), ("spine_05", "Chest_M"),
        ("neck_01", "Neck_M"), ("neck_02", "Neck1_M"), ("head", "Head_M"),
    ]
    for side in ("L", "R"):
        for source, destination in (
            ("clavicle", "Scapula"), ("upperarm", "Shoulder"), ("lowerarm", "Elbow"),
            ("hand", "Wrist"), ("thigh", "Hip"), ("calf", "Knee"),
            ("foot", "Ankle"), ("ball", "Toes"),
        ):
            pairs.append((f"{source}_{side.lower()}", f"{destination}_{side}"))
    source_names = {joint["name"] for joint in source_sk["joints"]}
    target_names = {joint["name"] for joint in target_sk["joints"]}
    for source, destination in pairs:
        if source not in source_names or destination not in target_names:
            raise ValueError(f"Unknown joint mapping: {source} -> {destination}")

    outputs = {
        "Meshes/SM_gasp_motion_source.json": source_sm,
        "Retarget/RT_Female_From_GASP.json": {
            "format": "vl.retarget.v1", "sourceSkeleton": source_sk["skeleton"],
            "targetSkeleton": target_sk["skeleton"], "sourceRoot": "root", "targetRoot": "FemaleRoot",
            "preview": {"sourceSkeletonPath": source_sk_path,
                        "targetSkeletonPath": f"{prefix}/Skeletons/SK_Female_Outside.json",
                        "targetModelPath": f"{prefix}/Meshes/SM_female_skinned.json"},
            "joints": [{"source": source, "target": destination} for source, destination in pairs],
        },
    }
    profile, calibration_report = calibrate_profile(
        outputs["Retarget/RT_Female_From_GASP.json"], source_sk, target_sk)
    outputs["Retarget/RT_Female_From_GASP.json"] = profile
    skin = {"skeleton": f"{prefix}/Skeletons/SK_Female_Outside.json", "clips": []}
    model = {
        "name": "Female Outside real Maya skin", "type": "mesh",
        "modelDataPath": f"{prefix}/Source/Female_Outside_Skinned.fbx", "skinning": skin,
        "materialSlots": [{"name": slot, "materialInstancePath":
                           f"{prefix}/Materials/MI_female_skinned_{slot}.json"} for slot in ("body", "eyes")],
    }
    outputs["Meshes/SM_female_bind.json"] = copy.deepcopy(model)
    model["skinning"] = {**skin, "sourceModel": f"{prefix}/Meshes/SM_gasp_motion_source.json",
                         "retarget": f"{prefix}/Retarget/RT_Female_From_GASP.json"}
    outputs["Meshes/SM_female_skinned.json"] = model
    for slot, color in (("body", [0.42, 0.45, 0.48, 1]), ("eyes", [0.1, 0.11, 0.12, 1])):
        outputs[f"Materials/MI_female_skinned_{slot}.json"] = {
            "name": f"Female skinned {slot} neutral preview", "type": "materialInstance",
            "material": "shader/glsl/M_pbr.json", "macros": {"VL_SKINNED_MESH": 1},
            "parameters": {"u_tintColor": color, "u_pbrFactors": [0.7, 0, 1, 0]},
        }
    outputs["Blueprints/BP_female_skinned_character.json"] = {
        "type": "blueprint", "name": "Female Outside skinned GASP character",
        "class": "TressFx.FemaleSkinnedCharacterBlueprint", "properties": {},
        "description": "Maya 原骨架和完整权重的 body/eyes；Play 后原地播放 GASP Walk。"
                       "无头发蒙皮、IK、物理角色控制或 Motion Matching。",
        "components": [{"id": "body", "type": "mesh",
                        "modelPath": f"{prefix}/Meshes/SM_female_skinned.json"}],
    }
    # 全身审阅采用明确作者的厘米相机/灯光；不把中性 PBR 预览称为源材质复现。
    studio = [
        {"name": "FullBodyCamera", "type": "camera", "position": [250, 120, 420],
         "rotation": [0, 0, 0], "scale": [1, 1, 1],
         "look_at": [0, 90, 0], "fov": 38, "near_clip": 0.5, "far_clip": 2000,
         "exposure": {"mode": "ev100", "ev100": 0, "aperture": 4,
                      "shutterSeconds": 0.008, "iso": 100, "compensation": 0}},
        {"name": "StudioKey", "type": "directionalLight", "position": [0, 200, 100],
         "rotation": [-35, -30, 0], "color": [1, 0.96, 0.92], "intensity": 3,
         "intensityUnit": "lux", "shadow": {"dynamicShadowDistance": 500}},
        {"name": "NeutralStudio", "type": "environment", "environment": {
            "type": "hdri", "hdrPath": "Common/Environments/UE_AssetsPreview/EpicQuadPanorama_Gray.hdr",
            "cubeSize": 256, "intensity": 1}},
    ]
    for suffix, mesh in (("skinned", "skinned"), ("bind", "bind"), ("skinned_blueprint", None)):
        character = (
            {"name": "FemaleCharacter", "type": "blueprintInstance", "id": "FemaleSkinnedCharacter",
             "blueprintPath": f"{prefix}/Blueprints/BP_female_skinned_character.json", "properties": {}}
            if mesh is None else
            {"name": "FemaleCharacter", "type": "mesh", "modelPath": f"{prefix}/Meshes/SM_female_{mesh}.json"}
        )
        character.update({"position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]})
        outputs[f"SC_tressfx_female_{suffix}.json"] = {
            "name": f"SC_tressfx_female_{suffix}", "type": "scene", "worldUnit": "cm",
            "description": "Female_Outside 原始 Maya 绑定姿态、105 关节、body/eyes 完整权重审阅。"
                           + ("仅显示绑定姿态。" if suffix == "bind" else
                              "从 GASP 源骨架运行时重定向 Idle/Walk；蓝图 Play 原地走路。")
                           + "未映射手指保持绑定姿态，无发丝蒙皮、IK 或 Motion Matching，"
                             "皮肤与眼球为中性材质；学习与资产验证，不是论文光照或 UE 成片验收。",
            "objects": [character, *copy.deepcopy(studio)],
        }

    dependencies = {source_sm["modelDataPath"], source_sk_path}
    for clip in source_sm["skinning"]["clips"]:
        dependencies.add(clip["path"])
        if "sequence" in clip:
            dependencies.add(clip["sequence"])
    dependencies.update(slot["materialInstancePath"] for slot in source_sm["materialSlots"])
    preview_dependencies = {
        "Maps/SC_model_preview/SC_model_preview.json",
        "Maps/SC_model_preview/Meshes/SM_neutral_backdrop.json",
        "Maps/SC_model_preview/Materials/MI_neutral_backdrop.json",
        "Maps/SC_model_preview/Source/quad.obj",
    }
    source_file = target / "Blueprints/FemaleSkinnedCharacterBlueprint.blueprint.cs"
    notice_file = target / "Source/Female_Outside_Skinned.ATTRIBUTION.md"
    source_notice = (target / "Source/ATTRIBUTION.md").read_text(encoding="utf-8")
    license_marker = "## Source License\n"
    if license_marker not in source_notice:
        raise ValueError("Missing AMD license in static Female attribution")
    notice = SKIN_NOTICE + license_marker + source_notice.split(license_marker, 1)[1]
    destinations = [target / name for name in outputs] + [source_file, notice_file]
    if any(path.exists() for path in destinations):
        raise ValueError("Skinned package output already exists; refusing to overwrite author content")
    copies = []
    for name in sorted(dependencies | preview_dependencies):
        original, destination = source_resource_root / name, resource_root / name
        if not original.is_file():
            raise ValueError(f"Missing preview package dependency: {original}")
        if destination.exists():
            if destination.read_bytes() != original.read_bytes():
                raise ValueError(f"Conflicting preview package dependency: {destination}")
        else:
            copies.append((original, destination))
    for original, destination in copies:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, destination)
    for name, value in outputs.items():
        write_json(target / name, value)
    source_file.write_text(BLUEPRINT_SOURCE, encoding="utf-8")
    notice_file.write_text(notice, encoding="utf-8")
    report = {"mappedJoints": len(pairs), "calibration": calibration_report, "outputs": {
        path.relative_to(resource_root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in destinations}, "gaspDependencies": sorted(dependencies),
        "previewDependencies": sorted(preview_dependencies)}
    write_json(resource_root / "Generated/Import/SC_tressfx_female/skin-v1/package-report.json", report)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--source-resource-root", type=Path, required=True)
    args = parser.parse_args()
    build(args.resource_root.resolve(), args.source_resource_root.resolve())
