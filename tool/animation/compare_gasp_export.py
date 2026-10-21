"""Compare UE audit poses with raw Assimp inspection JSON, outside the runtime.

Requires numpy/scipy; matplotlib is required only with --figure. This measures
the selected exporter policy in source centimeters, not a VL coordinate import.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation, Slerp


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def trs(translation, rotation, scale):
    result = np.eye(4)
    result[:3, :3] = Rotation.from_quat(rotation).as_matrix() @ np.diag(scale)
    result[:3, 3] = translation
    return result


def source_matrix(value):
    return trs(value["translation"], value["rotationXyzw"], value["scale"])


def sample_vector(keys, tick):
    times = [key["tick"] for key in keys]
    values = np.array([key["value"] for key in keys])
    return [float(np.interp(tick, times, values[:, axis])) for axis in range(3)]


def sample_rotation(keys, tick):
    times = [key["tick"] for key in keys]
    values = [key["valueXyzw"] for key in keys]
    if len(keys) == 1 or tick <= times[0]:
        return values[0]
    if tick >= times[-1]:
        return values[-1]
    return Slerp(times, Rotation.from_quat(values))([tick]).as_quat()[0]


def model_matrices(inspection, animation, time, stop_at=None):
    channels = {item["name"]: item for item in animation["channels"]}
    tick = time * animation["ticksPerSecond"]
    models = {}
    # Assimp 审计按父先子后输出；缺轨骨骼保留 FBX 本地绑定变换，不能用单位矩阵。
    for node in inspection["nodes"]:
        name = node["name"]
        if name in models:
            raise ValueError("Duplicate node name: " + name)
        local = np.array(node["local"])
        if name in channels:
            channel = channels[name]
            if not all(channel[key] for key in ("positions", "rotations", "scales")):
                raise ValueError("Partial TRS channel requires an explicit policy: " + name)
            local = trs(sample_vector(channel["positions"], tick),
                        sample_rotation(channel["rotations"], tick),
                        sample_vector(channel["scales"], tick))
        parent = node["parent"]
        models[name] = models[parent] @ local if parent else local
        if name == stop_at:
            break
    return models


def compare_clip(source, inspection, basis):
    if len(inspection["animations"]) != 1:
        raise ValueError("Expected exactly one FBX animation")
    animation = inspection["animations"][0]
    if animation["ticksPerSecond"] <= 0:
        raise ValueError("Missing animation time unit")
    frames = []
    virtual = set()
    real_names = {bone["name"] for bone in source["referenceFrames"][0]["bones"]
                  if not bone["name"].startswith("VB ")}
    channel_names = {channel["name"] for channel in animation["channels"]}
    for frame in source["referenceFrames"]:
        models = model_matrices(inspection, animation, frame["time"])
        errors = []
        for bone in frame["bones"]:
            name = bone["name"]
            if name.startswith("VB "):
                virtual.add(name)
                continue
            if name not in models:
                raise ValueError("Missing real bone: " + name)
            expected = basis @ source_matrix(bone["model"]) @ basis
            actual = models[name]
            distance = float(np.linalg.norm(actual[:3, 3] - expected[:3, 3]))
            expected_rotation = expected[:3, :3] / np.linalg.norm(expected[:3, :3], axis=0)
            actual_rotation = actual[:3, :3] / np.linalg.norm(actual[:3, :3], axis=0)
            angle = float(np.degrees(Rotation.from_matrix(
                expected_rotation.T @ actual_rotation).magnitude()))
            errors.append({"bone": name, "positionErrorCm": distance,
                           "rotationErrorDegrees": angle,
                           "linearMatrixMaxError": float(np.max(np.abs(
                               actual[:3, :3] - expected[:3, :3])))})
        frames.append({
            "time": frame["time"],
            "maxPositionErrorCm": max(item["positionErrorCm"] for item in errors),
            "maxRotationErrorDegrees": max(item["rotationErrorDegrees"] for item in errors),
            "bones": errors,
        })
    duration = animation["durationTicks"] / animation["ticksPerSecond"]
    root_errors = []
    for sample in source["rawRootSamples"]:
        actual = model_matrices(inspection, animation, sample["time"], stop_at="root")["root"]
        expected = basis @ source_matrix(sample["transform"]) @ basis
        root_errors.append(float(np.linalg.norm(actual[:3, 3] - expected[:3, 3])))
    return {
        "source": source["asset"], "sourceDuration": source["duration"],
        "fbxDuration": duration, "durationErrorSeconds": abs(duration - source["duration"]),
        "realBoneCount": len(real_names), "excludedVirtualBones": sorted(virtual),
        "realBonesWithoutAnimationChannel": sorted(real_names - channel_names),
        "missingAuthoredTracks": sorted(set(source["tracks"]) - channel_names),
        "restPoseFallbackBones": sorted(real_names - set(source["tracks"])),
        "maxPositionErrorCm": max(frame["maxPositionErrorCm"] for frame in frames),
        "maxRotationErrorDegrees": max(frame["maxRotationErrorDegrees"] for frame in frames),
        "maxRootPositionErrorCm": max(root_errors), "frames": frames,
    }


def compare_mesh(skeleton, inspection, basis):
    source_bones = {bone["name"]: bone for bone in skeleton["bones"]
                    if not bone["name"].startswith("VB ")}
    nodes = {}
    for node in inspection["nodes"]:
        if node["name"] in source_bones:
            if node["name"] in nodes:
                raise ValueError("Ambiguous skeleton node: " + node["name"])
            nodes[node["name"]] = node
    missing = sorted(set(source_bones) - set(nodes))
    if missing:
        raise ValueError("Mesh FBX missing real bones: " + repr(missing))
    positions = []
    rotations = []
    for name, bone in source_bones.items():
        expected = basis @ source_matrix(bone["referenceModel"]) @ basis
        actual = np.array(nodes[name]["model"])
        positions.append(float(np.linalg.norm(actual[:3, 3] - expected[:3, 3])))
        expected_rotation = expected[:3, :3] / np.linalg.norm(expected[:3, :3], axis=0)
        actual_rotation = actual[:3, :3] / np.linalg.norm(actual[:3, :3], axis=0)
        rotations.append(float(np.degrees(Rotation.from_matrix(
            expected_rotation.T @ actual_rotation).magnitude())))
    bind_positions = []
    bind_linear = []
    for mesh_index, mesh in enumerate(inspection["meshes"]):
        owners = [node for node in inspection["nodes"] if mesh_index in node["meshIndices"]]
        if not owners:
            raise ValueError("Mesh has no node owner")
        for owner in owners:
            mesh_model = np.array(owner["model"])
            for bone in mesh["bones"]:
                if bone["weights"] == 0:
                    continue
                actual = np.array(nodes[bone["name"]]["model"]) @ np.array(bone["offset"])
                bind_positions.append(float(np.linalg.norm(actual[:3, 3] - mesh_model[:3, 3])))
                bind_linear.append(float(np.max(np.abs(actual[:3, :3] - mesh_model[:3, :3]))))
    return {
        "realBones": len(source_bones), "missingRealBones": missing,
        "maxReferencePositionErrorCm": max(positions),
        "maxReferenceRotationErrorDegrees": max(rotations),
        "maxBindTranslationResidualCm": max(bind_positions),
        "maxBindLinearResidual": max(bind_linear),
        "weightStats": [{key: mesh[key] for key in (
            "vertices", "faces", "maxInfluences", "invalidWeights",
            "unweightedVertices", "maxWeightSumError")} for mesh in inspection["meshes"]],
        "scope": "Bind matrices only; vertex skinning and weight remapping are not evaluated",
    }


def plot_walk(source, inspection, skeleton, basis, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    figure, axes = plt.subplots(1, 5, figsize=(15, 6), sharey=True)
    parents = {bone["name"]: bone["parent"] for bone in skeleton["bones"]}
    animation = inspection["animations"][0]
    for axis, frame in zip(axes, source["referenceFrames"]):
        models = model_matrices(inspection, animation, frame["time"])
        expected = {bone["name"]: (basis @ source_matrix(bone["model"]) @ basis)[:3, 3]
                    for bone in frame["bones"] if not bone["name"].startswith("VB ")}
        origin = expected["root"]
        for name, position in expected.items():
            parent = parents[name]
            if parent not in expected or name.startswith(("ik_", "prop", "poi")):
                continue
            ue_segment = np.array([expected[parent] - origin, position - origin])
            fbx_segment = np.array([models[parent][:3, 3] - origin,
                                    models[name][:3, 3] - origin])
            # 该 Walk_F 源根轨道沿 UE +Y；B 已翻转 Y，投影时恢复源 +Y 的标注。
            axis.plot(-ue_segment[:, 1], ue_segment[:, 2], color="#16856b", linewidth=2)
            axis.plot(-fbx_segment[:, 1], fbx_segment[:, 2], color="#ca3d68",
                      linewidth=1, linestyle="--")
        axis.set_title(f't = {frame["time"]:.3f} s', fontsize=11)
        axis.set_aspect("equal", adjustable="box")
        axis.grid(alpha=0.2)
        axis.set_xlabel("Source +Y (cm)")
    axes[0].set_ylabel("Up Z (cm)")
    axes[0].plot([], [], color="#16856b", label="UE RAW pose")
    axes[0].plot([], [], color="#ca3d68", linestyle="--", label="FBX / Assimp")
    axes[0].legend(fontsize=8, loc="upper left")
    figure.suptitle("GASP walk export: UE reference vs FBX, root-relative YZ projection")
    figure.text(0.5, 0.01, "Source-space audit only. This is not VulkanLearn rendering.",
                ha="center", fontsize=10)
    figure.tight_layout(rect=(0, 0.1, 1, 0.94))
    figure.savefig(path, dpi=150)
    plt.close(figure)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--inspection", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--figure", type=Path)
    arguments = parser.parse_args()
    if arguments.output.exists() or (arguments.figure and arguments.figure.exists()):
        raise ValueError("Choose new output paths")
    audit = read(arguments.audit / "audit.json")
    # UE 左手源空间到当前 FBX 策略的右手空间。固定此基底做检验，不拟合误差。
    basis = np.diag([1.0, -1.0, 1.0, 1.0])
    report = {
        "format": "vl.gasp.fbx-comparison.v1",
        "basis": "FBX = (UE.X, -UE.Y, UE.Z), centimeters, no GlobalScale",
        "scope": "Five sampled RAW reference poses per clip; raw root translation at 60 Hz",
        "positionToleranceCm": 0.1, "rotationToleranceDegrees": 0.1,
        "durationToleranceSeconds": 0.00001, "clips": [],
    }
    mesh = compare_mesh(read(arguments.audit / "skeleton.json"),
                        read(arguments.inspection / "SKM_UEFN_Mannequin.json"), basis)
    mesh["passed"] = (
        mesh["maxReferencePositionErrorCm"] <= report["positionToleranceCm"]
        and mesh["maxReferenceRotationErrorDegrees"] <= report["rotationToleranceDegrees"]
        and mesh["maxBindTranslationResidualCm"] <= report["positionToleranceCm"]
        and mesh["maxBindLinearResidual"] <= 0.0001
        and all(item["invalidWeights"] == 0 and item["unweightedVertices"] == 0
                and item["maxWeightSumError"] <= 0.0001 for item in mesh["weightStats"]))
    report["mesh"] = mesh
    for entry in audit["clips"]:
        stem = Path(entry["fbx"]["file"]).stem
        source = read(arguments.audit / (stem + ".json"))
        inspection = read(arguments.inspection / (stem + ".json"))
        result = compare_clip(source, inspection, basis)
        # 源片段本就缺轨的骨骼使用绑定姿态，并已参与上面的姿态误差比较；
        # 拒绝的是作者已有轨道在导出时丢失，不能要求每根道具骨都凭空新增轨道。
        result["passed"] = (
            result["maxPositionErrorCm"] <= report["positionToleranceCm"]
            and result["maxRotationErrorDegrees"] <= report["rotationToleranceDegrees"]
            and result["maxRootPositionErrorCm"] <= report["positionToleranceCm"]
            and result["durationErrorSeconds"] <= report["durationToleranceSeconds"]
            and not result["missingAuthoredTracks"])
        report["clips"].append(result)
        if arguments.figure and stem == "M_Neutral_Walk_Loop_F":
            plot_walk(source, inspection, read(arguments.audit / "skeleton.json"),
                      basis, arguments.figure)
    report["passed"] = (mesh["passed"] and bool(report["clips"])
                        and all(item["passed"] for item in report["clips"]))
    with arguments.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write("\n")
    print(json.dumps({"passed": report["passed"], "clips": len(report["clips"]),
                      "maxPositionErrorCm": max(item["maxPositionErrorCm"]
                                               for item in report["clips"])}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
