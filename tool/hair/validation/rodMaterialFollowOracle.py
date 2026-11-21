import argparse
import hashlib
import json
from pathlib import Path
import struct

import numpy as np


def normalize(values):
    return values / np.linalg.norm(values, axis=-1, keepdims=True)


def rotate(quaternion, vector):
    quaternion = normalize(quaternion)
    axis = quaternion[..., :3]
    return vector + 2 * np.cross(axis, np.cross(axis, vector) + quaternion[..., 3:] * vector)


def point_frames(segment_frames):
    local = np.arange(32)
    first = segment_frames[:, np.maximum(local - 1, 0)]
    last = segment_frames[:, np.minimum(local, 30)]
    last = np.where((first * last).sum(axis=-1, keepdims=True) < 0, -last, last)
    return normalize(first + last)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--snapshot", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--binding-prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    metadata = json.loads(Path(str(options.snapshot) + ".json").read_text(encoding="utf-8"))
    raw = options.source.read_bytes()
    version, count, points, offset = struct.unpack_from("<fIII", raw)
    assert version >= 4 and count == 40334 and points == 32
    authored = np.frombuffer(raw, dtype="<f4", count=count * points * 4, offset=offset).reshape(count, points, 4)[..., :3].astype(np.float64)
    current = np.fromfile(options.snapshot, dtype="<f4").reshape(count, points, 4)[..., :3].astype(np.float64)
    prefix = str(options.binding_prefix)
    selected = np.fromfile(prefix + ".selected.bin", dtype="<u4")
    parents = np.fromfile(prefix + ".parents.bin", dtype="<u4")
    assert np.array_equal(selected, metadata["simulationSourceGuideIds"])
    assert len(selected) == 2000 and len(parents) == count
    state = np.fromfile(str(options.snapshot) + ".rod-state.bin", dtype="<f4").reshape(2000, 412, 4).astype(np.float64)
    references = np.fromfile(str(options.snapshot) + ".rod-refs.bin", dtype="<f4").reshape(2000, 96, 4).astype(np.float64)
    point_state = state[:, :160].reshape(2000, 32, 5, 4)
    segment_state = state[:, 160:408].reshape(2000, 31, 8, 4)
    current_frames = point_frames(segment_state[:, :, 0])
    rest_frames = point_frames(segment_state[:, :, 2])
    inverse_rest = rest_frames.copy()
    inverse_rest[..., :3] *= -1
    source_world = references[:, :32, :3] + point_state[:, :, 0, :3] + np.asarray(metadata["fixedWorldOriginSI"])
    slots = np.searchsorted(selected, parents)
    assert np.array_equal(selected[slots], parents)
    offset_si = (authored - authored[parents]) * .01
    material_offset = rotate(inverse_rest[slots], offset_si)
    rotated_offset = rotate(current_frames[slots], material_offset)
    predicted_world = source_world[slots] + rotated_offset
    hair_model = np.asarray(metadata["hairModelColumnMajor"]).reshape(4, 4, order="F")
    world_to_hair = np.linalg.inv(hair_model)
    predicted = predicted_world * 100 @ world_to_hair[:3, :3].T + world_to_hair[:3, 3]
    error_cm = np.linalg.norm(predicted - current, axis=-1)
    lengths = np.linalg.norm(np.diff(authored, axis=1), axis=-1).sum(axis=1)
    displacement = np.linalg.norm(current - authored, axis=-1)
    maximum = displacement.max(axis=1)
    amplified = np.linalg.norm(rotated_offset - offset_si, axis=-1)
    selected_mask = np.zeros(count, dtype=bool)
    selected_mask[selected] = True
    exceed = (~selected_mask) & (maximum > .015 * lengths)
    ranked = np.argsort(maximum / lengths)[::-1]
    speeds = np.linalg.norm(point_state[:, :, 1, :3], axis=-1)
    angular_speeds = np.linalg.norm(segment_state[:, :, 1, :3], axis=-1)
    iterations, counts = np.unique(state[:, 408, 3].astype(np.int32), return_counts=True)
    report = {
        "scope": "independent float64 CPU reconstruction using actual deformed GPU material frames and actual CPU imported binding; not an independent CPU dynamics trajectory",
        "sourceSha256": hashlib.sha256(raw).hexdigest(),
        "snapshotSha256": hashlib.sha256(options.snapshot.read_bytes()).hexdigest(),
        "guideCount": count, "actualDistinctSimulationGuides": len(selected),
        "currentBufferAcceptedSecondsMin": metadata.get("currentBufferAcceptedSecondsMin"),
        "currentBufferAcceptedSecondsMax": metadata.get("currentBufferAcceptedSecondsMax"),
        "maxReconstructionErrorCm": float(error_cm.max()),
        "maxFollowReconstructionErrorCm": float(error_cm[~selected_mask].max()),
        "maxGuideReconstructionErrorCm": float(error_cm[selected_mask].max()),
        "maxPointSpeedMPerSecond": float(speeds.max()),
        "rmsPointSpeedMPerSecond": float(np.sqrt(np.mean(speeds * speeds))),
        "maxBodyAngularSpeedRadPerSecond": float(angular_speeds.max()),
        "lastDispatchNewtonIterationHistogram": {str(int(key)): int(value) for key, value in zip(iterations, counts)},
        "maxMaterialRotationOffsetDisplacementCm": float(amplified.max() * 100),
        "followCountExceedingGuideStaticYardstick": int(exceed.sum()),
        "yardstickScope": "reported .015*individual follow length, not a preregistered whole-head acceptance requirement",
        "maxFollowDisplacementOverOwnLength": float((maximum / lengths)[~selected_mask].max()),
        "largestFollowAmplifications": [
            {"sourceId": int(guide), "parentSourceId": int(parents[guide]),
             "dmaxOverOwnLength": float(maximum[guide] / lengths[guide]),
             "maxOffsetRotationCm": float(amplified[guide].max() * 100),
             "maxAuthoredOffsetCm": float(np.linalg.norm(offset_si[guide], axis=-1).max() * 100),
             "parentDmaxCm": float(displacement[parents[guide]].max()),
             "reconstructionErrorCm": float(error_cm[guide].max())}
            for guide in ranked if not selected_mask[guide]][:30],
        "qualityPass": False,
        "limitations": [
            "Static bind, identity hair-model rotation scope; animated same-pose and model-rotation oracles remain separate.",
            "GPU q and u are measured inputs; this does not substitute a CPU double dynamics comparison.",
            "No director clamp, twist deletion or binding change is applied."
        ]
    }
    options.output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in (
        "maxReconstructionErrorCm", "maxMaterialRotationOffsetDisplacementCm",
        "followCountExceedingGuideStaticYardstick", "maxFollowDisplacementOverOwnLength")}))


if __name__ == "__main__":
    main()
