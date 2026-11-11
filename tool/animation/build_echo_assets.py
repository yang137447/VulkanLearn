#!/usr/bin/env python3
"""Build VL skeleton, sequence sidecars and mesh asset for Valley Echo.

Inputs are the read-only UE export directory produced by
``export_echo_assets.py`` and the Assimp FBX inspection from
``vl_animation_probe --fbx``.  Outputs are written into the resource checkout.

The FBX is the authority for the bind hierarchy and rest transforms; the UE
reference skeleton only cross-checks names, parents, edge lengths and root
distances.  No Actor/component transform is baked into the animation space.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from gasp_anim_common import (
    blake3_file,
    blake3_hex,
    json_bytes,
    preflight_targets,
    write_json,
)


BASIS = (
    (1.0, 0.0, 0.0, 0.0),
    (0.0, 0.0, 1.0, 0.0),
    (0.0, -1.0, 0.0, 0.0),
    (0.0, 0.0, 0.0, 1.0),
)
ROOT_JOINT = "root"
SKELETON_ID = "SK_Echo"
ASSET_NAME = "Echo"
EDGE_LENGTH_TOLERANCE = 1e-3
ROOT_DISTANCE_TOLERANCE = 1e-3
ORTHONORMAL_TOLERANCE = 1e-4

ROOT_LOCK_NAMES = {
    "REF_POSE": "RefPose",
    "ANIM_FIRST_FRAME": "AnimFirstFrame",
    "ZERO": "Zero",
}
ADDITIVE_NAMES = {
    "AAT_NONE": "None",
    "AAT_LOCAL_SPACE_BASE": "LocalSpaceBase",
    "AAT_ROTATION_OFFSET_MESH_SPACE": "RotationOffsetMeshSpace",
}


def identity4():
    return [[1.0 if row == column else 0.0 for column in range(4)]
            for row in range(4)]


def multiply(left, right):
    return [[sum(left[row][k] * right[k][column] for k in range(4))
             for column in range(4)] for row in range(4)]


def transpose(matrix):
    return [[matrix[column][row] for column in range(4)] for row in range(4)]


def convert_to_vl(matrix):
    basis = [list(row) for row in BASIS]
    return multiply(multiply(basis, matrix), transpose(basis))


def node_matrix(node):
    local = node.get("local")
    if not isinstance(local, list) or len(local) != 4:
        raise RuntimeError("FBX node %r has no 4x4 local matrix" %
                           node.get("name"))
    return [[float(value) for value in row] for row in local]


def model_matrix(node, nodes):
    chain = []
    current = node
    while current is not None:
        chain.append(current)
        parent_index = current.get("parentIndex", -1)
        current = nodes[parent_index] if parent_index >= 0 else None
    result = identity4()
    for entry in reversed(chain):
        result = multiply(result, node_matrix(entry))
    return result


def validate_rest_matrix(name, matrix):
    for row in matrix:
        for value in row:
            if not math.isfinite(value):
                raise RuntimeError("Non-finite rest transform for joint %r" % name)
    linear = [[matrix[row][column] for column in range(3)] for row in range(3)]
    columns = [[linear[row][column] for row in range(3)] for column in range(3)]
    for left in range(3):
        for right in range(3):
            dot = sum(columns[left][axis] * columns[right][axis]
                      for axis in range(3))
            if abs(dot - (1.0 if left == right else 0.0)) > ORTHONORMAL_TOLERANCE:
                raise RuntimeError("Sheared rest transform for joint %r" % name)


def translation(matrix):
    return [matrix[0][3], matrix[1][3], matrix[2][3]]


def distance(left, right):
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(left, right)))


def build_fbx_hierarchy(inspection):
    nodes = inspection.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        raise RuntimeError("FBX inspection has no nodes")
    roots = [node for node in nodes if node.get("name") == ROOT_JOINT]
    if len(roots) != 1:
        raise RuntimeError("FBX must contain exactly one %r joint" % ROOT_JOINT)
    root = roots[0]
    children = {}
    for node in nodes:
        children.setdefault(int(node.get("parentIndex", -1)), []).append(node)
    for group in children.values():
        group.sort(key=lambda node: int(node.get("index", 0)))
    joints = []

    def visit(node):
        name = str(node.get("name", ""))
        if not name:
            raise RuntimeError("FBX joint with empty name")
        if name.startswith("VB "):
            raise RuntimeError("Virtual bone %r unexpectedly appears in FBX" % name)
        joints.append(node)
        for child in children.get(int(node.get("index", -1)), []):
            visit(child)

    visit(root)
    matrices = []
    for joint in joints:
        source = model_matrix(joint, nodes) if joint is root else node_matrix(joint)
        matrix = convert_to_vl(source)
        validate_rest_matrix(str(joint["name"]), matrix)
        matrices.append(matrix)
    return joints, matrices


def cross_check(fbx_joints, fbx_matrices, ue_bones):
    fbx_names = [str(joint["name"]) for joint in fbx_joints]
    ue_names = [str(bone["name"]) for bone in ue_bones]
    if fbx_names != ue_names:
        missing = sorted(set(fbx_names) - set(ue_names))
        extra = sorted(set(ue_names) - set(fbx_names))
        raise RuntimeError("FBX/UE joint set mismatch: missing=%s extra=%s" %
                           (missing[:8], extra[:8]))
    fbx_index = {name: index for index, name in enumerate(fbx_names)}
    ue_index = {name: index for index, name in enumerate(ue_names)}
    for index, joint in enumerate(fbx_joints):
        parent = str(joint.get("parent", ""))
        expected = fbx_index.get(parent, -1)
        ue_parent = str(ue_bones[index].get("parent", "None"))
        actual = -1 if ue_parent == "None" else ue_index.get(ue_parent, -2)
        if expected != actual:
            raise RuntimeError("Parent mismatch for joint %r" % joint["name"])

    fbx_models = []
    for index, joint in enumerate(fbx_joints):
        parent = fbx_index.get(str(joint.get("parent", "")), -1)
        fbx_models.append(fbx_matrices[index] if parent < 0 else
                          multiply(fbx_models[parent], fbx_matrices[index]))
    fbx_positions = [translation(matrix) for matrix in fbx_models]
    ue_positions = [bone["referenceModel"]["translation"] for bone in ue_bones]
    max_edge_error = 0.0
    for index, joint in enumerate(fbx_joints):
        parent = fbx_index.get(str(joint.get("parent", "")), -1)
        if parent < 0:
            continue
        fbx_length = distance(fbx_positions[index], fbx_positions[parent])
        ue_length = distance(ue_positions[index], ue_positions[parent])
        max_edge_error = max(max_edge_error, abs(fbx_length - ue_length))
        if abs(fbx_length - ue_length) > EDGE_LENGTH_TOLERANCE:
            raise RuntimeError("FBX/UE rest edge-length mismatch for joint %r" %
                               joint["name"])
    for index, joint in enumerate(fbx_joints):
        if abs(distance(fbx_positions[index], fbx_positions[0]) -
               distance(ue_positions[index], ue_positions[0])) > ROOT_DISTANCE_TOLERANCE:
            raise RuntimeError("FBX/UE root-distance mismatch for joint %r" %
                               joint["name"])
    return max_edge_error


def significant(value, digits=9):
    if value == 0.0:
        return 0.0
    exponent = math.floor(math.log10(abs(value)))
    rounded = round(value, digits - 1 - exponent)
    return 0.0 if rounded == 0.0 else rounded


def collect_curves(clips):
    registry = {}
    for clip in clips:
        for curve in clip.get("curves", []):
            name = str(curve.get("name", ""))
            if name and len(name) <= 64:
                registry[name] = "Float"
    return {name: registry[name]
            for name in sorted(registry, key=lambda value: (value.casefold(), value))}


def notify_name(event):
    text = event.get("text")
    if isinstance(text, dict):
        text = text.get("ueStructText")
    if not isinstance(text, str):
        raise RuntimeError("Notify event has no UE struct text")
    marker = 'NotifyName="'
    start = text.find(marker)
    if start < 0:
        raise RuntimeError("Notify event text has no NotifyName: " + text[:160])
    start += len(marker)
    end = text.find('"', start)
    if end < 0:
        raise RuntimeError("Notify event has a malformed NotifyName")
    return text[start:end]


def settings_record(raw_clip):
    settings = raw_clip.get("settings")
    if not isinstance(settings, dict):
        raise RuntimeError("Clip %r has no settings" % raw_clip.get("name"))
    for key in ("enable_root_motion", "force_root_lock"):
        if not isinstance(settings.get(key), bool):
            raise RuntimeError("Clip %r setting %s is not a bool" %
                               (raw_clip.get("name"), key))
    lock = str(settings.get("root_motion_root_lock", "")).strip()
    lock = lock[1:-1].rsplit(".", 1)[-1].split(":", 1)[0].strip() \
        if lock.startswith("<") and lock.endswith(">") else lock
    if lock not in ROOT_LOCK_NAMES:
        raise RuntimeError("Clip %r has unknown RootMotionRootLock %r" %
                           (raw_clip.get("name"), lock))
    additive = str(settings.get("additive_anim_type", "")).strip()
    additive = additive[1:-1].rsplit(".", 1)[-1].split(":", 1)[0].strip() \
        if additive.startswith("<") and additive.endswith(">") else additive
    if additive not in ADDITIVE_NAMES:
        raise RuntimeError("Clip %r has unknown AdditiveAnimType %r" %
                           (raw_clip.get("name"), additive))
    return {
        "enableRootMotion": settings["enable_root_motion"],
        "forceRootLock": settings["force_root_lock"],
        "rootMotionRootLock": ROOT_LOCK_NAMES[lock],
        "additiveAnimType": ADDITIVE_NAMES[additive],
    }


def sequence_document(raw_clip, skeleton_blake3, joint_names, players):
    duration = float(raw_clip["duration"])
    if duration <= 0 or not math.isfinite(duration):
        raise RuntimeError("Clip %r has invalid duration" % raw_clip.get("name"))
    curves = []
    for raw_curve in raw_clip.get("curves", []):
        curves.append({
            "name": str(raw_curve["name"]),
            "times": [float(value) for value in raw_curve["times"]],
            "values": [float(value) for value in raw_curve["values"]],
        })
    notifies = []
    for raw_event in raw_clip.get("notifies", []):
        notifies.append({
            "track": str(raw_event["track"]),
            "name": notify_name(raw_event),
            "time": float(raw_event["time"]),
            "duration": float(raw_event["duration"]),
        })
    return {
        "format": "vl.anim-sequence.v1",
        "sequence": str(raw_clip["name"]),
        "skeleton": SKELETON_ID,
        "skeletonBlake3": skeleton_blake3,
        "duration": duration,
        "frameRate": raw_clip["frameRate"],
        "numFrames": int(raw_clip["numFrames"]),
        "numKeys": int(raw_clip["numKeys"]),
        "rateScale": float(raw_clip["rateScale"]),
        "rootMotion": settings_record(raw_clip),
        # UE 的 track 列表可能包含未蒙皮的 prop/attachment 节点；VL 的 sidecar
        # 只允许声明真实骨骼，FBX 动画导入也按骨架映射。
        "tracks": [str(name) for name in raw_clip.get("tracks", [])
                   if str(name) in joint_names],
        "floatCurveKeys": curves,
        "notifies": notifies,
        "syncMarkers": raw_clip.get("syncMarkers", []),
        # SyncGroup/role/start position/loop belong to AnimBP Sequence Players,
        # not to the AnimSequence itself. Keep every binding; runtime selects
        # by group name.
        "players": players,
        "sampling": {"interpolation": "linear", "outside": "clamp"},
        "source": {
            "fbx": str(raw_clip["name"]) + ".fbx",
            "uePackage": str(raw_clip.get("package", "")),
            "exportedBy": "export_echo_assets.py",
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--export", required=True, type=Path,
                        help="directory from export_echo_assets.py")
    parser.add_argument("--inspection", required=True, type=Path,
                        help="Assimp inspection JSON for Echo.fbx")
    parser.add_argument("--sync-metadata", type=Path,
                        help="JSON from extract_echo_sync.py (AnimBP sequence players)")
    parser.add_argument("--resources", required=True, type=Path,
                        help="VulkanLearn resource checkout")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    export_dir = args.export.resolve()
    resource_root = args.resources.resolve()
    raw = json.loads((export_dir / "ue-anim-raw.json").read_text(encoding="utf-8"))
    if raw.get("format") != "vl.echo.ue-animation-raw.v1" or raw.get("status") != "complete":
        raise RuntimeError("Echo UE export is incomplete")
    player_metadata = {}
    if args.sync_metadata:
        sync = json.loads(args.sync_metadata.read_text(encoding="utf-8"))
        if sync.get("format") != "vl.echo.anim-players.v1" or not isinstance(
                sync.get("players"), dict):
            raise RuntimeError("Unexpected sync metadata format")
        player_metadata = sync["players"]
    inspection = json.loads(args.inspection.read_text(encoding="utf-8"))
    if inspection.get("format") != "vl.animation.fbx-inspection.v1":
        raise RuntimeError("Unexpected FBX inspection format")

    fbx_joints, fbx_matrices = build_fbx_hierarchy(inspection)
    ue_bones = raw["skeleton"]["bones"]
    physical = [bone for bone in ue_bones if not str(bone["name"]).startswith("VB ")]
    virtual = [bone for bone in ue_bones if str(bone["name"]).startswith("VB ")]
    max_edge_error = cross_check(fbx_joints, fbx_matrices, physical)

    joints_document = []
    parent_index = {str(joint["name"]): index
                    for index, joint in enumerate(fbx_joints)}
    for joint in fbx_joints:
        flat = [significant(value) for row in fbx_matrices[len(joints_document)]
                for value in row]
        joints_document.append({
            "name": str(joint["name"]),
            "parent": parent_index.get(str(joint.get("parent", "")), -1),
            "restLocal": flat,
        })
    clips = raw.get("clips", [])
    if len(clips) != 24:
        raise RuntimeError("Expected 24 Echo AnimSequences, got %d" % len(clips))
    skeleton_document = {
        "format": "vl.skeleton.v1",
        "skeleton": SKELETON_ID,
        "root": ROOT_JOINT,
        "joints": joints_document,
        "slots": raw["skeleton"].get("slots") or ["DefaultSlot"],
        "curves": collect_curves(clips),
        "virtualBones": [{"name": str(bone["name"]),
                          "parent": str(bone.get("parent", "None"))}
                         for bone in virtual],
        "additiveReferencePose": None,
        "source": {
            "fbx": "Maps/SC_valley_echo/Source/SKM_Echo.fbx",
            "fbxBlake3": blake3_file(export_dir / "Source" / "Echo.fbx"),
            "tool": "build_echo_assets.py",
        },
    }

    skeletons_dir = resource_root / "Maps/SC_valley_echo/Skeletons"
    source_dir = resource_root / "Maps/SC_valley_echo/Source"
    materials_dir = resource_root / "Maps/SC_valley_echo/Materials"
    meshes_dir = resource_root / "Maps/SC_valley_echo/Meshes"
    skeleton_path = skeletons_dir / "SK_Echo.json"
    targets = [skeleton_path]
    targets += [source_dir / (clip["name"] + ".anim.json") for clip in clips]
    targets += [meshes_dir / "SKM_Echo.json", materials_dir / "MI_echo_neutral.json"]
    preflight_targets(targets, args.force)

    skeleton_bytes = json_bytes(skeleton_document)
    skeleton_path.parent.mkdir(parents=True, exist_ok=True)
    if args.force and skeleton_path.exists():
        skeleton_path.unlink()
    skeleton_path.write_bytes(skeleton_bytes)
    skeleton_blake3 = blake3_hex(skeleton_bytes)

    joint_names = {joint["name"] for joint in joints_document}
    for clip in clips:
        players = player_metadata.get(str(clip["name"]), [{
            "syncGroup": "",
            "syncRole": "CanBeLeader",
            "syncMethod": "DoNotSync",
            "syncGroupScope": "Local",
            "startPosition": 0.0,
            "loop": True,
        }])
        # 旧 sync metadata 没有 scope 字段时按 UE Echo 的默认 Local 写入资产，
        # 不把默认值留给运行时猜测。
        for player in players:
            player.setdefault("syncGroupScope", "Local")
        document = sequence_document(clip, skeleton_blake3, joint_names, players)
        write_json(source_dir / (clip["name"] + ".anim.json"), document, args.force)

    material_slots = []
    for mesh in inspection.get("meshes", []):
        name = str(mesh.get("materialName", ""))
        if not name:
            raise RuntimeError("FBX mesh is missing a material name")
        material_slots.append({
            "name": name,
            "materialInstancePath": "Maps/SC_valley_echo/Materials/MI_echo_neutral.json",
        })
    mesh_document = {
        "name": "Echo Skeletal",
        "type": "mesh",
        "description": "Valley of the Ancient Echo 骨骼角色；用于复刻 Idle/Walk/Jog 方向起步、起停、跳跃落地与 VaultOver/ReachOut 的动画状态机验证。Idle 动画在 VL 世界中的视觉正面需绕 Y 额外旋转 180° 才能对齐 yaw=0 -> -Z；不要沿用 UE +X 前向的直觉做 90° 校正。",
        "modelDataPath": "Maps/SC_valley_echo/Source/SKM_Echo.fbx",
        "skinning": {
            "skeleton": "Maps/SC_valley_echo/Skeletons/SK_Echo.json",
            "clips": [{
                "id": str(clip["name"]),
                "path": "Maps/SC_valley_echo/Source/" + str(clip["name"]) + ".fbx",
                "sequence": "Maps/SC_valley_echo/Source/" + str(clip["name"]) + ".anim.json",
            } for clip in clips],
        },
        "materialSlots": material_slots,
    }
    write_json(meshes_dir / "SKM_Echo.json", mesh_document, args.force)
    material_document = {
        "name": "Echo Neutral",
        "type": "materialInstance",
        "material": "shader/glsl/M_pbr.json",
        "macros": {"VL_SKINNED_MESH": 1},
        "parameters": {
            "u_tintColor": [0.55, 0.55, 0.6, 1],
            "u_pbrFactors": [0.6, 0, 1, 0],
        },
    }
    write_json(materials_dir / "MI_echo_neutral.json", material_document, args.force)
    print(json.dumps({
        "skeleton": str(skeleton_path),
        "joints": len(joints_document),
        "clips": len(clips),
        "materialSlots": len(material_slots),
        "maxEdgeErrorCm": max_edge_error,
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
