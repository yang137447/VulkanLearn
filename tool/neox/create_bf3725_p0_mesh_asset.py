from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
from typing import Any


# 角色还原完成后，35 个真实槽位必须全部显式映射；缺少任一槽位都应在生成阶段失败。
MIGRATED_MATERIALS = {
    "b_f_3725_1_high": "materials/neox/b_f_3725/MI_body_silk_flow.json",
    "b_f_3725_2_high_0": "materials/neox/b_f_3725/MI_body_silk_flow.json",
    "b_f_3725_2_high_1": "materials/neox/b_f_3725/MI_body_default_secondary.json",
    "b_f_3725_2_high_2": "materials/neox/b_f_3725/MI_body_silk_emissive.json",
    "b_f_3725_3_high_0": "materials/neox/b_f_3725/MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_3_high_1": "materials/neox/b_f_3725/MI_body_default_clip.json",
    "b_f_3725_3_high_2": "materials/neox/b_f_3725/MI_body_silk_emissive.json",
    "b_f_3725_4_high_0": "materials/neox/b_f_3725/MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_4_high_1": "materials/neox/b_f_3725/MI_body_silk_emissive.json",
    "b_f_3725_5_high_0": "materials/neox/b_f_3725/MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_5_high_1": "materials/neox/b_f_3725/MI_body_default_clip.json",
    "b_f_3725_5_high_2": "materials/neox/b_f_3725/MI_body_silk_emissive.json",
    "b_f_3725_6_high_0": "materials/neox/b_f_3725/MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_6_high_1": "materials/neox/b_f_3725/MI_body_default_clip.json",
    "b_f_3725_6_high_2": "materials/neox/b_f_3725/MI_body_silk_emissive.json",
    "b_f_3725_6_high_3": "materials/neox/b_f_3725/MI_crystal_red_clip.json",
    "b_f_3725_high_0": "materials/neox/b_f_3725/MI_b_f_3725_body_p0.json",
    "b_f_3725_high_1": "materials/neox/b_f_3725/MI_b_f_3725_high_1_pearl.json",
    "b_f_3725_high_2": "materials/neox/b_f_3725/MI_body_default_clip.json",
    "b_f_3725_high_3": "materials/neox/b_f_3725/MI_body_silk_emissive.json",
    "b_f_3725_high_4": "materials/neox/b_f_3725/MI_body_silk_plain.json",
    "b_f_3725_high_5": "materials/neox/b_f_3725/MI_body_silk_plain.json",
    "b_f_3725_high_6": "materials/neox/b_f_3725/MI_body_silk_emissive_alt.json",
    "b_f_3725_high_7": "materials/neox/b_f_3725/MI_body_silk_flow.json",
    "b_f_3725_high_8": "materials/neox/b_f_3725/MI_body_default_secondary.json",
    "b_f_3725_high_9": "materials/neox/b_f_3725/MI_crystal_red_clip.json",
    "b_f_3725_high_10": "materials/neox/b_f_3725/MI_crystal_red_opaque.json",
    "b_f_3725_high_11": "materials/neox/b_f_3725/MI_crystal_gold_opaque.json",
    "h_f_3725_high_0": "materials/neox/b_f_3725/MI_hair_cards.json",
    "h_f_3725_high_1": "materials/neox/b_f_3725/MI_hair_pearl.json",
    "h_f_3725_high_2": "materials/neox/b_f_3725/MI_hair_default_sparkle.json",
    "h_f_3725_high_3": "materials/neox/b_f_3725/MI_hair_silk.json",
    "07 - Default": "materials/neox/b_f_3725/MI_eye.json",
    "09 - Default": "materials/neox/b_f_3725/MI_face_skin.json",
    "08 - Default": "materials/neox/b_f_3725/MI_eye_edge.json",
}

HAIR_MODE8_SLOT = "h_f_3725_high_0"
HAIR_MODE8_CORE_MI = "materials/neox/b_f_3725/MI_hair_cards_clip.json"
HAIR_MODE8_SOURCE_GLTF = "models/datas/neox/b_f_3725/b_f_3725.gltf"
HAIR_MODE8_CORE_GLTF = "models/datas/neox/b_f_3725/b_f_3725_hair_core.gltf"
HAIR_MODE8_CORE_MODEL = "models/neox/b_f_3725/SM_b_f_3725_hair_core.json"

def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create the VulkanLearn P0 mesh asset and validation scene from the b_f_3725 glTF audit."
    )
    parser.add_argument(
        "--resource-root",
        default=r"D:\YYBWorkSpace\GitHub\VukanLearnResources",
    )
    parser.add_argument("--overwrite", action="store_true")
    return parser.parse_args()


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def write_json(path: Path, value: dict[str, Any], overwrite: bool) -> None:
    if path.exists() and not overwrite:
        raise RuntimeError("Output exists: %s" % path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=4)
        stream.write("\n")


