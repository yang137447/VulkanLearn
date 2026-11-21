import argparse
import copy
import json
import shutil
import subprocess
import sys
from pathlib import Path

from export_gasp_skeleton import build_fbx_hierarchy


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--export-dir", type=Path, required=True)
    parser.add_argument("--inspection", type=Path, required=True)
    parser.add_argument("--resource-root", type=Path, required=True)
    args = parser.parse_args()
    resources = args.resource_root.resolve()
    base = resources / "Maps/SC_motionbricks"
    source = args.export_dir / "SKM_Manny_Simple.fbx"
    inspection = read(args.inspection)
    joints, matrices = build_fbx_hierarchy(inspection)
    indices = {joint["name"]: index for index, joint in enumerate(joints)}
    skeleton = {
        "format": "vl.skeleton.v1", "skeleton": "SK_Manny", "root": "root",
        "joints": [
            {"name": joint["name"], "parent": -1 if joint["name"] == "root" else indices[joint["parent"]],
             "restLocal": [value for row in matrix for value in row]}
            for joint, matrix in zip(joints, matrices)],
        "slots": ["Default"], "curves": {}
    }
    write(base / "Skeletons/SK_Manny.json", skeleton)
    (base / "Source").mkdir(exist_ok=True)
    shutil.copyfile(source, base / "Source/SKM_Manny_Simple.fbx")
    shutil.copyfile(args.export_dir / "manny-export.json", base / "Source/manny-export.json")
    report = read(args.export_dir / "manny-export.json")
    material = read(resources / "Maps/SC_gasp/Materials/MI_mannequin.json")
    material["name"] = "MotionBricks UE5 Manny Silver"
    material["parameters"]["u_tintColor"] = [0.62, 0.65, 0.7, 1]
    material["parameters"]["u_pbrFactors"] = [0.4, 0.65, 1, 0]
    write(base / "Materials/MI_manny.json", material)
    mesh = {
        "name": "UE5 Manny Simple", "type": "mesh",
        "description": "UE5.8 官方模板 Manny Simple，MotionBricks 人形重定向演示。",
        "modelDataPath": "Maps/SC_motionbricks/Source/SKM_Manny_Simple.fbx",
        "skinning": {"skeleton": "Maps/SC_motionbricks/Skeletons/SK_Manny.json", "clips": []},
        "materialSlots": [
            {"name": name, "materialInstancePath": "Maps/SC_motionbricks/Materials/MI_manny.json"}
            for name in report["materialSlots"]]
    }
    write(base / "Meshes/SKM_manny.json", mesh)
    profile = read(base / "Retarget/RT_UEFN_From_SOMA.json")
    profile["targetSkeleton"] = "SK_Manny"
    profile["sourcePose"] = {"name": "BindPose", "rotationOffsets": []}
    write(base / "Retarget/RT_Manny_From_SOMA.json", profile)
    subprocess.run([sys.executable, str(Path(__file__).with_name("calibrate_motionbricks_retarget.py")),
        "--resource-root", str(resources),
        "--target-skeleton", "Maps/SC_motionbricks/Skeletons/SK_Manny.json",
        "--profile", "Maps/SC_motionbricks/Retarget/RT_Manny_From_SOMA.json",
        "--pose-name", "SOMA_AlignedToMannyApose"], check=True)
    blueprint = read(base / "Blueprints/BP_motionbricks_live.json")
    blueprint["name"] = "MotionBricks UE5 Manny"
    visual = next(component for component in blueprint["components"] if component["id"] == "visual")
    visual["modelPath"] = "Maps/SC_motionbricks/Meshes/SKM_manny.json"
    visual["motionBricks"]["retarget"] = "Maps/SC_motionbricks/Retarget/RT_Manny_From_SOMA.json"
    write(base / "Blueprints/BP_motionbricks_manny.json", blueprint)
    scene = read(base / "SC_motionbricks_live_probe.json")
    scene["name"] = "MotionBricks UE5 Manny"
    scene["description"] = "UE5 Manny 的 MotionBricks 实时动作生成与重定向演示，使用 SOMA 预训练模型、Manny 独立骨架与 T/A-pose 校准；工程对照场景，非论文验证。"
    player = next(item for item in scene["objects"] if item.get("id") == "player")
    player["name"] = "MotionBricks UE5 Manny"
    player["blueprintPath"] = "Maps/SC_motionbricks/Blueprints/BP_motionbricks_manny.json"
    write(base / "SC_motionbricks_manny.json", scene)
    comparison = copy.deepcopy(scene)
    comparison["name"] = "MotionBricks Manny and UEFN comparison"
    comparison["description"] = "MotionBricks 双角色重定向对照：左侧 UE5 Manny，右侧 UEFN Mannequin；使用相同模型与动作目标，独立推理实例，不宣称逐帧输出一致。"
    player = next(item for item in comparison["objects"] if item.get("id") == "player")
    player["position"][0] = -130
    second = copy.deepcopy(player)
    second["id"] = "uefn"
    second["name"] = "UEFN comparison"
    second["position"][0] = 130
    second["blueprintPath"] = "Maps/SC_motionbricks/Blueprints/BP_motionbricks_live.json"
    comparison["objects"].append(second)
    for blueprint_path in (base / "Blueprints/BP_motionbricks_manny.json",):
        independent = read(blueprint_path)
        for component in independent["components"]:
            if component["type"] == "camera":
                component["autoActivate"] = False
        write(base / "Blueprints/BP_motionbricks_manny_comparison.json", independent)
    player["blueprintPath"] = "Maps/SC_motionbricks/Blueprints/BP_motionbricks_manny_comparison.json"
    second.pop("blueprintPath")
    uefn = read(base / "Blueprints/BP_motionbricks_live.json")
    second["class"] = uefn["class"]
    second["physics"] = uefn["physics"]
    second["components"] = [item for item in uefn["components"] if item["type"] == "mesh"]
    camera = next(item for item in comparison["objects"] if item["type"] == "camera")
    camera["position"] = [0, 180, 800]
    camera["look_at"] = [0, 90, 0]
    write(base / "SC_motionbricks_manny_comparison.json", comparison)
    print(f"Built Manny assets: {len(joints)} joints, {len(report['materialSlots'])} materials")


if __name__ == "__main__":
    main()
