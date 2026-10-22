"""Author a Female/GASP reference pose without changing either skin bind."""

import argparse
import copy
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation


MAP = Path("Maps/SC_tressfx_female")


def model_matrices(joints, locals_):
    models = []
    for joint, local in zip(joints, locals_):
        parent = joint["parent"]
        models.append(local if parent < 0 else models[parent] @ local)
    return models


def unit(vector):
    length = np.linalg.norm(vector)
    if not np.isfinite(length) or length < 1e-8:
        raise ValueError("Degenerate calibration direction")
    return vector / length


def direction(models, indices, first, second):
    return unit(models[indices[second]][:3, 3] - models[indices[first]][:3, 3])


def angle(first, second):
    # atan2 avoids acos losing precision around the exact aligned pose.
    return float(np.degrees(np.arctan2(np.linalg.norm(np.cross(first, second)), first @ second)))


def palm_frame(models, indices, hand, middle, index, pinky):
    forward = direction(models, indices, hand, middle)
    lateral = direction(models, indices, pinky, index)
    lateral = unit(lateral - forward * (lateral @ forward))
    return np.column_stack((lateral, unit(np.cross(forward, lateral)), forward))


def calibrate_profile(profile, source, target):
    if profile["sourceSkeleton"] != source["skeleton"] or profile["targetSkeleton"] != target["skeleton"]:
        raise ValueError("Calibration skeleton identity mismatch")
    if profile.get("sourcePose", {}).get("rotationOffsets", []):
        raise ValueError("Female calibration expects the GASP bind reference")
    source_joints, target_joints = source["joints"], target["joints"]
    source_indices = {j["name"]: i for i, j in enumerate(source_joints)}
    target_indices = {j["name"]: i for i, j in enumerate(target_joints)}
    source_locals = [np.asarray(j["restLocal"], dtype=float).reshape(4, 4) for j in source_joints]
    target_bind = [np.asarray(j["restLocal"], dtype=float).reshape(4, 4) for j in target_joints]
    target_locals = [local.copy() for local in target_bind]
    source_models = model_matrices(source_joints, source_locals)
    target_original = model_matrices(target_joints, target_bind)
    mappings = {p["target"]: p["source"] for p in profile["joints"]}
    segments = []
    for side in ("L", "R"):
        lower = side.lower()
        for a, b, ta, tb in (
            ("clavicle", "upperarm", "Scapula", "Shoulder"),
            ("upperarm", "lowerarm", "Shoulder", "Elbow"),
            ("lowerarm", "hand", "Elbow", "Wrist"),
            ("thigh", "calf", "Hip", "Knee"),
            ("calf", "foot", "Knee", "Ankle"),
            ("foot", "ball", "Ankle", "Toes"),
        ):
            mode = "preserve-footwear" if a == "foot" else "swing"
            segments.append((f"{a}_{lower}", f"{b}_{lower}", f"{ta}_{side}", f"{tb}_{side}", side, mode))
        segments.append((f"hand_{lower}", f"middle_01_{lower}", f"Wrist_{side}",
                         f"MiddleFinger2_{side}", side, "palm"))
    segments.sort(key=lambda segment: target_indices[segment[2]])
    offsets, measurements = [], []
    for a, b, ta, tb, side, mode in segments:
        if mappings.get(ta) != a:
            raise ValueError(f"Missing calibration joint mapping: {a} -> {ta}")
        models = model_matrices(target_joints, target_locals)
        source_direction = direction(source_models, source_indices, a, b)
        current_direction = direction(models, target_indices, ta, tb)
        index = target_indices[ta]
        if mode == "preserve-footwear":
            # Female 高跟鞋的踝趾倾角属于目标造型；盲目匹配平底 mannequin 会把鞋尖翘起。
            # 保留目标脚模型空间参考朝向，仅抵消祖先腿链校准；动作旋转增量仍由 runtime 传递。
            correction = (Rotation.from_matrix(target_original[index][:3, :3]) *
                          Rotation.from_matrix(models[index][:3, :3]).inv())
        elif mode == "palm":
            lower = side.lower()
            source_frame = palm_frame(source_models, source_indices, a, b,
                                      f"index_01_{lower}", f"pinky_01_{lower}")
            target_frame = palm_frame(models, target_indices, ta, tb,
                                      f"IndexFinger2_{side}", f"PinkyFinger2_{side}")
            correction = Rotation.from_matrix(source_frame @ target_frame.T)
        else:
            # 单向量使用最短 swing；不声称仅凭骨段方向就能恢复作者的轴向 twist。
            correction, _ = Rotation.align_vectors([source_direction], [current_direction])
        parent = target_joints[index]["parent"]
        parent_rotation = Rotation.from_matrix(models[parent][:3, :3])
        desired_model = correction * Rotation.from_matrix(models[index][:3, :3])
        desired_local = parent_rotation.inv() * desired_model
        offset = Rotation.from_matrix(target_bind[index][:3, :3]).inv() * desired_local
        # 只改工作副本旋转；平移、缩放和正式 bind/inverse bind 不写回。
        scales = np.linalg.norm(target_bind[index][:3, :3], axis=0)
        target_locals[index][:3, :3] = desired_local.as_matrix() @ np.diag(scales)
        offsets.append({"joint": ta, "rotation": offset.as_quat().tolist()})
        measurements.append({"source": [a, b], "target": [ta, tb], "method": mode,
                             "beforeDegrees": angle(source_direction, direction(
                                 target_original, target_indices, ta, tb))})
    final_models = model_matrices(target_joints, target_locals)
    for measurement in measurements:
        a, b = measurement["source"]
        ta, tb = measurement["target"]
        measurement["afterDegrees"] = angle(direction(source_models, source_indices, a, b),
                                            direction(final_models, target_indices, ta, tb))
        if measurement["method"] == "preserve-footwear":
            index = target_indices[ta]
            measurement["footReferenceRotationErrorDegrees"] = float(np.degrees(
                (Rotation.from_matrix(target_original[index][:3, :3]).inv() *
                 Rotation.from_matrix(final_models[index][:3, :3])).magnitude()))
    maximum = max(m["afterDegrees"] for m in measurements if m["method"] != "preserve-footwear")
    if maximum > 0.001:
        raise ValueError(f"Calibration direction residual too large: {maximum}")
    result = copy.deepcopy(profile)
    result["sourcePose"] = {"name": "GaspBind", "rotationOffsets": []}
    result["targetPose"] = {"name": "FemaleAlignedToGasp", "rotationOffsets": offsets}
    report = {"method": "Parent-first shortest swing; wrist palm frame; preserve Female footwear pitch",
              "rotationConvention": "xyzw; calibratedLocalRotation = bindLocalRotation * offset",
              "calibratedJoints": len(offsets), "segments": measurements,
              "maxDirectionErrorDegrees": maximum,
              "limits": "Limb axial twist is not solved; no pelvis translation, IK or foot contact",
              "sourceSkeletonSha256": hashlib.sha256(json.dumps(source, sort_keys=True).encode()).hexdigest(),
              "targetSkeletonSha256": hashlib.sha256(json.dumps(target, sort_keys=True).encode()).hexdigest()}
    return result, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root, output = args.resource_root.resolve(), args.output.resolve()
    report_path = output.with_suffix(".report.json")
    if output.exists() or report_path.exists():
        raise ValueError("Calibration output exists; refusing to overwrite")
    profile = json.loads((root / MAP / "Retarget/RT_Female_From_GASP.json").read_text(encoding="utf-8"))
    source = json.loads((root / "Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json").read_text(encoding="utf-8"))
    target = json.loads((root / MAP / "Skeletons/SK_Female_Outside.json").read_text(encoding="utf-8"))
    result, report = calibrate_profile(profile, source, target)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=4) + "\n", encoding="utf-8")
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
