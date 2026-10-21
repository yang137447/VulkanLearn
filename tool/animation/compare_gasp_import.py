"""Compare native ozz import output against UE RAW references in VL centimeters.

Requires numpy/scipy, plus matplotlib only for --figure. Does not load Vulkan.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation

from compare_gasp_export import read, source_matrix


def errors(actual, expected):
    actual = np.asarray(actual)
    position = float(np.linalg.norm(actual[:3, 3] - expected[:3, 3]))
    actual_rotation = actual[:3, :3] / np.linalg.norm(actual[:3, :3], axis=0)
    expected_rotation = expected[:3, :3] / np.linalg.norm(expected[:3, :3], axis=0)
    angle = float(np.degrees(Rotation.from_matrix(
        expected_rotation.T @ actual_rotation).magnitude()))
    return position, angle


def compare(audit_path, imported):
    # Fixed published policy, not an error-minimizing fitted basis.
    basis = np.array([[1, 0, 0, 0], [0, 0, 1, 0],
                      [0, 1, 0, 0], [0, 0, 0, 1]])
    names = [joint["name"] for joint in imported["joints"]]
    if len(set(names)) != len(names):
        raise ValueError("Duplicate imported joint name")
    source_bones = {bone["name"]: bone for bone in read(audit_path / "skeleton.json")["bones"]
                    if not bone["name"].startswith("VB ")}
    if set(names) != set(source_bones):
        raise ValueError("Physical skeleton differs from the UE reference")
    mesh_errors = []
    for joint in imported["joints"]:
        expected = basis @ source_matrix(source_bones[joint["name"]]["referenceModel"]) @ basis.T
        mesh_errors.append(errors(joint["restModel"], expected))
        parent_index = joint["parent"]
        expected_parent = source_bones[joint["name"]]["parent"]
        if (names[parent_index] if parent_index >= 0 else "None") != expected_parent:
            raise ValueError("Imported skeleton parent mismatch")
    report = {
        "format": "vl.gasp.cpu-import-comparison.v1",
        "inputBlake3": imported["inputBlake3"], "policy": imported["policy"],
        "positionToleranceCm": 0.1, "rotationToleranceDegrees": 0.1,
        "durationToleranceSeconds": 0.00001,
        "maxReferencePositionErrorCm": max(item[0] for item in mesh_errors),
        "maxReferenceRotationErrorDegrees": max(item[1] for item in mesh_errors),
        "sections": imported["sections"], "clips": [],
    }
    seen = set()
    for clip in imported["clips"]:
        if clip["stem"] in seen:
            raise ValueError("Duplicate imported clip")
        seen.add(clip["stem"])
        source = read(audit_path / (clip["stem"] + ".json"))
        if len(clip["frames"]) != len(source["referenceFrames"]):
            raise ValueError("Missing reference frames")
        frame_errors = []
        for frame, reference in zip(clip["frames"], source["referenceFrames"]):
            if frame["time"] != reference["time"] or len(frame["models"]) != len(names):
                raise ValueError("Wrong reference frame time/joint count")
            actual = dict(zip(names, frame["models"]))
            for bone in reference["bones"]:
                if bone["name"].startswith("VB "):
                    continue
                expected = basis @ source_matrix(bone["model"]) @ basis.T
                position, angle = errors(actual[bone["name"]], expected)
                frame_errors.append({"time": frame["time"], "bone": bone["name"],
                                     "positionErrorCm": position,
                                     "rotationErrorDegrees": angle})
        if len(clip["rootSamples"]) != len(source["rawRootSamples"]):
            raise ValueError("Missing root samples")
        root_errors = []
        for sample, reference in zip(clip["rootSamples"], source["rawRootSamples"]):
            if sample["time"] != reference["time"]:
                raise ValueError("Root sample time mismatch")
            expected = basis @ source_matrix(reference["transform"]) @ basis.T
            root_errors.append(errors(sample["model"], expected))
        metadata = clip["metadata"]
        for key in ("floatCurveKeys", "notifies", "syncMarkers", "settings", "rateScale", "curveLimit"):
            if metadata[key] != source[key]:
                raise ValueError("Metadata was altered: " + key)
        expected_fallback = set(names) - set(source["tracks"])
        if set(clip["restFallbackJoints"]) != expected_fallback:
            raise ValueError("Unexpected missing/fallback tracks")
        report["clips"].append({
            "stem": clip["stem"], "durationErrorSeconds": abs(clip["duration"] - source["duration"]),
            "maxPositionErrorCm": max(item["positionErrorCm"] for item in frame_errors),
            "maxRotationErrorDegrees": max(item["rotationErrorDegrees"] for item in frame_errors),
            "maxRootPositionErrorCm": max(item[0] for item in root_errors),
            "maxRootRotationErrorDegrees": max(item[1] for item in root_errors),
            "checkedBonePoses": len(frame_errors), "checkedRootSamples": len(root_errors),
            "restFallbackJoints": clip["restFallbackJoints"], "metadataPreserved": True,
            "frames": frame_errors,
        })
    report["passed"] = (
        len(seen) == 12 and
        report["maxReferencePositionErrorCm"] <= report["positionToleranceCm"] and
        report["maxReferenceRotationErrorDegrees"] <= report["rotationToleranceDegrees"] and
        all(section["maxBindSkinErrorCm"] <= 0.1 and
            section["maxBindPositionErrorCm"] <= 0.1 and
            section["maxBindLinearError"] <= 0.0001 and
            section["maxWeightSumError"] <= 0.0001 for section in report["sections"]) and
        all(clip["maxPositionErrorCm"] <= report["positionToleranceCm"] and
            clip["maxRootPositionErrorCm"] <= report["positionToleranceCm"] and
            clip["maxRotationErrorDegrees"] <= report["rotationToleranceDegrees"] and
            clip["maxRootRotationErrorDegrees"] <= report["rotationToleranceDegrees"] and
            clip["durationErrorSeconds"] <= report["durationToleranceSeconds"]
            for clip in report["clips"])
    )
    report["scope"] = "CPU import/sampled poses and metadata preservation; no rendered character"
    return report


def figure(imported, report, output):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    walk = next(clip for clip in imported["clips"] if clip["stem"] == "M_Neutral_Walk_Loop_F")
    turn = next(clip for clip in imported["clips"] if clip["stem"] == "M_Neutral_Stand_Turn_180_L")
    fig, axes = plt.subplots(1, 3, figsize=(14, 5))
    times = [sample["time"] for sample in walk["rootSamples"]]
    positions = [sample["model"][2][3] for sample in walk["rootSamples"]]
    axes[0].plot(times, positions, color="#167b70")
    axes[0].set(xlabel="Time (s)", ylabel="Forward +Z (cm)", title="Walk root retained")
    axes[0].grid(alpha=0.2)
    joints = imported["joints"]
    for axis, clip, label in zip(axes[1:], (walk, turn), ("Walk t=1s", "Turn L midpoint")):
        frame = clip["frames"][1 if clip is walk else 2]
        models = np.array(frame["models"])
        points = models[:, :3, 3] - models[0, :3, 3]
        for joint, point in zip(joints, points):
            if joint["parent"] < 0 or joint["name"].startswith(("ik_", "prop", "poi", "attach")):
                continue
            pair = np.array([points[joint["parent"]], point])
            axis.plot(pair[:, 2], pair[:, 1], color="#b63764", linewidth=1)
        axis.set(xlabel="Forward Z (cm)", ylabel="Up Y (cm)", title=label)
        axis.set_aspect("equal")
        axis.grid(alpha=0.2)
    position_error = max(clip["maxPositionErrorCm"] for clip in report["clips"])
    fig.suptitle(f'VL CPU import: {len(joints)} joints / 12 clips / '
                 f'max UE pose error {position_error:.4f} cm')
    fig.text(0.5, 0.025, "Native importer + ozz numerical evidence. No Vulkan rendering.",
             ha="center", fontsize=10)
    fig.tight_layout(rect=(0, 0.08, 1, 0.92))
    fig.savefig(output, dpi=150)
    plt.close(fig)


def main():
    import json

    parser = argparse.ArgumentParser()
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--imported", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--figure", type=Path)
    arguments = parser.parse_args()
    if arguments.output.exists() or (arguments.figure and arguments.figure.exists()):
        raise ValueError("Choose new output paths")
    imported = read(arguments.imported)
    report = compare(arguments.audit, imported)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if arguments.figure:
        figure(imported, report, arguments.figure)
    print(json.dumps({"passed": report["passed"], "clips": len(report["clips"]),
                      "maxPositionErrorCm": max(c["maxPositionErrorCm"] for c in report["clips"]),
                      "maxRotationErrorDegrees": max(c["maxRotationErrorDegrees"] for c in report["clips"]),
                      "maxRootPositionErrorCm": max(c["maxRootPositionErrorCm"] for c in report["clips"])},
                     indent=2))
    raise SystemExit(0 if report["passed"] else 1)


if __name__ == "__main__":
    main()
