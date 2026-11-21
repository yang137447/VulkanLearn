"""Regenerate soma_golden.json with NVIDIA's official MotionBricks Warp kernels.

This script is not part of the runtime or CTest. It exists so the C++ SOMA math
core can be checked against the upstream implementation instead of a second
hand-written reference. See README.md in this directory for provenance.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np


def axis_quat(warp, x: float, y: float, z: float, angle: float):
    direction = np.array([x, y, z], dtype=np.float64)
    direction /= np.linalg.norm(direction)
    return warp.quat_from_axis_angle(
        warp.vec3(float(direction[0]), float(direction[1]), float(direction[2])),
        float(angle),
    )


def transform_np(position, rotation):
    return [
        float(position[0]),
        float(position[1]),
        float(position[2]),
        float(rotation[0]),
        float(rotation[1]),
        float(rotation[2]),
        float(rotation[3]),
    ]


def make_skeleton(warp, joint_count: int):
    parents = [
        -1, 0, 1, 2, 3, 4, 5, 6, 6, 6,
        3, 10, 11, 12, 13, 13,
        3, 16, 17, 18, 19, 19,
        0, 22, 23, 24,
        0, 26, 27, 28,
    ]
    reference = []
    for joint in range(joint_count):
        position = (0.04 * joint, 0.15 + 0.02 * joint, -0.03 * joint)
        rotation = axis_quat(warp, 0.1 + 0.01 * joint, 1.0, 0.2, 0.015 * joint)
        reference.append(transform_np(position, rotation))
    return parents, np.array(reference, dtype=np.float32)


def make_pose(warp, joint_count: int, frame: int, base_x: float, base_z: float,
    joint_scale: float):
    pose = []
    for joint in range(joint_count):
        position = (
            0.04 * joint + 0.01 * frame,
            0.15 + 0.02 * joint,
            -0.03 * joint - 0.005 * frame,
        )
        rotation = axis_quat(
            warp,
            0.2 + 0.01 * joint,
            1.0,
            0.1 + 0.005 * joint,
            joint_scale * (joint + 1) * (frame + 1),
        )
        pose.append(transform_np(position, rotation))
    pose[0] = transform_np(
        (base_x, 0.9, base_z),
        axis_quat(warp, 0.0, 1.0, 0.0, 0.05 * frame),
    )
    return pose


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--motionbricks-root", required=True, type=Path,
        help="Checkout containing the motionbricks package")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    root = args.motionbricks_root.resolve()
    manifest = root / "motionbricks/assets/onnx/soma/soma_n1.json"

    import sys
    sys.path.insert(0, str(root))

    import warp as wp
    from motionbricks.model.contract import load_model_manifest
    from motionbricks.model.kernels import write_motionbricks_conditions_kernel
    from motionbricks.model.soma.kernels import (
        apply_endpoint_deltas_kernel,
        compute_endpoint_deltas_kernel,
        compute_guide_alignment_kernel,
        decode_motionbricks_output_kernel,
        encode_motionbricks_inputs_kernel,
    )
    import motionbricks.utils.pose_utils as pose_utils
    import motionbricks.utils.time_utils as time_utils

    wp.set_device("cpu")
    contract, _, _ = load_model_manifest(manifest)
    names = list(contract.joint_names)
    joint_count = len(names)
    if joint_count != 30:
        raise RuntimeError(f"Expected 30 SOMA joints, got {joint_count}")

    parents, reference = make_skeleton(wp, joint_count)
    context = [make_pose(wp, joint_count, frame, 0.1 * frame, 0.2 * frame, 0.01)
        for frame in range(4)]
    target = [make_pose(wp, joint_count, 0, 0.8 + 0.1 * frame, 1.2 + 0.2 * frame, 0.02)
        for frame in range(2)]
    context_np = np.array(context, dtype=np.float32)
    target_np = np.array(target, dtype=np.float32)
    target_padded = np.concatenate(
        [target_np, np.repeat(target_np[-1:], 4 - len(target_np), axis=0)], axis=0)
    guide_np = np.concatenate([context_np, target_padded], axis=0)

    parent_wp = wp.array(np.array(parents, dtype=np.int32), dtype=wp.int32)
    reference_wp = wp.array(reference, dtype=wp.transform)
    guide_wp = wp.array(guide_np, dtype=wp.transform)
    context_wp = wp.array(context_np, dtype=wp.transform)
    target_wp = wp.array(target_np, dtype=wp.transform)

    model_to_runtime = wp.transform_identity()
    runtime_to_model = wp.transform_inverse(model_to_runtime)
    world = wp.empty((8, joint_count), dtype=wp.transform)
    pose_utils.compute_world_poses(guide_wp, parent_wp, world, root_tx=runtime_to_model,
        num_poses=8)

    alignment = wp.empty(1, dtype=wp.transform)
    right_leg = names.index("RightLeg")
    left_leg = names.index("LeftLeg")
    wp.launch(
        compute_guide_alignment_kernel,
        dim=1,
        inputs=[right_leg, left_leg, wp.vec2(1.0, 0.0), world],
        outputs=[alignment],
    )

    model_to_pose_wp = wp.array(np.arange(joint_count, dtype=np.int32), dtype=wp.int32)
    pose_to_model_wp = wp.array(np.arange(joint_count, dtype=np.int32), dtype=wp.int32)
    global_root = wp.zeros((1, 8, 5), dtype=wp.float32)
    local_root = wp.zeros((1, 8, 4), dtype=wp.float32)
    local_poses = wp.zeros((1, 8, 267), dtype=wp.float32)
    wp.launch(
        encode_motionbricks_inputs_kernel,
        dim=8,
        inputs=[joint_count, right_leg, left_leg, model_to_pose_wp, world, alignment],
        outputs=[global_root, local_root, local_poses],
    )

    allowed_counts = [8, 9, 10, 11, 12, 13, 14]
    allowed_bits = 0
    for value in allowed_counts:
        allowed_bits |= 1 << (value - 6)
    has_global = wp.zeros((1, 8), dtype=wp.int32)
    has_local_root = wp.zeros((1, 8), dtype=wp.int32)
    has_local_poses = wp.zeros((1, 8), dtype=wp.int32)
    allowed = wp.zeros((1, 11), dtype=wp.int32)
    wp.launch(
        write_motionbricks_conditions_kernel,
        dim=11,
        inputs=[len(target), allowed_bits],
        outputs=[has_global, has_local_root, has_local_poses, allowed],
    )

    global_np = global_root.numpy()[0].copy()
    local_root_np = local_root.numpy()[0].copy()
    local_poses_np = local_poses.numpy()[0].copy()
    alignment_np = alignment.numpy()[0].copy()

    rotation_offset = 5 + (joint_count - 1) * 3
    prediction = np.zeros((1, 64, 366), dtype=np.float32)
    for raw in range(64):
        prediction[0, raw, 0:3] = global_np[0, 0:3] + np.array(
            [0.01 * raw, 0.0, 0.0], dtype=np.float32)
        prediction[0, raw, rotation_offset:rotation_offset + 180] = local_poses_np[0][87:267]
        prediction[0, raw, 362:366] = np.array([0.1, 0.2, 0.3, 0.4], dtype=np.float32)

    token_count = 6
    prediction_wp = wp.array(prediction, dtype=wp.float32)
    token_wp = wp.array(np.array([token_count], dtype=np.int32), dtype=wp.int32)
    output_poses = wp.zeros((58, joint_count), dtype=wp.transform)
    output_contacts = wp.zeros((58, 4), dtype=wp.float32)
    output_count = wp.zeros(1, dtype=wp.int32)
    wp.launch(
        decode_motionbricks_output_kernel,
        dim=58,
        inputs=[joint_count, joint_count, parent_wp, pose_to_model_wp, reference_wp,
            alignment, model_to_runtime, prediction_wp, token_wp],
        outputs=[output_poses, output_contacts, output_count],
    )
    frame_count = int(output_count.numpy()[0])
    decoded = output_poses.numpy()[:frame_count].copy()
    decoded_contacts = output_contacts.numpy()[:frame_count].copy()

    start = wp.empty(joint_count, dtype=wp.transform)
    end = wp.empty(joint_count, dtype=wp.transform)
    wp.launch(
        compute_endpoint_deltas_kernel,
        dim=joint_count,
        inputs=[output_count, context_wp, target_wp, output_poses],
        outputs=[start, end],
    )
    wp.launch(
        apply_endpoint_deltas_kernel,
        dim=(58, joint_count),
        inputs=[output_count, start, end],
        outputs=[output_poses],
    )
    corrected = output_poses.numpy()[:frame_count].copy()

    sample_time = 0.35
    frame, next_frame, blend = time_utils.frame_index_from_time(
        sample_time, 30.0, frame_count)
    sampled = wp.empty(joint_count, dtype=wp.transform)
    wp.launch(
        pose_utils.blend_pose_joints_with_root_offset_kernel,
        dim=joint_count,
        inputs=[output_poses, frame, next_frame, blend, wp.transform_identity()],
        outputs=[sampled],
    )

    def values(array):
        return np.asarray(array, dtype=np.float32).tolist()

    fixture = {
        "source": "NVlabs/motionbricks v0.2.0, Apache-2.0",
        "manifest": manifest.name,
        "joint_names": names,
        "parents": parents,
        "reference": values(reference),
        "context": values(context_np),
        "target": values(target_np),
        "target_poses_used": len(target),
        "allowed_token_counts": allowed_counts,
        "heading_alignment": [1.0, 0.0],
        "model_to_runtime": transform_np((0, 0, 0), (0, 0, 0, 1)),
        "encoded": {
            "alignment": values(alignment_np),
            "global_root_values": values(global_np),
            "local_root_values": values(local_root_np),
            "local_poses": values(local_poses_np),
            "has_global_root_values": has_global.numpy()[0].copy().tolist(),
            "has_local_root_values": has_local_root.numpy()[0].copy().tolist(),
            "has_local_poses": has_local_poses.numpy()[0].copy().tolist(),
            "allowed_pred_num_tokens": allowed.numpy()[0].copy().tolist(),
        },
        "prediction": {
            "token_count": token_count,
            "features": values(prediction[0, 0:24]),
        },
        "decoded": {
            "frame_count": frame_count,
            "frames": values(decoded),
            "foot_contacts": values(decoded_contacts),
        },
        "corrected": {
            "first": values(corrected[0]),
            "middle": values(corrected[frame_count // 2]),
            "last": values(corrected[-1]),
        },
        "sampled": {
            "time": sample_time,
            "frame": int(frame),
            "next_frame": int(next_frame),
            "blend": float(blend),
            "pose": values(sampled.numpy().copy()),
        },
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(fixture, indent=1), encoding="utf-8")
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
