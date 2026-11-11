"""Unit tests for tool/animation/extract_bone_physics.py."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import extract_bone_physics as ebp  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]
REAL_DIR = REPO_ROOT / "artifacts" / "ue-echo-export" / "physics-t3d"


def body_block(
    name: str,
    bone: str,
    physics_type: str,
    *,
    aggregate: str | None = None,
    default_instance: str | None = None,
) -> str:
    lines = [
        f'   Begin Object Name="{name}" '
        f'ExportPath="/Script/Engine.SkeletalBodySetup\'PA_Test:{name}\'"',
    ]
    if aggregate is not None:
        lines.append(f"      AggGeom={aggregate}")
    if default_instance is not None:
        lines.append(f"      DefaultInstance=({default_instance})")
    lines.extend(
        [
            f'      BoneName="{bone}"',
            f"      PhysicsType={physics_type}",
            "   End Object",
        ]
    )
    return "\n".join(lines)


def constraint_block(
    name: str,
    bone1: str,
    bone2: str,
    *,
    joint: str = "Joint",
    extra: str = "",
) -> str:
    return "\n".join(
        [
            f'   Begin Object Name="{name}" '
            f'ExportPath="/Script/Engine.PhysicsConstraintTemplate'
            f'\'PA_Test:{name}\'"',
            "      DefaultInstance=("
            f'JointName="{joint}",'
            f'ConstraintBone1="{bone1}",'
            f'ConstraintBone2="{bone2}"{extra})',
            "   End Object",
        ]
    )


def assemble(
    bodies: list[str],
    constraints: list[str],
    body_refs: list[str],
    constraint_refs: list[str],
    *,
    asset: str = "PA_Test",
) -> str:
    lines = [
        f'Begin Object Class=/Script/Engine.PhysicsAsset Name="{asset}" '
        f'ExportPath="/Script/Engine.PhysicsAsset\'{asset}.{asset}\'"'
    ]
    lines.extend(bodies)
    lines.extend(constraints)
    for index, name in enumerate(body_refs):
        lines.append(
            f'   SkeletalBodySetups({index})="/Script/Engine.SkeletalBodySetup'
            f"'{name}'\""
        )
    for index, name in enumerate(constraint_refs):
        lines.append(
            f'   ConstraintSetup({index})="/Script/Engine.PhysicsConstraintTemplate'
            f"'{name}'\""
        )
    lines.append("End Object")
    return "\n".join(lines) + "\n"


CAPSULE = (
    "(SphylElems=((Center=(X=1.000000,Y=2.000000,Z=3.000000),"
    "Rotation=(Pitch=90.000000,Yaw=0.000000,Roll=0.000000),"
    "Radius=2.000000,Length=4.000000)))"
)


def simple_scene(
    *,
    child_type: str = "PhysType_Simulated",
    extra_cone: str = "Stiffness=500.000000,Damping=50.000000",
    frames: str = (
        "Pos1=(X=1.000000,Y=0.000000,Z=0.000000),"
        "PriAxis1=(X=1.000000,Y=0.000000,Z=0.000000),"
        "SecAxis1=(X=0.000000,Y=1.000000,Z=0.000000),"
        "Pos2=(X=0.000000,Y=0.000000,Z=0.000000),"
        "PriAxis2=(X=0.000000,Y=1.000000,Z=0.000000),"
        "SecAxis2=(X=1.000000,Y=0.000000,Z=0.000000)"
    ),
) -> str:
    parent = body_block(
        "SkeletalBodySetup_parent",
        "parent",
        "PhysType_Kinematic",
        aggregate=CAPSULE,
    )
    child = body_block(
        "SkeletalBodySetup_child",
        "child",
        child_type,
        aggregate=CAPSULE,
        default_instance=(
            "MassInKgOverride=7.500000,LinearDamping=1.500000,"
            "AngularDamping=2.500000,bOverrideMass=True"
        ),
    )
    constraint = constraint_block(
        "PhysicsConstraintTemplate_0",
        "child",
        "parent",
        extra=(
            f",{frames},ProfileInstance=(ConeLimit=("
            "Swing1LimitDegrees=30.000000,"
            "Swing2LimitDegrees=20.000000,"
            "Swing1Motion=ACM_Locked,"
            f"Swing2Motion=ACM_Limited,{extra_cone}),"
            "TwistLimit=(TwistLimitDegrees=10.000000,"
            "TwistMotion=ACM_Limited))"
        ),
    )
    return assemble(
        [child, parent],
        [constraint],
        ["SkeletalBodySetup_parent", "SkeletalBodySetup_child"],
        ["PhysicsConstraintTemplate_0"],
    )


class ParseTests(unittest.TestCase):
    def test_capsule_values_and_reference_order(self) -> None:
        parsed = ebp.parse_physics_asset(simple_scene(), "PA_Test")

        self.assertEqual([body["bone"] for body in parsed["bodies"]], ["parent", "child"])
        self.assertEqual(parsed["bodies"][0]["type"], "kinematic")
        self.assertIsNone(parsed["bodies"][0]["mass"])

        child = parsed["bodies"][1]
        self.assertEqual(child["type"], "simulated")
        self.assertEqual(child["mass"], 7.5)
        self.assertEqual(child["linearDamping"], 1.5)
        self.assertEqual(child["angularDamping"], 2.5)
        shape = child["shapes"][0]
        self.assertEqual(shape["type"], "capsule")
        self.assertEqual(shape["radius"], 2.0)
        self.assertEqual(shape["halfHeight"], 2.0)
        self.assertEqual(shape["center"], [1.0, 2.0, 3.0])
        self.assertAlmostEqual(shape["rotation"][1], -(2 ** -0.5))
        self.assertAlmostEqual(shape["rotation"][3], 2 ** -0.5)

    def test_constraint_limits_and_frames(self) -> None:
        parsed = ebp.parse_physics_asset(simple_scene(), "PA_Test")
        constraint = parsed["constraints"][0]

        self.assertEqual(constraint["name"], "Joint")
        self.assertEqual(constraint["child"], "child")
        self.assertEqual(constraint["parent"], "parent")
        self.assertEqual(
            constraint["swing1"], {"limit": 30.0, "motion": "locked"}
        )
        self.assertEqual(
            constraint["swing2"], {"limit": 20.0, "motion": "limited"}
        )
        self.assertEqual(
            constraint["twist"], {"limit": 10.0, "motion": "limited"}
        )
        self.assertEqual(constraint["stiffness"], 500.0)
        self.assertEqual(constraint["damping"], 50.0)
        self.assertEqual(constraint["childFrame"]["position"], [1.0, 0.0, 0.0])
        self.assertEqual(constraint["parentFrame"]["position"], [0.0, 0.0, 0.0])
        self.assertEqual(constraint["childFrame"]["rotation"], [0.0, 0.0, 0.0, 1.0])
        self.assertAlmostEqual(constraint["parentFrame"]["rotation"][0], 2 ** -0.5)
        self.assertAlmostEqual(constraint["parentFrame"]["rotation"][1], 2 ** -0.5)
        self.assertAlmostEqual(constraint["parentFrame"]["rotation"][3], 0.0)

    def test_swaps_frames_when_ue_order_is_parent_first(self) -> None:
        parent = body_block(
            "SkeletalBodySetup_parent",
            "clavicle",
            "PhysType_Kinematic",
            aggregate=CAPSULE,
        )
        child = body_block(
            "SkeletalBodySetup_child",
            "pad",
            "PhysType_Simulated",
            aggregate=CAPSULE,
        )
        # UE order is parent first, child second.
        constraint = constraint_block(
            "PhysicsConstraintTemplate_0",
            "clavicle",
            "pad",
            extra=(
                ",Pos1=(X=9.000000,Y=9.000000,Z=9.000000),"
                "PriAxis1=(X=1.000000,Y=0.000000,Z=0.000000),"
                "SecAxis1=(X=0.000000,Y=1.000000,Z=0.000000),"
                "Pos2=(X=1.000000,Y=2.000000,Z=3.000000),"
                "PriAxis2=(X=1.000000,Y=0.000000,Z=0.000000),"
                "SecAxis2=(X=0.000000,Y=1.000000,Z=0.000000)"
            ),
        )
        text = assemble(
            [parent, child],
            [constraint],
            ["SkeletalBodySetup_parent", "SkeletalBodySetup_child"],
            ["PhysicsConstraintTemplate_0"],
        )
        parsed = ebp.parse_physics_asset(text, "PA_Test")
        entry = parsed["constraints"][0]
        self.assertEqual(entry["child"], "pad")
        self.assertEqual(entry["parent"], "clavicle")
        self.assertEqual(entry["childFrame"]["position"], [1.0, 2.0, 3.0])
        self.assertEqual(entry["parentFrame"]["position"], [9.0, 9.0, 9.0])

    def test_capsule_defaults_and_missing_cone(self) -> None:
        parent = body_block(
            "SkeletalBodySetup_parent",
            "parent",
            "PhysType_Kinematic",
            aggregate="(SphylElems=((Rotation=(Pitch=0.000000,Yaw=0.000000,Roll=0.000000))))",
        )
        child = body_block(
            "SkeletalBodySetup_child",
            "child",
            "PhysType_Simulated",
            aggregate=CAPSULE,
        )
        constraint = constraint_block(
            "PhysicsConstraintTemplate_0",
            "child",
            "parent",
        )
        text = assemble(
            [parent, child],
            [constraint],
            ["SkeletalBodySetup_parent", "SkeletalBodySetup_child"],
            ["PhysicsConstraintTemplate_0"],
        )
        parsed = ebp.parse_physics_asset(text, "PA_Test")
        shape = parsed["bodies"][0]["shapes"][0]
        self.assertEqual(shape["radius"], 1.0)
        self.assertEqual(shape["halfHeight"], 0.5)
        self.assertEqual(shape["center"], [0.0, 0.0, 0.0])
        self.assertEqual(shape["rotation"], [0.0, 0.0, 0.0, 1.0])

        entry = parsed["constraints"][0]
        for axis in ("swing1", "swing2", "twist"):
            self.assertEqual(entry[axis], {"limit": 45.0, "motion": "free"})
        self.assertEqual(entry["stiffness"], 0.0)
        self.assertEqual(entry["damping"], 0.0)
        self.assertEqual(entry["childFrame"]["rotation"], [0.0, 0.0, 0.0, 1.0])
        self.assertEqual(entry["parentFrame"]["rotation"], [0.0, 0.0, 0.0, 1.0])

    def test_kinematic_pair_warns_and_strict_rejects(self) -> None:
        parent = body_block(
            "SkeletalBodySetup_parent",
            "parent",
            "PhysType_Kinematic",
            aggregate=CAPSULE,
        )
        child = body_block(
            "SkeletalBodySetup_child",
            "child",
            "PhysType_Kinematic",
            aggregate=CAPSULE,
        )
        spare = body_block(
            "SkeletalBodySetup_spare",
            "spare",
            "PhysType_Simulated",
            aggregate=CAPSULE,
        )
        text = assemble(
            [parent, child, spare],
            [constraint_block("PhysicsConstraintTemplate_0", "child", "parent")],
            [
                "SkeletalBodySetup_parent",
                "SkeletalBodySetup_child",
                "SkeletalBodySetup_spare",
            ],
            ["PhysicsConstraintTemplate_0"],
        )
        parsed = ebp.parse_physics_asset(text, "PA_Test")
        self.assertEqual(len(parsed["constraints"]), 1)
        self.assertEqual(len(parsed["warnings"]), 1)
        self.assertIn("two kinematic bodies", parsed["warnings"][0])

        with self.assertRaisesRegex(
            ebp.BonePhysicsExtractError, "two kinematic bodies"
        ):
            ebp.parse_physics_asset(text, "PA_Test", strict_constraints=True)

    def test_two_simulated_bodies_are_rejected(self) -> None:
        parent = body_block(
            "SkeletalBodySetup_parent",
            "parent",
            "PhysType_Simulated",
            aggregate=CAPSULE,
        )
        child = body_block(
            "SkeletalBodySetup_child",
            "child",
            "PhysType_Simulated",
            aggregate=CAPSULE,
        )
        text = assemble(
            [parent, child],
            [constraint_block("PhysicsConstraintTemplate_0", "child", "parent")],
            ["SkeletalBodySetup_parent", "SkeletalBodySetup_child"],
            ["PhysicsConstraintTemplate_0"],
        )
        with self.assertRaisesRegex(
            ebp.BonePhysicsExtractError, "simulated child"
        ):
            ebp.parse_physics_asset(text, "PA_Test", strict_constraints=True)

    def test_box_shape_is_rejected_and_listed(self) -> None:
        bad = simple_scene().replace(
            CAPSULE,
            "(BoxElems=((Box=(X=1.000000,Y=2.000000,Z=3.000000))),"
            "SphylElems=((Radius=2.000000,Length=4.000000)))",
            1,
        )
        with self.assertRaisesRegex(ebp.BonePhysicsExtractError, "BoxElems"):
            ebp.parse_physics_asset(bad, "PA_Test")

    def test_empty_capsule_array_is_rejected(self) -> None:
        bad = simple_scene().replace(CAPSULE, "(SphylElems=())", 1)
        with self.assertRaisesRegex(
            ebp.BonePhysicsExtractError, "empty SphylElems"
        ):
            ebp.parse_physics_asset(bad, "PA_Test")

    def test_unbalanced_blocks_are_rejected(self) -> None:
        text = simple_scene().replace("End Object\n", "", 1)
        with self.assertRaisesRegex(
            ebp.BonePhysicsExtractError, "unterminated object block"
        ):
            ebp.parse_object_tree(text)


@unittest.skipUnless(REAL_DIR.is_dir(), "real UE physics T3D export is absent")
class RealAssetTests(unittest.TestCase):
    def test_expected_sets_and_counts(self) -> None:
        document, changed = ebp.extract_bone_physics(REAL_DIR, None)
        self.assertFalse(changed)
        self.assertEqual(document["format"], "vl.bone-physics.v1")
        self.assertEqual(
            [entry["asset"] for entry in document["sets"]],
            [
                "PA_Echo_Canister",
                "PA_Echo_Pouch",
                "PA_Echo_ShoulderPad",
                "PA_Echo_Skirt",
            ],
        )
        counts = {
            entry["name"]: (len(entry["bodies"]), len(entry["constraints"]))
            for entry in document["sets"]
        }
        self.assertEqual(
            counts,
            {
                "canister": (5, 1),
                "pouch": (2, 1),
                "shoulderPad": (2, 1),
                "skirt": (12, 6),
            },
        )
        for entry in document["sets"]:
            self.assertTrue(
                any(body["type"] == "simulated" for body in entry["bodies"])
            )
            self.assertEqual(entry["baseBone"], "root")
            self.assertEqual(entry["gravity"], [0.0, -980.0, 0.0])

        skirt = next(entry for entry in document["sets"] if entry["name"] == "skirt")
        inert = [
            entry
            for entry in skirt["constraints"]
            if entry["name"] == "UserConstraint"
        ]
        self.assertEqual(len(inert), 1)
        self.assertEqual(inert[0]["child"], "C_Skirt_A_BackRoot_Jnt")
        self.assertEqual(inert[0]["parent"], "C_Skirt_A_Back1_Jnt")

        self.assertEqual(
            [source["asset"] for source in document["sources"]],
            [entry["asset"] for entry in document["sets"]],
        )
        for source in document["sources"]:
            self.assertEqual(
                source["blake3"],
                ebp.blake3_file(REAL_DIR / f"{source['asset']}.t3d"),
            )

    def test_output_is_byte_stable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "BPH_EchoPostProcess.json"
            first, first_changed = ebp.extract_bone_physics(REAL_DIR, output)
            self.assertTrue(first_changed)
            first_bytes = output.read_bytes()
            second, second_changed = ebp.extract_bone_physics(REAL_DIR, output)
            self.assertFalse(second_changed)
            self.assertEqual(output.read_bytes(), first_bytes)
            self.assertEqual(first, second)
            self.assertTrue(first_bytes.endswith(b"\n"))
            self.assertNotIn(b"\r\n", first_bytes)
            self.assertEqual(
                first_bytes.decode("utf-8"),
                json.dumps(first, ensure_ascii=False, indent=2) + "\n",
            )


if __name__ == "__main__":
    unittest.main()