def collect_ordered_material_slots(audit: dict[str, Any]) -> list[str]:
    # 运行时按 glTF primitive 的材质名绑定槽位；这里按首次出现顺序去重，保持导出审计的稳定顺序。
    ordered_slots = []
    for mesh in audit["gltfValidation"]:
        for primitive in mesh["primitives"]:
            slot_name = primitive["materialName"]
            if not slot_name:
                raise RuntimeError("glTF audit contains an unnamed material slot")
            if slot_name not in ordered_slots:
                ordered_slots.append(slot_name)
    missing_slots = [slot_name for slot_name in MIGRATED_MATERIALS if slot_name not in ordered_slots]
    if missing_slots:
        raise RuntimeError(
            "Migrated material slots are missing from glTF audit: %s"
            % ", ".join(missing_slots)
        )
    return ordered_slots


def build_hair_core_gltf(source_gltf: dict[str, Any]) -> dict[str, Any]:
    materials = source_gltf.get("materials", [])
    target_material_indices = {
        index
        for index, material in enumerate(materials)
        if material.get("name") == HAIR_MODE8_SLOT
    }
    if len(target_material_indices) != 1:
        raise RuntimeError(
            "Hair mode 8 source material must resolve exactly once: %s"
            % HAIR_MODE8_SLOT
        )

    target_material_index = next(iter(target_material_indices))
    hair_nodes: list[dict[str, Any]] = []
    hair_meshes: list[dict[str, Any]] = []
    for source_node in source_gltf.get("nodes", []):
        source_mesh_index = source_node.get("mesh")
        if source_mesh_index is None:
            continue
        source_mesh = source_gltf["meshes"][source_mesh_index]
        selected_primitives = []
        for source_primitive in source_mesh.get("primitives", []):
            if source_primitive.get("material") != target_material_index:
                continue
            primitive = copy.deepcopy(source_primitive)
            primitive["material"] = 0
            selected_primitives.append(primitive)
        if not selected_primitives:
            continue

        hair_mesh = {"primitives": selected_primitives}
        if "name" in source_mesh:
            hair_mesh["name"] = source_mesh["name"] + "_HairCore"
        hair_meshes.append(hair_mesh)

        hair_node = {
            "name": source_node.get("name", "") + "_HairCore",
            "mesh": len(hair_meshes) - 1,
        }
        # 当前静态 Pose 节点应为单位变换；仍原样复制显式字段，避免过滤资源改变几何空间。
        for field_name in ("translation", "rotation", "scale", "matrix", "extras"):
            if field_name in source_node:
                hair_node[field_name] = copy.deepcopy(source_node[field_name])
        hair_nodes.append(hair_node)

    # 两个源 Hair 网格都包含同名 Card primitive；缺少任一份都表示导出合同已漂移。
    if len(hair_nodes) != 2:
        raise RuntimeError(
            "Hair mode 8 core expects two source mesh nodes, got %d" % len(hair_nodes)
        )

    return {
        "asset": copy.deepcopy(source_gltf.get("asset", {"version": "2.0"})),
        "scene": 0,
        "scenes": [{"nodes": list(range(len(hair_nodes)))}],
        "nodes": hair_nodes,
        "materials": [copy.deepcopy(materials[target_material_index])],
        "meshes": hair_meshes,
        # Hair-only glTF 复用原 b_f_3725.bin；保留完整 accessor/view 表可避免复制二进制。
        "accessors": copy.deepcopy(source_gltf.get("accessors", [])),
        "bufferViews": copy.deepcopy(source_gltf.get("bufferViews", [])),
        "buffers": copy.deepcopy(source_gltf.get("buffers", [])),
    }


def build_mesh_asset(material_slots: list[str]) -> dict[str, Any]:
    unmapped_slots = [slot_name for slot_name in material_slots if slot_name not in MIGRATED_MATERIALS]
    if unmapped_slots:
        raise RuntimeError("Unmapped character material slots: %s" % ", ".join(unmapped_slots))
    return {
        "name": "b_f_3725 Restored Character",
        "type": "mesh",
        "modelDataPath": "models/datas/neox/b_f_3725/b_f_3725.gltf",
        "materialSlots": [
            {
                "name": slot_name,
                "materialInstancePath": MIGRATED_MATERIALS[slot_name],
            }
            for slot_name in material_slots
        ],
    }


def build_hair_core_mesh_asset() -> dict[str, Any]:
    return {
        "name": "b_f_3725 Hair Mode 8 Alpha Test Core",
        "type": "mesh",
        "modelDataPath": HAIR_MODE8_CORE_GLTF,
        "materialSlots": [
            {
                "name": HAIR_MODE8_SLOT,
                "materialInstancePath": HAIR_MODE8_CORE_MI,
            }
        ],
    }


