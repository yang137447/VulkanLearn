"""Unit tests for tool/animation/extract_pose_driver.py."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import extract_pose_driver as epd  # noqa: E402


NODE_10 = r'''Node=(SourceBones=((BoneName="neck_02"),(BoneName="spine_03")),
PoseTargets=(
  (BoneTransforms=((TargetTranslation=(X=1,Y=2.5,Z=-3),TargetRotation=(Pitch=10,Yaw=-20.5,Roll=30)),(TargetTranslation=(X=4,Y=5,Z=6),TargetRotation=(Pitch=7,Yaw=8,Roll=9))),DrivenName="Curve_A,with(parens)",TargetScale=0.5,DistanceMethod=Quaternion,FunctionType=Cubic,bApplyCustomCurve=True),
  (BoneTransforms=((TargetTranslation=(X=1,Y=1,Z=1),TargetRotation=(Pitch=0,Yaw=0,Roll=0)),(TargetTranslation=(X=2,Y=2,Z=2),TargetRotation=(Pitch=1,Yaw=2,Roll=3))),DrivenName="Curve_\"quoted\"")
),RBFParams=(SolverType=LeastSquares,Radius=12.5,AutomaticRadius=True,Function=Gaussian,DistanceMethod=Euclidean,TwistAxis=Y,WeightThreshold=0.25,NormalizeMethod=NormalizeWithinMedian,MedianReference=(X=1,Y=2,Z=3),MedianMin=10,MedianMax=20),DriveSource=Translation,DriveOutput=DriveCurves,EvalSpaceBone=(BoneName="pelvis"),bEvalFromRefPose=True)'''

NODE_11 = '''Node=(SourceBones=((BoneName="head")),PoseTargets=((BoneTransforms=((TargetTranslation=(X=0,Y=0,Z=0),TargetRotation=(Pitch=0,Yaw=0,Roll=0))),DrivenName="Curve_Default")),RBFParams=(SolverType=Interpolative,Radius=30,Function=Linear),DriveOutput=DriveCurves)'''


def _template_block(name: str, node: str, *, with_node: bool) -> str:
    header = (
        f'      Begin Object Name="{name}" '
        f'ExportPath="/Script/AnimGraph.AnimGraphNode_PoseDriver\'{name}\'"\n'
    )
    body = (
        '         Begin Object Name="AnimGraphNodeBinding_Base_0" '
        'ExportPath="/Script/AnimGraph.AnimGraphNodeBinding_Base\'AnimGraphNodeBinding_Base_0\'"\n'
        "         End Object\n"
    )
    if with_node:
        body += f"         {node}\n"
    return header + body + "      End Object\n"


def _anim_graph_block(name: str) -> str:
    return (
        f'      Begin Object Class=/Script/AnimGraph.AnimGraphNode_PoseDriver '
        f'Name="{name}" ExportPath="/Script/AnimGraph.AnimGraphNode_PoseDriver\'{name}\'"\n'
        '         Begin Object Class=/Script/AnimGraph.AnimGraphNodeBinding_Base '
        'Name="AnimGraphNodeBinding_Base_0" ExportPath="/Script/AnimGraph.AnimGraphNodeBinding_Base\'x\'"\n'
        "         End Object\n"
        "      End Object\n"
    )


def build_t3d(nodes: dict[str, str]) -> str:
    text = (
        'Begin Object Class=/Script/Engine.AnimBlueprint Name="TestAnimBP" '
        'ExportPath="/Script/Engine.AnimBlueprint\'TestAnimBP\'"\n'
        '   Begin Object Class=/Script/AnimGraph.AnimationGraph Name="AnimGraph" '
        'ExportPath="/Script/AnimGraph.AnimationGraph\'AnimGraph\'"\n'
    )
    for full_name in nodes:
        text += _anim_graph_block(full_name)
    text += "   End Object\n"
    # Two ExecuteUbergraph copies, matching the real Echo export.
    for _ in range(2):
        for full_name, node in nodes.items():
            text += _template_block(full_name, node, with_node=True)
    text += "End Object\n"
    return text


class ParseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = build_t3d(
            {
                "AnimGraphNode_PoseDriver_10": NODE_10,
                "AnimGraphNode_PoseDriver_11": NODE_11,
            }
        )

    def test_bom_and_encoding_detection(self) -> None:
        self.assertEqual(epd.decode_t3d(self.text.encode("utf-16")), self.text)
        self.assertEqual(epd.decode_t3d(self.text.encode("utf-8")), self.text)

    def test_deduplicates_ubergraph_copies(self) -> None:
        document = epd.parse_pose_drivers(
            self.text,
            expected_counts={"PoseDriver_10": 2, "PoseDriver_11": 1},
            t3d_blake3="00" * 32,
        )
        self.assertEqual([d["name"] for d in document["drivers"]], ["PoseDriver_10", "PoseDriver_11"])
        self.assertEqual(document["format"], epd.FORMAT)
        self.assertEqual(document["source"]["t3dBlake3"], "00" * 32)

    def test_parses_nested_structs_and_explicit_values(self) -> None:
        document = epd.parse_pose_drivers(
            self.text,
            expected_counts={"PoseDriver_10": 2, "PoseDriver_11": 1},
        )
        driver = document["drivers"][0]
        self.assertEqual(driver["sourceBones"], ["neck_02", "spine_03"])
        self.assertEqual(driver["evalSpaceBone"], "pelvis")
        self.assertTrue(driver["evalFromRefPose"])
        self.assertEqual(driver["driveSource"], "Translation")
        self.assertEqual(driver["driveOutput"], "Curves")
        self.assertEqual(
            driver["rbf"],
            {
                "solverType": "LeastSquares",
                "radius": 12.5,
                "automaticRadius": True,
                "function": "Gaussian",
                "distanceMethod": "Euclidean",
                "twistAxis": "Y",
                "weightThreshold": 0.25,
                "normalizeMethod": "NormalizeWithinMedian",
                "medianReference": [1.0, 2.0, 3.0],
                "medianMin": 10.0,
                "medianMax": 20.0,
            },
        )
        self.assertEqual(len(driver["targets"]), 2)
        self.assertEqual(driver["targets"][0]["drivenName"], "Curve_A,with(parens)")
        self.assertEqual(driver["targets"][0]["scale"], 0.5)
        self.assertEqual(driver["targets"][0]["distanceMethod"], "Quaternion")
        self.assertEqual(driver["targets"][0]["function"], "Cubic")
        self.assertTrue(driver["targets"][0]["applyCustomCurve"])
        self.assertEqual(driver["targets"][1]["drivenName"], 'Curve_"quoted"')
        self.assertEqual(
            driver["targets"][0]["transforms"],
            [
                {
                    "translation": [1.0, 2.5, -3.0],
                    "rotation": {"pitch": 10.0, "yaw": -20.5, "roll": 30.0},
                },
                {
                    "translation": [4.0, 5.0, 6.0],
                    "rotation": {"pitch": 7.0, "yaw": 8.0, "roll": 9.0},
                },
            ],
        )

    def test_fills_cdo_defaults(self) -> None:
        document = epd.parse_pose_drivers(
            self.text,
            expected_counts={"PoseDriver_10": 2, "PoseDriver_11": 1},
        )
        driver = document["drivers"][1]
        self.assertEqual(driver["evalSpaceBone"], "")
        self.assertFalse(driver["evalFromRefPose"])
        self.assertEqual(driver["driveSource"], "Rotation")
        self.assertEqual(
            driver["rbf"],
            {
                "solverType": "Interpolative",
                "radius": 30.0,
                "automaticRadius": False,
                "function": "Linear",
                "distanceMethod": "SwingAngle",
                "twistAxis": "X",
                "weightThreshold": 0.0001,
                "normalizeMethod": "OnlyNormalizeAboveOne",
                "medianReference": [0.0, 0.0, 0.0],
                "medianMin": 45.0,
                "medianMax": 60.0,
            },
        )
        target = driver["targets"][0]
        self.assertEqual(target["scale"], 1.0)
        self.assertEqual(target["distanceMethod"], "Default")
        self.assertEqual(target["function"], "Default")
        self.assertFalse(target["applyCustomCurve"])

    def test_missing_transform_for_source_bone_is_an_error(self) -> None:
        node = NODE_11.replace(
            '((BoneName="head"))',
            '((BoneName="head"),(BoneName="neck_02"))',
        )
        text = build_t3d({"AnimGraphNode_PoseDriver_10": node})
        with self.assertRaises(epd.PoseDriverExtractError):
            epd.parse_pose_drivers(text, expected_counts={"PoseDriver_10": 1})

    def test_drive_poses_is_an_error(self) -> None:
        text = build_t3d(
            {"AnimGraphNode_PoseDriver_10": NODE_10.replace("DriveOutput=DriveCurves", "DriveOutput=DrivePoses")}
        )
        with self.assertRaises(epd.PoseDriverExtractError):
            epd.parse_pose_drivers(text, expected_counts={"PoseDriver_10": 2})

    def test_wrong_driver_count_is_an_error(self) -> None:
        with self.assertRaises(epd.PoseDriverExtractError):
            epd.parse_pose_drivers(
                self.text,
                expected_counts={"PoseDriver_10": 2, "PoseDriver_11": 1, "PoseDriver_12": 3},
            )

    def test_conflicting_duplicate_blocks_are_an_error(self) -> None:
        first = _template_block("AnimGraphNode_PoseDriver_10", NODE_11, with_node=True)
        second = _template_block(
            "AnimGraphNode_PoseDriver_10",
            NODE_11.replace('Radius=30', 'Radius=31'),
            with_node=True,
        )
        text = (
            'Begin Object Class=/Script/Engine.AnimBlueprint Name="TestAnimBP"\n'
            + first
            + second
            + "End Object\n"
        )
        with self.assertRaises(epd.PoseDriverExtractError):
            epd.parse_pose_drivers(text, expected_counts={"PoseDriver_10": 1})


class WriteTests(unittest.TestCase):
    def test_output_is_lf_utf8_stable_and_idempotent(self) -> None:
        text = build_t3d(
            {
                "AnimGraphNode_PoseDriver_10": NODE_10,
                "AnimGraphNode_PoseDriver_11": NODE_11,
            }
        )
        with tempfile.TemporaryDirectory() as directory:
            t3d_path = Path(directory) / "sample.t3d"
            output_path = Path(directory) / "nested" / "EPD_Test.json"
            t3d_path.write_bytes(text.encode("utf-16"))

            document, changed = epd.extract_pose_driver(
                t3d_path,
                output_path,
                expected_counts={"PoseDriver_10": 2, "PoseDriver_11": 1},
            )
            self.assertTrue(changed)
            payload = output_path.read_bytes()
            self.assertNotIn(b"\r", payload)
            self.assertTrue(payload.endswith(b"\n"))
            self.assertEqual(json.loads(payload.decode("utf-8")), document)

            _, changed_again = epd.extract_pose_driver(
                t3d_path,
                output_path,
                expected_counts={"PoseDriver_10": 2, "PoseDriver_11": 1},
            )
            self.assertFalse(changed_again)
            self.assertEqual(output_path.read_bytes(), payload)


if __name__ == "__main__":
    unittest.main()
