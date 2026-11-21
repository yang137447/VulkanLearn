import argparse
import json
import sys
from pathlib import Path

import numpy as np


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--motionbricks-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.motionbricks_root.resolve()))
    import warp as wp
    from app.runtime.common.io.smart_primitive_document import load_smart_primitive_document
    from app.runtime.common.clip_registry import ClipRegistry
    from motionbricks.model.contract import ModelRig
    from motionbricks.animation.skeleton import Skeleton
    from motionbricks.utils.space_conversion_utils import SpaceConverter, FacingDirectionType
    from scipy.spatial.transform import Rotation

    wp.init()
    with wp.ScopedDevice("cpu"):
        source = json.loads((Path(__file__).resolve().parents[1] /
            "animation-tests/fixtures/motionbricks/SK_soma.json").read_text(encoding="utf-8"))
        joints = source["joints"]
        transforms = []
        for joint in joints:
            matrix = np.asarray(joint["restLocal"]).reshape(4, 4)
            transforms.append([*(matrix[:3, 3] / 100), *Rotation.from_matrix(matrix[:3, :3]).as_quat()])
        skeleton = Skeleton([joint["name"] for joint in joints],
                            [joint["parent"] for joint in joints], transforms)
        registry = ClipRegistry(ModelRig.SOMA, skeleton=skeleton)
        conversion = SpaceConverter(FacingDirectionType.MUJOCO).convert_transform(wp.transform_identity())
        path = args.motionbricks_root / "assets/smart_primitives/soma/locomotion_bs1_linear.smart_primitive.json"
        document, _, _ = load_smart_primitive_document(
            registry, skeleton.reference_local_transforms, conversion, path)
        entries = []
        speeds = {"Idle": 0.0, "Walk": 2.3, "Jog": 3.0, "Run": 4.5}
        for node, component in document.iter_target_poses():
            poses = component.sampled_poses.numpy()
            if not np.isfinite(poses).all():
                raise ValueError("Nonfinite target pose")
            entries.append({"name": node.name, "speed": speeds[node.name],
                            "poses": poses.tolist()})
        output = {"format": "vl.motionbricks.locomotion.v1",
                  "source": "NVIDIA MotionBricks v0.2.0 locomotion_bs1_linear",
                  "jointNames": skeleton.joint_names,
                  "targets": entries}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(output, separators=(",", ":")), encoding="utf-8")
        print(f"Exported {len(entries)} targets to {args.output}")


if __name__ == "__main__":
    main()