def build_scene() -> dict[str, Any]:
    return {
        "name": "b_f_3725 P0 Character Validation",
        "type": "scene",
        "objects": [
            {
                "name": "b_f_3725",
                "type": "mesh",
                "modelPath": "models/neox/b_f_3725/SM_b_f_3725_p0.json",
                "position": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "rotation": [0.0, 0.0, 0.0],
            },
            {
                "name": "b_f_3725_HairMode8Core",
                "type": "mesh",
                "modelPath": HAIR_MODE8_CORE_MODEL,
                "position": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
                "rotation": [0.0, 0.0, 0.0],
            },
            # 角色验证只保留独立相机，场景光照与汽车展示场景共用同一基线，避免材质对比混入灯光差异。
            {
                "name": "Character_Key",
                "type": "directionalLight",
                "position": [0.0, 1.0, 0.0],
                "rotation": [-45.0, 45.0, 0.0],
                "color": [1.0, 1.0, 1.0],
                "intensity": 10.0,
                "shadow": {
                    "castShadows": True,
                    "dynamicShadowDistance": 300.0,
                    "dynamicShadowCascades": 4,
                    "cascadeDistributionExponent": 1.6,
                    "cascadeTransitionFraction": 0.0,
                    "shadowDistanceFadeoutFraction": 0.23,
                    "shadowBias": 0.69,
                    "shadowSlopeBias": 0.48,
                    "shadowCascadeBiasDistribution": 0.0,
                },
            },
            {
                "name": "Character_Fill",
                "type": "pointLight",
                "position": [-1.5, 1.2, 2.0],
                "rotation": [0.0, 0.0, 0.0],
                "color": [0.35, 0.55, 1.0],
                "intensity": 0.0,
                "radius": 5.0,
            },
            {
                "name": "Character_Camera",
                "type": "camera",
                "fov": 42.0,
                "near_clip": 0.05,
                "far_clip": 100.0,
                # 固定头部近景机位；显式 look_at 避免启动兼容逻辑重新看向世界原点。
                "position": [0.0, 1.45, 1.45],
                "look_at": [0.0, 1.45, 0.0],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            },
            {
                "name": "Character_Environment",
                "type": "environment",
                "environment": {
                    "type": "proceduralSky",
                    "cubeSize": 128,
                    "intensity": 1.0,
                    "skyParameters": {
                        "sunIntensity": 1.0,
                        "sunColor": [22.0, 17.5, 10.0],
                        "sunAngularRadius": 0.08,
                        "zenithColor": [0.09, 0.32, 0.95],
                        "horizonColor": [0.85, 0.78, 0.58],
                        "groundColor": [0.06, 0.07, 0.055],
                        "skyGradientExponent": 0.42,
                        "groundGradientExponent": 0.35,
                        "sunHaloExponent": 96.0,
                        "sunHaloStrength": 0.45,
                    },
                },
            },
        ],
    }


def main() -> None:
    arguments = parse_arguments()
    resource_root = Path(arguments.resource_root).resolve()
    audit_path = resource_root / "models/datas/neox/b_f_3725/b_f_3725.audit.json"
    source_gltf_path = resource_root / HAIR_MODE8_SOURCE_GLTF
    required_paths = [audit_path, source_gltf_path, resource_root / HAIR_MODE8_CORE_MI]
    required_paths.extend(resource_root / path for path in MIGRATED_MATERIALS.values())
    for required_path in required_paths:
        if not required_path.is_file():
            raise RuntimeError("Required asset is missing: %s" % required_path)

    audit = read_json(audit_path)
    if audit.get("staticBakePolicy", {}).get("skinsExported") is not False:
        raise RuntimeError("Audit is not a validated static glTF export: %s" % audit_path)
    material_slots = collect_ordered_material_slots(audit)
    source_gltf = read_json(source_gltf_path)

    mesh_path = resource_root / "models/neox/b_f_3725/SM_b_f_3725_p0.json"
    hair_core_gltf_path = resource_root / HAIR_MODE8_CORE_GLTF
    hair_core_mesh_path = resource_root / HAIR_MODE8_CORE_MODEL
    scene_path = resource_root / "scenes/SC_b_f_3725_p0.json"
    write_json(mesh_path, build_mesh_asset(material_slots), arguments.overwrite)
    write_json(hair_core_gltf_path, build_hair_core_gltf(source_gltf), arguments.overwrite)
    write_json(hair_core_mesh_path, build_hair_core_mesh_asset(), arguments.overwrite)
    write_json(scene_path, build_scene(), arguments.overwrite)

    print("Created b_f_3725 P0 mesh asset with %d real material slots" % len(material_slots))
    for slot_name, material_path in MIGRATED_MATERIALS.items():
        print("Migrated slot: %s -> %s" % (slot_name, material_path))

    print("Unmigrated slots: 0")

    print("Mesh asset: %s" % mesh_path)
    print("Hair mode 8 core glTF: %s" % hair_core_gltf_path)
    print("Hair mode 8 core mesh asset: %s" % hair_core_mesh_path)
    print("Scene: %s" % scene_path)


if __name__ == "__main__":
    main()



