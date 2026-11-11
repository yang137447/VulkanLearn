"""Build VL texture/material/mesh descriptors for the exported UE assets."""

from __future__ import annotations

import json
from pathlib import Path

from PIL import Image


RESOURCE_ROOT = Path(
    r"D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/niagara-particle-system/"
    r"VukanLearnResources"
)
TEXTURE_SOURCE = RESOURCE_ROOT / "Common/Source/UE/NiagaraExamples/Textures"
TEXTURE_ROOT = RESOURCE_ROOT / "Common/Textures"
MATERIAL_ROOT = RESOURCE_ROOT / "Common/Materials"
MESH_ROOT = RESOURCE_ROOT / "Common/Meshes"


def write_json(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")


def repack_aorm(source_name: str, output_name: str) -> Path:
    source = TEXTURE_SOURCE / source_name
    output = TEXTURE_SOURCE / output_name
    image = Image.open(source).convert("RGB")
    red, green, blue = image.split()
    # UE AORM: R=AO, G=Roughness, B=Metallic.
    # VL M_pbr pbrParamMap: R=Roughness, G=Metallic, B=AO.
    packed = Image.merge("RGB", (green, blue, red))
    packed.save(output)
    return output


def texture_descriptor(name: str, source: str, color_space: str) -> str:
    path = TEXTURE_ROOT / (name + ".json")
    write_json(path, {
        "name": "UE Niagara - " + name,
        "type": "texture",
        "source": source,
        "colorSpace": color_space,
        "mipmaps": True,
        "filter": "linear",
        "wrapMode": "repeat",
    })
    return str(path.relative_to(RESOURCE_ROOT)).replace("\\", "/")


def material_instance(name: str, textures: dict, parameters: dict, skinned: bool) -> str:
    macros = {}
    if skinned:
        macros["VL_SKINNED_MESH"] = 1
    if "albedoMap" in textures:
        macros["USE_ALBEDO_MAP"] = 1
    if "normalMap" in textures:
        macros["USE_NORMAL_MAP"] = 1
    if "pbrParamMap" in textures:
        macros["USE_PBR_MAP"] = 1
    if "emissionMap" in textures:
        macros["USE_EMISSION_MAP"] = 1
    document = {
        "name": name,
        "type": "materialInstance",
        "material": "shader/glsl/M_pbr.json",
    }
    if macros:
        document["macros"] = macros
    if textures:
        document["textures"] = textures
    document["parameters"] = parameters
    path = MATERIAL_ROOT / (name + ".json")
    write_json(path, document)
    return str(path.relative_to(RESOURCE_ROOT)).replace("\\", "/")


def mesh_descriptor(name: str, description: str, model: str, materials: list[dict],
                    skeleton: str | None = None) -> str:
    document = {
        "name": name,
        "type": "mesh",
        "description": description,
        "modelDataPath": model,
        "materialSlots": materials,
    }
    if skeleton:
        document["skinning"] = {"skeleton": skeleton, "clips": []}
    path = MESH_ROOT / (name + ".json")
    write_json(path, document)
    return str(path.relative_to(RESOURCE_ROOT)).replace("\\", "/")


def main() -> None:
    rifle_param_png = repack_aorm("T_Rifle_AORM.png", "T_ue_rifle_vlparam.png")

    textures = {
        "dinoAlbedo": texture_descriptor(
            "T_ue_dino_albedo", "Common/Source/UE/NiagaraExamples/Textures/DINO_A.png",
            "srgb"),
        "dinoNormal": texture_descriptor(
            "T_ue_dino_normal", "Common/Source/UE/NiagaraExamples/Textures/Dino_N.png",
            "linear"),
        "eyesAlbedo": texture_descriptor(
            "T_ue_dino_eyes", "Common/Source/UE/NiagaraExamples/Textures/Eyeball.png",
            "srgb"),
        "mannyAlbedo": texture_descriptor(
            "T_ue_manny_albedo",
            "Common/Source/UE/NiagaraExamples/Textures/T_Manny_01_D.png", "srgb"),
        "mannyNormal": texture_descriptor(
            "T_ue_manny_normal",
            "Common/Source/UE/NiagaraExamples/Textures/T_Manny_01_BN.png", "linear"),
        "toyCowNormal": texture_descriptor(
            "T_ue_toycow_normal",
            "Common/Source/UE/NiagaraExamples/Textures/T_Toy_Cow_N.png", "linear"),
        "rifleAlbedo": texture_descriptor(
            "T_ue_rifle_albedo",
            "Common/Source/UE/NiagaraExamples/Textures/T_Rifle_D.png", "srgb"),
        "rifleNormal": texture_descriptor(
            "T_ue_rifle_normal",
            "Common/Source/UE/NiagaraExamples/Textures/T_Rifle_Combined_N.png", "linear"),
        "riflePbr": texture_descriptor(
            "T_ue_rifle_pbr",
            str(rifle_param_png.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
            "linear"),
    }

    materials = {
        "dino": material_instance(
            "MI_ue_dino", {
                "albedoMap": textures["dinoAlbedo"],
                "normalMap": textures["dinoNormal"],
            }, {
                "u_pbrFactors": [0.58, 0.0, 1.0, 0.0],
                "u_specular": 0.35,
            }, True),
        "dinoEye": material_instance(
            "MI_ue_dino_eye", {"albedoMap": textures["eyesAlbedo"]}, {
                "u_tintColor": [1.0, 0.92, 0.72, 1.0],
                "u_pbrFactors": [0.35, 0.0, 1.0, 0.0],
            }, True),
        "mannyHead": material_instance(
            "MI_ue_manny_head", {
                "albedoMap": textures["mannyAlbedo"],
                "normalMap": textures["mannyNormal"],
            }, {
                "u_pbrFactors": [0.58, 0.0, 1.0, 0.0],
                "u_specular": 0.35,
            }, True),
        "mannyTorso": material_instance(
            "MI_ue_manny_torso", {
                "albedoMap": textures["mannyAlbedo"],
                "normalMap": textures["mannyNormal"],
            }, {
                "u_tintColor": [0.88, 0.92, 0.98, 1.0],
                "u_pbrFactors": [0.62, 0.0, 1.0, 0.0],
                "u_specular": 0.35,
            }, True),
        "quinnHead": material_instance(
            "MI_ue_quinn_head", {
                "albedoMap": textures["mannyAlbedo"],
                "normalMap": textures["mannyNormal"],
            }, {
                "u_tintColor": [0.82, 0.90, 1.0, 1.0],
                "u_pbrFactors": [0.58, 0.0, 1.0, 0.0],
                "u_specular": 0.35,
            }, True),
        "quinnTorso": material_instance(
            "MI_ue_quinn_torso", {
                "albedoMap": textures["mannyAlbedo"],
                "normalMap": textures["mannyNormal"],
            }, {
                "u_tintColor": [0.72, 0.84, 1.0, 1.0],
                "u_pbrFactors": [0.62, 0.0, 1.0, 0.0],
                "u_specular": 0.35,
            }, True),
        "toyCow": material_instance(
            "MI_ue_toycow", {"normalMap": textures["toyCowNormal"]}, {
                "u_tintColor": [0.82, 0.56, 0.30, 1.0],
                "u_pbrFactors": [0.72, 0.0, 1.0, 0.0],
                "u_specular": 0.3,
            }, False),
        "rifle": material_instance(
            "MI_ue_rifle", {
                "albedoMap": textures["rifleAlbedo"],
                "normalMap": textures["rifleNormal"],
                "pbrParamMap": textures["riflePbr"],
            }, {
                "u_pbrFactors": [0.52, 0.0, 1.0, 0.0],
            }, False),
    }

    dino = mesh_descriptor(
        "SM_ue_dino",
        "UE 5.8 NiagaraExamples DinoDragon 骨骼网格；绑定姿态，使用 FBX 导出的 DINO/EYES 材质槽近似。",
        "Common/Source/UE/NiagaraExamples/SkeletalMesh/DinoDragon/SK_DinoDragon.fbx",
        [
            {"name": "DINO", "materialInstancePath": materials["dino"]},
            {"name": "EYES", "materialInstancePath": materials["dinoEye"]},
        ],
        "Common/Skeletons/SKM_DinoDragon.json")
    dino_particles = mesh_descriptor(
        "SM_ue_dino_particles",
        "UE SK_DinoDragon 的 UE 自动生成 LOD1，41,108 三角面；供骨骼粒子源使用。",
        "Common/Source/UE/NiagaraExamples/SkeletalMesh/DinoDragon/SK_DinoDragon_LOD1.fbx",
        [
            {"name": "DINO", "materialInstancePath": materials["dino"]},
            {"name": "EYES", "materialInstancePath": materials["dinoEye"]},
        ],
        "Common/Skeletons/SKM_DinoDragon.json")
    manny = mesh_descriptor(
        "SM_ue_manny",
        "UE 5.8 NiagaraExamples Manny 骨骼网格；绑定姿态，使用 Manny 01 贴图与两个材质槽。",
        "Common/Source/UE/NiagaraExamples/SkeletalMesh/Mannequins/SKM_Manny_Simple.fbx",
        [
            {"name": "M_HeadLegs", "materialInstancePath": materials["mannyHead"]},
            {"name": "M_Torso", "materialInstancePath": materials["mannyTorso"]},
        ],
        "Common/Skeletons/SKM_Manny_Simple.json")
    manny_particles = mesh_descriptor(
        "SM_ue_manny_particles",
        "UE SKM_Manny_Simple 的 UE 自动生成 LOD1，26,998 三角面；供骨骼粒子源使用。",
        "Common/Source/UE/NiagaraExamples/SkeletalMesh/Mannequins/SKM_Manny_Simple_LOD1.fbx",
        [
            {"name": "M_HeadLegs", "materialInstancePath": materials["mannyHead"]},
            {"name": "M_Torso", "materialInstancePath": materials["mannyTorso"]},
        ],
        "Common/Skeletons/SKM_Manny_Simple.json")
    quinn = mesh_descriptor(
        "SM_ue_quinn",
        "UE 5.8 NiagaraExamples Quinn 骨骼网格；绑定姿态，使用 Manny 01 贴图与冷色 tint 近似。",
        "Common/Source/UE/NiagaraExamples/SkeletalMesh/Mannequins/SKM_Quinn_Simple.fbx",
        [
            {"name": "Quinn_01", "materialInstancePath": materials["quinnHead"]},
            {"name": "Quinn_02", "materialInstancePath": materials["quinnTorso"]},
        ],
        "Common/Skeletons/SKM_Quinn_Simple.json")
    toy_cow = mesh_descriptor(
        "SM_ue_toycow",
        "UE 5.8 NiagaraExamples ToyCow 静态网格；碰撞/溶解演示使用，材质为暖色近似。",
        "Common/Source/UE/NiagaraExamples/StaticMesh/ToyCow/SM_ToyCow.fbx",
        [{"name": "M_Toy_Cow", "materialInstancePath": materials["toyCow"]}])
    rifle = mesh_descriptor(
        "SM_ue_rifle",
        "UE 5.8 NiagaraExamples SM_Rifle 静态网格；使用 Rifle D/N/AORM 贴图。",
        "Common/Source/UE/NiagaraExamples/StaticMesh/Rifle/SM_Rifle.fbx",
        [{"name": "lambert2", "materialInstancePath": materials["rifle"]}])

    print(json.dumps({
        "textures": textures,
        "materials": materials,
        "meshes": {"dino": dino, "dinoParticles": dino_particles,
                   "manny": manny, "mannyParticles": manny_particles, "quinn": quinn,
                   "toyCow": toy_cow, "rifle": rifle},
    }, indent=2))


if __name__ == "__main__":
    main()
