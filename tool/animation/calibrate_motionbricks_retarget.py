import argparse
import json
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--resource-root", type=Path, required=True)
    parser.add_argument("--target-skeleton", default="Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json")
    parser.add_argument("--profile", default="Maps/SC_motionbricks/Retarget/RT_UEFN_From_SOMA.json")
    parser.add_argument("--pose-name", default="SOMA_AlignedToUEFNApose")
    args = parser.parse_args()
    resources = args.resource_root
    source = json.loads((resources / "Maps/SC_motionbricks/Skeletons/SK_soma.json").read_text(encoding="utf-8"))["joints"]
    target = json.loads((resources / args.target_skeleton).read_text(encoding="utf-8"))["joints"]
    source_names = {joint["name"]: index for index, joint in enumerate(source)}
    target_names = {joint["name"]: index for index, joint in enumerate(target)}
    locals_source = [np.asarray(joint["restLocal"]).reshape(4, 4).copy() for joint in source]

    def globals_for(joints, locals_list):
        result = []
        for joint, local in zip(joints, locals_list):
            result.append(local if joint["parent"] < 0 else result[joint["parent"]] @ local)
        return result

    target_world = globals_for(target, [np.asarray(joint["restLocal"]).reshape(4, 4) for joint in target])
    offsets = []
    pairs = []
    for side, suffix in (("Left", "l"), ("Right", "r")):
        pairs += [(side + "Shoulder", side + "Arm", "clavicle_" + suffix, "upperarm_" + suffix),
                  (side + "Arm", side + "ForeArm", "upperarm_" + suffix, "lowerarm_" + suffix),
                  (side + "ForeArm", side + "Hand", "lowerarm_" + suffix, "hand_" + suffix),
                  (side + "Leg", side + "Shin", "thigh_" + suffix, "calf_" + suffix),
                  (side + "Shin", side + "Foot", "calf_" + suffix, "foot_" + suffix),
                  (side + "Foot", side + "ToeBase", "foot_" + suffix, "ball_" + suffix)]
    for source_name, source_child, target_name, target_child in pairs:
        world = globals_for(source, locals_source)
        index = source_names[source_name]
        current = world[source_names[source_child]][:3, 3] - world[index][:3, 3]
        desired = target_world[target_names[target_child]][:3, 3] - target_world[target_names[target_name]][:3, 3]
        current /= np.linalg.norm(current)
        desired /= np.linalg.norm(desired)
        axis = np.cross(current, desired)
        axis_length = np.linalg.norm(axis)
        if axis_length < 1e-8:
            if np.dot(current, desired) < 0:
                raise ValueError(f"Opposed calibration bone: {source_name}")
            rotation = np.eye(3)
        else:
            rotation = Rotation.from_rotvec(axis / axis_length *
                np.arctan2(axis_length, np.dot(current, desired))).as_matrix()
        parent = source[index]["parent"]
        parent_rotation = np.eye(3) if parent < 0 else world[parent][:3, :3]
        local_rotation = parent_rotation.T @ rotation @ world[index][:3, :3]
        rest_rotation = np.asarray(source[index]["restLocal"]).reshape(4, 4)[:3, :3]
        offset = Rotation.from_matrix(rest_rotation.T @ local_rotation).as_quat()
        locals_source[index][:3, :3] = local_rotation
        offsets.append({"joint": source_name, "rotation": offset.tolist()})
    path = resources / args.profile
    profile = json.loads(path.read_text(encoding="utf-8"))
    profile["sourcePose"] = {"name": args.pose_name, "rotationOffsets": offsets}
    path.write_text(json.dumps(profile, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
