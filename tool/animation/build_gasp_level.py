"""Import the isolated DefaultLevel export through VL's normal OBJ/SM/Scene path."""
import argparse
import json
from pathlib import Path
import shutil
import warnings

import numpy as np
from scipy.spatial.transform import Rotation


BASIS = np.array([[1, 0, 0], [0, 0, 1], [0, 1, 0]], dtype=float)
PREFIX = "Maps/SC_gasp"


def placement(source):
    position = BASIS @ source["position"]
    scale = BASIS @ source["scale"]
    matrix = BASIS @ Rotation.from_quat(source["quaternion"]).as_matrix() @ BASIS
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        yaw, pitch, roll = Rotation.from_matrix(matrix).as_euler("YXZ", degrees=True)
    assert np.max(np.abs(Rotation.from_euler("YXZ", [yaw, pitch, roll], degrees=True).as_matrix() - matrix)) < 1e-8
    if not np.isfinite(position).all() or not np.isfinite(scale).all() or np.any(scale <= 0):
        raise ValueError("Invalid authored transform")
    return position.tolist(), [float(pitch), float(yaw), float(roll)], scale.tolist()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def obj_text(sections):
    lines = []
    offset = 1
    triangles = 0
    for index, section in enumerate(sections):
        vertices = np.asarray(section["vertices"]) @ BASIS.T
        normals = np.asarray(section["normals"]) @ BASIS.T
        if not np.isfinite(vertices).all() or not np.isfinite(normals).all():
            raise ValueError("Nonfinite section")
        lines.extend([f"o section_{index}", f"usemtl section_{index}"])
        lines.extend("v " + " ".join(f"{v:.9g}" for v in point) for point in vertices)
        lines.extend("vt " + " ".join(f"{v:.9g}" for v in point) for point in section["uvs"])
        lines.extend("vn " + " ".join(f"{v:.9g}" for v in point) for point in normals)
        indices = np.asarray(section["indices"]).reshape(-1, 3)
        for face in indices:
            a, b, c = face
            cross = np.cross(vertices[b] - vertices[a], vertices[c] - vertices[a])
            if np.linalg.norm(cross) < 1e-8:
                continue
            # UE 到 VL 为反射基底；OBJ 保持 CCW，正式静态导入器随后统一转为 Vulkan CW。
            if np.dot(cross, normals[face].sum(axis=0)) < 0:
                face = face[[0, 2, 1]]
            lines.append("f " + " ".join(f"{v + offset}/{v + offset}/{v + offset}" for v in face))
            triangles += 1
        offset += len(vertices)
    if triangles == 0:
        raise ValueError("Empty geometry")
    return "\n".join(lines) + "\n", triangles


