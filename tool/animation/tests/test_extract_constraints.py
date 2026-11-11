"""Unit tests for tool/animation/extract_constraints.py."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import extract_constraints as ec  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]
REAL_T3D = (
    REPO_ROOT
    / "artifacts"
    / "ue-echo-export"
    / "anim-audit"
    / "Echo_PostProcess_AnimBP.t3d"
)


def node_text(
    bone: str,
    targets: list[tuple[str, str]],
    *,
    transform_type: str = "Parent",
    extra: str = "",
) -> str:
    setups = ",".join(
        f'(TargetBone=(BoneName="{target}"),TransformType={transform_type})'
        for target, _ in targets
    )
    weights = ",".join(weight for _, weight in targets)
    return (
        f'Node=(BoneToModify=(BoneName="{bone}"),'
        f"ConstraintSetup=({setups}),"
        f"ConstraintWeights=({weights}){extra},"
        f"AlphaBoolBlend=(BlendOption=Linear))"
    )


def _graph_block(graph_name: str, nodes: list[tuple[str, str]]) -> str:
    body = ""
    for name, node in nodes:
        body += (
            f'      Begin Object Name="{name}" '
            f'ExportPath="/Script/AnimGraph.AnimGraphNode_Constraint\''
            f"TestAnimBP:{graph_name}.{name}'\"\n"
            f"         {node}\n"
            "      End Object\n"
        )
    return body


def build_t3d(
    animgraph: list[tuple[str, str]],
    ubergraph: list[tuple[str, str]] | None = None,
) -> str:
    """Build a minimal AnimBlueprint T3D with AnimGraph + Ubergraph copies."""

    ubergraph = animgraph if ubergraph is None else ubergraph
    text = (
        'Begin Object Class=/Script/Engine.AnimBlueprint Name="TestAnimBP" '
        'ExportPath="/Script/Engine.AnimBlueprint\'TestAnimBP\'"\n'
        '   Begin Object Class=/Script/AnimGraph.AnimationGraph '
        'Name="AnimGraph" ExportPath="'
        "/Script/AnimGraph.AnimationGraph'TestAnimBP:AnimGraph'\"\n"
        + _graph_block("AnimGraph", animgraph)
        + "   End Object\n"
        '   Begin Object Class=/Script/Engine.EdGraph '
        'Name="ExecuteUbergraph_TestAnimBP" ExportPath="'
        "/Script/Engine.EdGraph'TestAnimBP:ExecuteUbergraph_TestAnimBP'\"\n"
        + _graph_block("ExecuteUbergraph_TestAnimBP", ubergraph)
        + "   End Object\n"
        "End Object\n"
    )
    return text


SCARF = node_text(
    "C_Scarf_A_Root_Jnt",
    [("spine_05", "0.490000"), ("clavicle_l", "0.510000")],
)


class ParseTests(unittest.TestCase):
    def test_utf16_bom_and_utf8_fallback(self) -> None:
        text = build_t3d([("AnimGraphNode_Constraint_0", SCARF)])
        self.assertEqual(ec.decode_t3d(text.encode("utf-16")), text)
        self.assertEqual(ec.decode_t3d(text.encode("utf-8")), text)

    def test_deduplicates_animgraph_and_ubergraph_copies(self) -> None:
        text = build_t3d([("AnimGraphNode_Constraint_0", SCARF)])
        document = ec.parse_constraints(
            text, validate=False, expected_counts={"AnimGraphNode_Constraint_0": 2}
        )
        self.assertEqual(document["format"], ec.FORMAT)
        self.assertEqual(len(document["constraints"]), 1)
        entry = document["constraints"][0]
        self.assertEqual(entry["name"], "C_Scarf_A_Root_Jnt")
        self.assertEqual(entry["bone"], "C_Scarf_A_Root_Jnt")
        self.assertEqual(entry["transformType"], "Parent")
        self.assertEqual(entry["offsetOption"], "RefPose")
        self.assertEqual(
            entry["targets"],
            [
                {"bone": "spine_05", "weight": 0.49},
                {"bone": "clavicle_l", "weight": 0.51},
            ],
        )
        self.assertNotIn("divergences", document["source"])

    def test_offsets_and_per_axis_defaults_are_implicit(self) -> None:
        text = build_t3d([("AnimGraphNode_Constraint_0", SCARF)])
        entry = ec.parse_constraints(text, validate=False)["constraints"][0]
        self.assertEqual(entry["offsetOption"], "RefPose")

    def test_explicit_ref_pose_and_per_axis_true_are_accepted(self) -> None:
        node = node_text(
            "C_Scarf_A_Root_Jnt",
            [("spine_05", "0.490000"), ("clavicle_l", "0.510000")],
            extra=",PerAxis=(bX=True,bY=True,bZ=True)",
        ).replace(
            "TransformType=Parent)",
            "TransformType=Parent,OffsetOption=Offset_RefPose)",
        )
        text = build_t3d([("AnimGraphNode_Constraint_0", node)])
        entry = ec.parse_constraints(text, validate=False)["constraints"][0]
        self.assertEqual(entry["offsetOption"], "RefPose")

    def test_non_parent_transform_is_an_error(self) -> None:
        node = node_text(
            "C_Scarf_A_Root_Jnt",
            [("spine_05", "0.490000"), ("clavicle_l", "0.510000")],
            transform_type="Rotation",
        )
        text = build_t3d([("AnimGraphNode_Constraint_0", node)])
        with self.assertRaisesRegex(ec.ConstraintsExtractError, "TransformType"):
            ec.parse_constraints(text, validate=False)

    def test_no_offset_is_an_error(self) -> None:
        node = node_text(
            "C_Scarf_A_Root_Jnt",
            [("spine_05", "0.490000"), ("clavicle_l", "0.510000")],
        ).replace(
            "TransformType=Parent)",
            "TransformType=Parent,OffsetOption=None)",
        )
        text = build_t3d([("AnimGraphNode_Constraint_0", node)])
        with self.assertRaisesRegex(ec.ConstraintsExtractError, "OffsetOption"):
            ec.parse_constraints(text, validate=False)

    def test_per_axis_filter_is_an_error(self) -> None:
        node = node_text(
            "C_Scarf_A_Root_Jnt",
            [("spine_05", "0.490000"), ("clavicle_l", "0.510000")],
        ).replace(
            "TransformType=Parent)",
            "TransformType=Parent,PerAxis=(bX=True,bY=False,bZ=True))",
        )
        text = build_t3d([("AnimGraphNode_Constraint_0", node)])
        with self.assertRaisesRegex(ec.ConstraintsExtractError, "PerAxis.bY"):
            ec.parse_constraints(text, validate=False)

    def test_weight_count_mismatch_is_an_error(self) -> None:
        node = node_text(
            "C_Scarf_A_Root_Jnt",
            [("spine_05", "0.490000"), ("clavicle_l", "0.510000")],
        ).replace("ConstraintWeights=(0.490000,0.510000)", "ConstraintWeights=(0.490000)")
        text = build_t3d([("AnimGraphNode_Constraint_0", node)])
        with self.assertRaisesRegex(ec.ConstraintsExtractError, "2 targets but 1 weights"):
            ec.parse_constraints(text, validate=False)

    def test_conflicting_copies_are_reported_and_prefer_ubergraph(self) -> None:
        anim = node_text(
            "R_LargePouch_A_Joint1_Jnt",
            [
                ("spine_02", "1.000000"),
                ("spine_01", "1.000000"),
                ("pelvis", "1.000000"),
            ],
        )
        uber = node_text(
            "R_LargePouch_A_Joint1_Jnt",
            [
                ("spine_02", "0.050000"),
                ("spine_01", "0.250000"),
                ("pelvis", "0.700000"),
            ],
        )
        text = build_t3d(
            [("AnimGraphNode_Constraint_4", anim)],
            [("AnimGraphNode_Constraint_4", uber)],
        )
        document = ec.parse_constraints(text, validate=False)
        self.assertEqual(
            [target["weight"] for target in document["constraints"][0]["targets"]],
            [0.05, 0.25, 0.7],
        )
        self.assertEqual(
            document["source"]["divergences"],
            [
                {
                    "node": "AnimGraphNode_Constraint_4",
                    "chosen": "ubergraph",
                    "chosenWeights": [0.05, 0.25, 0.7],
                    "other": "animgraph",
                    "otherWeights": [1.0, 1.0, 1.0],
                }
            ],
        )

        flipped = ec.parse_constraints(text, validate=False, prefer="animgraph")
        self.assertEqual(
            [target["weight"] for target in flipped["constraints"][0]["targets"]],
            [1.0, 1.0, 1.0],
        )
        self.assertEqual(flipped["source"]["divergences"][0]["chosen"], "animgraph")

    def test_echo_contract_rejects_missing_constraints(self) -> None:
        text = build_t3d([("AnimGraphNode_Constraint_0", SCARF)])
        with self.assertRaisesRegex(ec.ConstraintsExtractError, "3 constraints"):
            ec.parse_constraints(text)


class WriteTests(unittest.TestCase):
    def test_output_is_lf_utf8_stable_and_idempotent(self) -> None:
        text = build_t3d([("AnimGraphNode_Constraint_0", SCARF)])
        with tempfile.TemporaryDirectory() as directory:
            t3d_path = Path(directory) / "sample.t3d"
            output_path = Path(directory) / "nested" / "constraints.json"
            t3d_path.write_bytes(text.encode("utf-16"))

            document, changed = ec.extract_constraints(
                t3d_path, output_path, validate=False
            )
            self.assertTrue(changed)
            payload = output_path.read_bytes()
            self.assertNotIn(b"\r", payload)
            self.assertTrue(payload.endswith(b"\n"))
            self.assertEqual(json.loads(payload.decode("utf-8")), document)

            _, changed_again = ec.extract_constraints(
                t3d_path, output_path, validate=False
            )
            self.assertFalse(changed_again)
            self.assertEqual(output_path.read_bytes(), payload)


@unittest.skipUnless(REAL_T3D.exists(), f"missing real export: {REAL_T3D}")
class RealExportTests(unittest.TestCase):
    def test_real_echo_constraints(self) -> None:
        document, changed = ec.extract_constraints(REAL_T3D, None)
        self.assertFalse(changed)
        self.assertEqual(len(document["constraints"]), 3)
        self.assertEqual(
            [entry["bone"] for entry in document["constraints"]],
            [
                "C_Scarf_A_Root_Jnt",
                "R_Canister_A_Joint1_Jnt",
                "R_LargePouch_A_Joint1_Jnt",
            ],
        )
        by_bone = {entry["bone"]: entry for entry in document["constraints"]}
        self.assertEqual(
            [(t["bone"], t["weight"]) for t in by_bone["C_Scarf_A_Root_Jnt"]["targets"]],
            [("spine_05", 0.49), ("clavicle_l", 0.51)],
        )
        self.assertEqual(
            [
                (t["bone"], t["weight"])
                for t in by_bone["R_Canister_A_Joint1_Jnt"]["targets"]
            ],
            [("spine_02", 0.212), ("spine_01", 0.283), ("pelvis", 0.505)],
        )
        # Compiled CDO value; the AnimGraph page copy still says 1,1,1.
        self.assertEqual(
            [
                (t["bone"], t["weight"])
                for t in by_bone["R_LargePouch_A_Joint1_Jnt"]["targets"]
            ],
            [("spine_02", 0.05), ("spine_01", 0.25), ("pelvis", 0.7)],
        )
        self.assertEqual(
            document["source"]["divergences"],
            [
                {
                    "node": "AnimGraphNode_Constraint_4",
                    "chosen": "ubergraph",
                    "chosenWeights": [0.05, 0.25, 0.7],
                    "other": "animgraph",
                    "otherWeights": [1.0, 1.0, 1.0],
                }
            ],
        )


if __name__ == "__main__":
    unittest.main()
