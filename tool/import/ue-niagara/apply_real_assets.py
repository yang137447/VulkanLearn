"""Replace stand-in references in the three Niagara galleries."""

from __future__ import annotations

import json
from pathlib import Path


RESOURCE_ROOT = Path(
    r"D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/niagara-particle-system/"
    r"VukanLearnResources"
)


def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def save(path: Path, document) -> None:
    path.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")


def replace_strings(value, replacements):
    if isinstance(value, str):
        for old, new in replacements.items():
            value = value.replace(old, new)
        return value
    if isinstance(value, list):
        return [replace_strings(item, replacements) for item in value]
    if isinstance(value, dict):
        return {key: replace_strings(item, replacements) for key, item in value.items()}
    return value


def patch_skeletalmesh() -> None:
    folder = RESOURCE_ROOT / "Maps/SC_particle_ue_skeletalmesh"
    replacements = {
        "Maps/SC_particle_ue_skeletalmesh/Meshes/SM_mannequin_particle.json":
            "Common/Meshes/SM_ue_dino.json",
        "Maps/SC_gasp/Materials/MI_mannequin.json":
            "Common/Materials/MI_ue_dino.json",
    }
    for path in folder.glob("PS_*.json"):
        save(path, replace_strings(load(path), replacements))
    scene_path = folder / "SC_niagara_skeletalmesh_gallery.json"
    scene = load(scene_path)
    scene["description"] = (
        "UE Niagara 骨骼网格示例复刻画廊：参考 NiagaraExamples FX_SkeletalMesh 的 "
        "NS_Dino_Tri_Loop 与 NS_Dino_Tris_Color_Burst。左侧为持续发射的三角面骨骼表面循环，"
        "右侧为短寿命、高初速、彩色曲线的三角面爆发。Dino 模型、骨架与材质来自 UE 5.8 "
        "SK_DinoDragon FBX 导出；用于学习骨骼三角面粒子的结构与视觉对齐，不是论文验证场景。"
    )
    save(scene_path, scene)


def patch_misc() -> None:
    folder = RESOURCE_ROOT / "Maps/SC_particle_ue_misc"
    replacements = {
        "Maps/SC_particle_ue_misc/Meshes/SM_ue_misc_mannequin.json":
            "Common/Meshes/SM_ue_manny.json",
        "Maps/SC_particle_ue_misc/Materials/MI_ue_misc_mannequin.json":
            "Common/Materials/MI_ue_manny_head.json",
        "Maps/SC_particle_ue_misc/Materials/MI_ue_misc_mannequin_static.json":
            "Common/Materials/MI_ue_manny_head.json",
        "Maps/SC_particle_advanced/Meshes/SM_rock.json":
            "Common/Meshes/SM_ue_toycow.json",
        "Maps/SC_particle_advanced/Materials/MI_advanced_rock.json":
            "Common/Materials/MI_ue_toycow.json",
    }
    for path in folder.glob("PS_*.json"):
        save(path, replace_strings(load(path), replacements))
    scene_path = folder / "SC_particle_ue_misc.json"
    scene = load(scene_path)
    scene["description"] = (
        "UE 5.8 Niagara 示例复刻 gallery：参考 NiagaraExamples/FX_Misc 的 "
        "NS_Bubble_Burst、NS_HitDissolve 与四个 NS_SkeletalMeshTris/Bones_Burst/Loop。"
        "场景把六个独立 PS 横向排开展示，用于学习气泡破裂、ToyCow 命中溶解、"
        "骨骼三角面/骨段粒子的 burst 与 loop 结构。ToyCow 与 Manny 模型、骨架和贴图来自 "
        "UE 5.8 FBX/PNG 导出；不是论文验证场景。"
    )
    save(scene_path, scene)


def patch_player() -> None:
    folder = RESOURCE_ROOT / "Maps/SC_particle_ue_player"
    scene_path = folder / "SC_ue_player_gallery.json"
    scene = load(scene_path)
    body_prefixes = ("Buff", "DeBuff", "Electricity", "TeleportIn", "TeleportOut")
    body_meshes = {
        "Buff": "Common/Meshes/SM_ue_manny.json",
        "DeBuff": "Common/Meshes/SM_ue_quinn.json",
        "Electricity": "Common/Meshes/SM_ue_manny.json",
        "TeleportIn": "Common/Meshes/SM_ue_quinn.json",
        "TeleportOut": "Common/Meshes/SM_ue_manny.json",
    }
    kept = []
    inserted = set()
    for entry in scene["objects"]:
        name = entry.get("name", "")
        prefix = next((value for value in body_prefixes if name.startswith(value)), None)
        if prefix and name.endswith(("Torso", "Head", "ArmL", "ArmR", "Legs")):
            if prefix not in inserted:
                inserted.add(prefix)
                kept.append({
                    "name": prefix + "Body",
                    "type": "mesh",
                    "modelPath": body_meshes[prefix],
                    "position": [entry["position"][0], 0.0, 0.0],
                    "rotation": [0.0, 0.0, 0.0],
                    "scale": [1.0, 1.0, 1.0],
                })
            continue
        kept.append(entry)

    for entry in kept:
        if entry.get("name") in ("WeaponBuffRifle", "WeaponDeBuffRifle"):
            entry["modelPath"] = "Common/Meshes/SM_ue_rifle.json"
    for prefix, x in (("WeaponBuff", 560.0), ("WeaponDeBuff", 840.0)):
        if not any(entry.get("name") == prefix + "Body" for entry in kept):
            kept.append({
                "name": prefix + "Body",
                "type": "mesh",
                "modelPath": "Common/Meshes/SM_ue_manny.json",
                "position": [x, 0.0, 0.0],
                "rotation": [0.0, 0.0, 0.0],
                "scale": [1.0, 1.0, 1.0],
            })

    scene["objects"] = kept
    scene["description"] = (
        "UE Niagara FX_Player 七态对齐 gallery：NS_Player_Buff_Looping 的上升 ribbon/光柱、"
        "NS_Player_DeBuff_Looping 的下沉暗烟/紫光、NS_Player_Electricity_Looping 的 beam "
        "电弧与点光、NS_Player_Teleport_In/Out 的 mesh shard dissolve/汇聚/发散与 distortion、"
        "NS_Weapon_Buff/DeBuff_Looping 的武器附着循环光/火花。角色使用 UE 5.8 "
        "SKM_Manny_Simple / SKM_Quinn_Simple，武器使用 SM_Rifle；不是论文验证场景。"
    )
    save(scene_path, scene)


def main() -> None:
    patch_skeletalmesh()
    patch_misc()
    patch_player()
    print("patched SC_particle_ue_skeletalmesh, SC_particle_ue_misc, SC_particle_ue_player")


if __name__ == "__main__":
    main()