def material_kind(instance):
    material = " ".join(instance["materials"]).lower()
    if "transparent_green" in material:
        return "green"
    if "transparent_red" in material:
        return "red"
    if "blue" in material:
        return "blue"
    return "gray"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--export", required=True, type=Path)
    parser.add_argument("--resources", required=True, type=Path)
    args = parser.parse_args()
    report = json.loads((args.export / "level.json").read_text(encoding="utf-8"))
    if report["status"] != "complete" or report["errors"] or not report["starts"]:
        parser.error("Expected a completed DefaultLevel export with a PlayerStart")
    target = args.resources.resolve() / PREFIX
    outputs = ["Source/Level", "Meshes/Level", "Materials/Level", "SC_gasp_playground.json",
               "Blueprints/BP_gasp_character.json"]
    if any((target / name).exists() for name in outputs):
        parser.error("Refusing to replace existing formal level outputs")
    source_dir = target / "Source/Level"
    source_dir.mkdir(parents=True)
    shutil.copyfile(args.export / "level.json", source_dir / "level.json")
    audit = {"sourceLevel": report["level"], "basis": "VL=(UE.X,UE.Z,UE.Y), cm",
             "materialPolicy": "VL PBR flat palette, not UE graph equivalence",
             "collisionPolicy": "LOD0 static triangle mesh for UE collision-enabled components",
             "frozenInteractions": "No UE teleport/button/widget/NPC blueprint execution",
             "meshes": {}, "instances": [], "trianglesPerUniqueMesh": {}}
    mesh_indices = {}
    for index, (path, mesh) in enumerate(report["meshes"].items()):
        name = f"level_{index:03d}"
        sections = json.loads((args.export / mesh["file"]).read_text())
        text, triangles = obj_text(sections)
        (source_dir / f"{name}.obj").write_text(text, encoding="ascii")
        shutil.copyfile(args.export / mesh["file"], source_dir / mesh["file"])
        mesh_indices[path] = (name, len(sections))
        audit["meshes"][path] = f"{PREFIX}/Source/Level/{name}.obj"
        audit["trianglesPerUniqueMesh"][path] = triangles
    for kind, color in {"gray": [0.42, 0.45, 0.49, 1], "blue": [0.035, 0.26, 0.65, 1],
                        "red": [0.65, 0.04, 0.045, 1], "green": [0.05, 0.55, 0.16, 1]}.items():
        write_json(target / f"Materials/Level/MI_{kind}.json", {
            "name": f"GASP Level {kind}", "type": "materialInstance", "material": "shader/glsl/M_pbr.json",
            "parameters": {"u_tintColor": color, "u_pbrFactors": [0.8, 0, 1, 0]}})
    descriptors = {}
    objects = []
    for index, instance in enumerate(report["instances"]):
        name, section_count = mesh_indices[instance["mesh"]]
        kind = material_kind(instance)
        key = (name, kind)
        if key not in descriptors:
            descriptor = f"{PREFIX}/Meshes/Level/SM_{name}_{kind}.json"
            write_json(args.resources / descriptor, {
                "type": "mesh", "name": f"GASP {name} {kind}",
                "modelDataPath": f"{PREFIX}/Source/Level/{name}.obj",
                "materialSlots": [{"name": f"section_{section}", "materialInstancePath":
                    f"{PREFIX}/Materials/Level/MI_{kind}.json"} for section in range(section_count)]})
            descriptors[key] = descriptor
        position, rotation, scale = placement(instance["transform"])
        actor_id = f"level-{index:03d}"
        obj = {"type": "blueprintInstance", "id": actor_id,
               "name": f'{instance["label"]} / {instance["component"]} / {instance["instance"]}',
               "class": "Gasp.StaticLevelBlueprint", "properties": {},
               "position": position, "rotation": rotation, "scale": [1, 1, 1],
               "components": [{"id": "visual", "type": "mesh",
                               "modelPath": descriptors[key], "scale": scale}]}
        if "NO_COLLISION" not in instance["collision"]:
            obj["physics"] = {"motion": "static", "shape": "mesh", "component": "visual", "friction": 0.8}
        objects.append(obj)
        audit["instances"].append({"id": actor_id, "source": instance, "target": obj})
    write_json(target / "Blueprints/BP_gasp_character.json", {
        "type": "blueprint", "name": "GASP Character", "class": "Gasp.CharacterBlueprint",
        "properties": {}, "instanceEditable": ["WalkSpeed", "RunSpeed", "LookSensitivity"],
        "physics": {"motion": "character", "shape": "capsule", "radius": 30, "halfHeight": 60,
                    "stepHeight": 30, "jumpSpeed": 500, "maxSlopeAngle": 45},
        "components": [
            {"id": "visual", "type": "mesh", "modelPath": f"{PREFIX}/Meshes/SM_mannequin.json",
             "position": [0, -90, 0], "rotation": [0, 180, 0]},
            {"id": "springArm", "type": "springArm", "position": [0, 0, 0],
             "rotation": [-12, 0, 0], "targetArmLength": 450, "probeRadius": 12,
             "doCollisionTest": True, "usePawnControlRotation": True},
            {"id": "camera", "type": "camera", "parent": "springArm",
             "position": [0, 0, 0], "rotation": [0, 0, 0],
             "fov": 65, "near_clip": 5, "far_clip": 50000}]})
    start, _, _ = placement(report["starts"][0]["transform"])
    objects.append({"type": "blueprintInstance", "id": "player", "name": "GASP Player",
                    "blueprintPath": f"{PREFIX}/Blueprints/BP_gasp_character.json",
                    "properties": {}, "position": start, "rotation": [0, -90, 0], "scale": [1, 1, 1]})
    objects.extend([
        {"name": "camera", "type": "camera", "position": [6800, 5500, 9000], "look_at": [2600, 400, 0],
         "rotation": [0, 0, 0], "scale": [1, 1, 1], "fov": 50, "near_clip": 5, "far_clip": 50000,
         "exposure": {"mode": "ev100", "ev100": 8, "compensation": 0, "aperture": 4,
                      "shutterSeconds": 0.008, "iso": 100, "autoMinEv100": -10, "autoMaxEv100": 20}},
        {"name": "sun", "type": "directionalLight", "position": [0, 5000, 0],
         "rotation": [-50, -35, 0], "color": [1, 0.96, 0.9], "intensity": 2000, "intensityUnit": "lux",
         "shadow": {"dynamicShadowDistance": 15000}},
        {"name": "environment", "type": "environment", "environment": {"type": "hdri", "intensity": 0.2,
         "cubeSize": 128, "hdrPath": "Common/Environments/UE_AssetsPreview/EpicQuadPanorama_Gray.hdr"}}])
    write_json(target / "SC_gasp_playground.json", {
        "name": "GASP DefaultLevel", "type": "scene", "worldUnit": "cm",
        "description": (
            "GASP DefaultLevel 原始 LOD0 几何、摆放与出生点迁移，静态三角碰撞配合 C# 高层状态机与"
            "原生 Motion Matching 基础搜索、局部 TRS 交叉混合，支持 Idle/Walk/Run/Jump/Fall/Land。"
            "工程验收场景，非论文验证；本步骤材质采用 VL PBR 简化色板，贴图、地表与 Logo 由后续"
            "材质导入步骤建立。可选静态支撑足部接地默认关闭；灯光与动作不宣称 UE 等价，"
            "UE 蓝图交互、完整 UE FootPlacement、惯性化与步幅变形尚未实现。"),
        "objects": objects})
    audit["instanceCount"] = len(report["instances"])
    audit["collisionCount"] = sum(obj.get("physics", {}).get("shape") == "mesh" for obj in objects)
    write_json(args.resources / "Generated/Import/SC_gasp/level-import.json", audit)
    print(json.dumps({"scene": f"{PREFIX}/SC_gasp_playground.json",
                      "uniqueMeshes": len(mesh_indices), "instances": len(report["instances"]),
                      "staticColliders": sum(obj.get("physics", {}).get("shape") == "mesh" for obj in objects)}))


if __name__ == "__main__":
    main()
