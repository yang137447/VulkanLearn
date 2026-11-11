"""Unit tests for tool/animation/extract_post_process.py."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import extract_post_process as epp  # noqa: E402
import extract_constraints as ec  # noqa: E402
from gasp_anim_common import blake3_file, json_bytes  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]
REAL_LINKS = REPO_ROOT / "artifacts" / "ue-echo-export" / "ctrlrig-links3.json"
REAL_HELPERS = REPO_ROOT / "artifacts" / "ue-echo-export" / "helpers-graph.json"

HELPER_PARENTS: dict[str, tuple[str, ...]] = {
    "C_Scarf_A_Root_Jnt": ("clavicle_l",),
    "C_Scarf_A_Joint2_Jnt": ("spine_04",),
    "C_Scarf_A_Joint3_Jnt": ("spine_04",),
    "L_Skirt_A_BackRoot_Jnt": ("pelvis",),
    "R_Skirt_A_BackRoot_Jnt": ("pelvis",),
    "C_Skirt_A_BackRoot_Jnt": ("pelvis",),
    "C_Skirt_A_FrontRoot_Jnt": (
        "L_Skirt_A_FrontRoot_Jnt",
        "R_Skirt_A_FrontRoot_Jnt",
    ),
}


def _links(nodes: list[dict]) -> dict:
    return {"twist": nodes, "helpers": []}


def _append_chain(
    nodes: list[dict],
    start: int,
    driver: str,
    targets: list[tuple[str, str]],
    *,
    invert: bool,
    parent_bone: str | None = None,
) -> int:
    """Append one complete ControlRig chain and return the next free index."""

    bind_index = start
    pose_index = start + 1
    relative_index = start + 2
    swing_index = start + 3
    nodes.extend(
        [
            {
                "index": bind_index,
                "title": epp.TITLE_GET_TRANSFORM,
                "item": driver,
                "weight": "",
                "inputs": [],
            },
            {
                "index": pose_index,
                "title": epp.TITLE_GET_TRANSFORM,
                "item": parent_bone if parent_bone is not None else driver,
                "weight": "",
                "inputs": [],
            },
            {
                "index": relative_index,
                "title": epp.TITLE_SET_RELATIVE,
                "item": "",
                "weight": "",
                "inputs": [
                    {
                        "pin": "Parent",
                        "from_index": bind_index,
                        "from_title": epp.TITLE_GET_TRANSFORM,
                        "from_pin": "Transform",
                    },
                    {
                        "pin": "Global",
                        "from_index": pose_index,
                        "from_title": epp.TITLE_GET_TRANSFORM,
                        "from_pin": "Transform",
                    },
                ],
            },
            {
                "index": swing_index,
                "title": epp.TITLE_SWING_AND_TWIST,
                "item": "",
                "weight": "",
                "inputs": [
                    {
                        "pin": "Input",
                        "from_index": relative_index,
                        "from_title": epp.TITLE_SET_RELATIVE,
                        "from_pin": "Rotation",
                    }
                ],
            },
        ]
    )

    value_index = swing_index
    value_title = epp.TITLE_SWING_AND_TWIST
    value_pin = "Twist"
    next_index = swing_index + 1
    if invert:
        invert_index = next_index
        nodes.append(
            {
                "index": invert_index,
                "title": epp.TITLE_INVERT,
                "item": "",
                "weight": "",
                "inputs": [
                    {
                        "pin": "Value",
                        "from_index": swing_index,
                        "from_title": epp.TITLE_SWING_AND_TWIST,
                        "from_pin": "Twist",
                    }
                ],
            }
        )
        value_index = invert_index
        value_title = epp.TITLE_INVERT
        value_pin = "Result"
        next_index += 1

    for target_bone, weight in targets:
        nodes.append(
            {
                "index": next_index,
                "title": epp.TITLE_SET_ROTATION,
                "item": target_bone,
                "weight": weight,
                "inputs": [
                    {
                        "pin": "Value",
                        "from_index": value_index,
                        "from_title": value_title,
                        "from_pin": value_pin,
                    }
                ],
            }
        )
        next_index += 1
    return next_index


def _echo_like_links() -> dict:
    nodes: list[dict] = []
    next_index = 0
    for driver, invert in epp.EXPECTED_INVERT.items():
        targets = [
            (bone, "1.000000" if position == 0 else "0.500000")
            for position, bone in enumerate(epp.EXPECTED_TWIST_BONES[driver])
        ]
        next_index = _append_chain(
            nodes, next_index, driver, targets, invert=invert
        )
    return _links(nodes)


def _bone_pin(name: str, bone: str) -> dict:
    return {
        "name": name,
        "default": f'(Type=Bone,Name="{bone}")',
        "sub": {"Type": "Bone", "Name": bone},
    }


def _scalar_pin(name: str, value: str) -> dict:
    return {"name": name, "default": value, "sub": {}}


def _projection_node(
    index: int,
    child: str,
    old_parent: str,
    new_parent: str,
    *,
    child_initial: str = "True",
    old_initial: str = "True",
    new_initial: str = "",
) -> dict:
    return {
        "index": index,
        "title": epp.TITLE_PROJECT_TO_NEW_PARENT,
        "pins": [
            _bone_pin("Child", child),
            _scalar_pin("bChildInitial", child_initial),
            _bone_pin("OldParent", old_parent),
            _scalar_pin("bOldParentInitial", old_initial),
            _bone_pin("NewParent", new_parent),
            _scalar_pin("bNewParentInitial", new_initial),
        ],
        "inputs": [],
    }


def _set_rotation_node(
    index: int,
    target: str,
    weight: str,
    value_index: int,
    value_title: str,
    *,
    space: str = "GlobalSpace",
    propagate: str = "true",
) -> dict:
    return {
        "index": index,
        "title": epp.TITLE_SET_ROTATION,
        "pins": [
            _bone_pin("Item", target),
            _scalar_pin("Space", space),
            _scalar_pin("Weight", weight),
            _scalar_pin("bPropagateToChildren", propagate),
        ],
        "inputs": [
            {
                "pin": "Value",
                "from_index": value_index,
                "from_title": value_title,
                "from_pin": "Rotation",
            }
        ],
    }


def _interpolate_node(
    index: int,
    left_index: int,
    right_index: int,
    blend: str,
) -> dict:
    return {
        "index": index,
        "title": epp.TITLE_INTERPOLATE,
        "pins": [_scalar_pin("T", blend)],
        "inputs": [
            {
                "pin": "A",
                "from_index": left_index,
                "from_title": epp.TITLE_PROJECT_TO_NEW_PARENT,
                "from_pin": "Rotation",
            },
            {
                "pin": "B",
                "from_index": right_index,
                "from_title": epp.TITLE_PROJECT_TO_NEW_PARENT,
                "from_pin": "Rotation",
            },
        ],
    }


def _single_projection_helper(name: str, weight: str, parent: str) -> list[dict]:
    return [
        _projection_node(0, name, parent, parent),
        _set_rotation_node(
            1, name, weight, 0, epp.TITLE_PROJECT_TO_NEW_PARENT
        ),
    ]


def _blended_projection_helper(
    name: str,
    weight: str,
    parents: tuple[str, str],
    blend: str = "0.500000",
) -> list[dict]:
    return [
        _projection_node(0, name, parents[0], parents[0]),
        _projection_node(1, name, parents[1], parents[1]),
        _interpolate_node(2, 0, 1, blend),
        _set_rotation_node(3, name, weight, 2, epp.TITLE_INTERPOLATE),
    ]


def _helpers(nodes: list[dict]) -> dict:
    return {"helpers": nodes}


def _echo_like_helpers() -> dict:
    nodes: list[dict] = []
    for name, weight in epp.EXPECTED_HELPER_WEIGHTS.items():
        parents = HELPER_PARENTS[name]
        base = len(nodes)
        if len(parents) == 2:
            nodes.extend(
                _blended_projection_helper(name, f"{weight:.6f}", parents)
            )
        else:
            nodes.extend(
                _single_projection_helper(name, f"{weight:.6f}", parents[0])
            )
        for node in nodes[base:]:
            node["index"] += base
        for node in nodes[base:]:
            for entry in node["inputs"]:
                entry["from_index"] += base
    return _helpers(nodes)


class ParseTests(unittest.TestCase):
    def test_parses_invert_true(self) -> None:
        nodes: list[dict] = []
        _append_chain(
            nodes,
            0,
            "upperarm_l",
            [
                ("upperarm_twist_01_l", "1.000000"),
                ("upperarm_twist_02_l", "0.500000"),
            ],
            invert=True,
        )
        groups = epp.parse_twist_groups(_links(nodes))
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0]["driverBone"], "upperarm_l")
        self.assertTrue(groups[0]["invert"])
        self.assertEqual(
            [entry["bone"] for entry in groups[0]["targets"]],
            ["upperarm_twist_01_l", "upperarm_twist_02_l"],
        )
        self.assertEqual(groups[0]["targets"][0]["weight"], 1.0)
        self.assertEqual(groups[0]["targets"][1]["weight"], 0.5)

    def test_parses_invert_false(self) -> None:
        nodes: list[dict] = []
        _append_chain(
            nodes,
            0,
            "hand_l",
            [("lowerarm_twist_01_l", "1.000000")],
            invert=False,
        )
        groups = epp.parse_twist_groups(_links(nodes))
        self.assertEqual(len(groups), 1)
        self.assertFalse(groups[0]["invert"])
        self.assertEqual(groups[0]["targets"][0]["bone"], "lowerarm_twist_01_l")

    def test_driver_bone_mismatch_raises(self) -> None:
        nodes: list[dict] = []
        _append_chain(
            nodes,
            0,
            "upperarm_l",
            [("upperarm_twist_01_l", "1.000000")],
            invert=True,
            parent_bone="upperarm_r",
        )
        with self.assertRaisesRegex(epp.PostProcessExtractError, "驱动骨 mismatch"):
            epp.parse_twist_groups(_links(nodes))

    def test_missing_source_raises(self) -> None:
        nodes: list[dict] = []
        _append_chain(
            nodes,
            0,
            "hand_r",
            [("lowerarm_twist_01_r", "1.000000")],
            invert=False,
        )
        nodes[-1]["inputs"][0]["from_index"] = 999
        with self.assertRaisesRegex(epp.PostProcessExtractError, "missing node 999"):
            epp.parse_twist_groups(_links(nodes))

    def test_orders_drivers_and_targets_deterministically(self) -> None:
        nodes: list[dict] = []
        next_index = 0
        next_index = _append_chain(
            nodes,
            next_index,
            "upperarm_l",
            [
                ("upperarm_twist_02_l", "0.500000"),
                ("upperarm_twist_01_l", "1.000000"),
            ],
            invert=True,
        )
        _append_chain(
            nodes,
            next_index,
            "hand_l",
            [("lowerarm_twist_01_l", "1.000000")],
            invert=False,
        )
        groups = epp.parse_twist_groups(_links(list(reversed(nodes))))
        self.assertEqual([group["name"] for group in groups], ["hand_l", "upperarm_l"])
        self.assertEqual(
            [entry["bone"] for entry in groups[1]["targets"]],
            ["upperarm_twist_01_l", "upperarm_twist_02_l"],
        )

    def test_canonical_json_uses_numeric_weights(self) -> None:
        groups = epp.parse_twist_groups(_echo_like_links())
        asset = epp.build_document(groups, "00" * 32)
        payload = json_bytes(asset)
        self.assertIn(b'"weight": 1.0', payload)
        self.assertNotIn(b'"1.000000"', payload)
        self.assertTrue(payload.endswith(b"\n"))
        self.assertNotIn(b"\r\n", payload)


class EchoValidationTests(unittest.TestCase):
    def test_full_synthetic_graph_passes(self) -> None:
        groups = epp.parse_twist_groups(_echo_like_links())
        epp.validate_echo_twists(groups)
        total = sum(len(group["targets"]) for group in groups)
        self.assertEqual(len(groups), 8)
        self.assertEqual(total, 16)

    def test_wrong_group_count_raises(self) -> None:
        nodes: list[dict] = []
        _append_chain(
            nodes,
            0,
            "upperarm_l",
            [
                ("upperarm_twist_01_l", "1.000000"),
                ("upperarm_twist_02_l", "0.500000"),
            ],
            invert=True,
        )
        groups = epp.parse_twist_groups(_links(nodes))
        with self.assertRaisesRegex(epp.PostProcessExtractError, "8 twist groups"):
            epp.validate_echo_twists(groups)

    def test_wrong_target_count_raises(self) -> None:
        links = _echo_like_links()
        next_index = max(node["index"] for node in links["twist"]) + 1
        invert_node = next(
            node for node in links["twist"] if node["title"] == epp.TITLE_INVERT
        )
        links["twist"].append(
            {
                "index": next_index,
                "title": epp.TITLE_SET_ROTATION,
                "item": "upperarm_twist_03_l",
                "weight": "0.250000",
                "inputs": [
                    {
                        "pin": "Value",
                        "from_index": invert_node["index"],
                        "from_title": epp.TITLE_INVERT,
                        "from_pin": "Result",
                    }
                ],
            }
        )
        groups = epp.parse_twist_groups(links)
        self.assertEqual(len(groups), 8)
        self.assertEqual(
            sum(len(group["targets"]) for group in groups), 17
        )
        with self.assertRaisesRegex(epp.PostProcessExtractError, "16 twist targets"):
            epp.validate_echo_twists(groups)

    def test_wrong_invert_raises(self) -> None:
        links = _echo_like_links()
        targets = set(epp.EXPECTED_TWIST_BONES["upperarm_l"])
        invert_node = next(
            node for node in links["twist"] if node["title"] == epp.TITLE_INVERT
        )
        swing_index = invert_node["inputs"][0]["from_index"]
        for node in links["twist"]:
            if node["title"] == epp.TITLE_SET_ROTATION and node["item"] in targets:
                node["inputs"][0]["from_title"] = epp.TITLE_SWING_AND_TWIST
                node["inputs"][0]["from_pin"] = "Twist"
                node["inputs"][0]["from_index"] = swing_index
        groups = epp.parse_twist_groups(links)
        with self.assertRaisesRegex(epp.PostProcessExtractError, "invert mismatch"):
            epp.validate_echo_twists(groups)


class HelperParseTests(unittest.TestCase):
    def test_single_projection(self) -> None:
        groups = epp.parse_helper_groups(
            _helpers(_single_projection_helper("scarf_root", "1.000000", "clavicle_l"))
        )
        self.assertEqual(len(groups), 1)
        group = groups[0]
        self.assertEqual(group["name"], "scarf_root")
        self.assertEqual(group["blendT"], 0.0)
        self.assertEqual(
            group["projections"],
            [
                {
                    "child": "scarf_root",
                    "oldParent": "clavicle_l",
                    "newParent": "clavicle_l",
                    "childInitial": True,
                    "oldParentInitial": True,
                    "newParentInitial": False,
                }
            ],
        )
        self.assertEqual(
            group["targets"], [{"bone": "scarf_root", "weight": 1.0}]
        )

    def test_two_projections_interpolate(self) -> None:
        groups = epp.parse_helper_groups(
            _helpers(
                _blended_projection_helper(
                    "front_root",
                    "0.500000",
                    ("left_root", "right_root"),
                )
            )
        )
        self.assertEqual(len(groups), 1)
        group = groups[0]
        self.assertEqual(group["blendT"], 0.5)
        self.assertEqual(
            [projection["oldParent"] for projection in group["projections"]],
            ["left_root", "right_root"],
        )
        self.assertEqual(
            [projection["child"] for projection in group["projections"]],
            ["front_root", "front_root"],
        )
        self.assertEqual(
            group["targets"], [{"bone": "front_root", "weight": 0.5}]
        )

    def test_non_global_space_raises(self) -> None:
        nodes = _single_projection_helper("scarf_root", "1.000000", "clavicle_l")
        nodes[1]["pins"][1] = _scalar_pin("Space", "ParentSpace")
        with self.assertRaisesRegex(
            epp.PostProcessExtractError, "GlobalSpace"
        ):
            epp.parse_helper_groups(_helpers(nodes))

    def test_non_propagating_raises(self) -> None:
        nodes = _single_projection_helper("scarf_root", "1.000000", "clavicle_l")
        nodes[1]["pins"][3] = _scalar_pin("bPropagateToChildren", "false")
        with self.assertRaisesRegex(
            epp.PostProcessExtractError, "bPropagateToChildren"
        ):
            epp.parse_helper_groups(_helpers(nodes))

    def test_blend_out_of_range_raises(self) -> None:
        nodes = _blended_projection_helper(
            "front_root", "1.000000", ("left_root", "right_root"), blend="1.5"
        )
        with self.assertRaisesRegex(
            epp.PostProcessExtractError, "blend T must be in"
        ):
            epp.parse_helper_groups(_helpers(nodes))

    def test_echo_like_helpers_validate(self) -> None:
        groups = epp.parse_helper_groups(_echo_like_helpers())
        epp.validate_echo_helpers(groups)
        self.assertEqual(len(groups), 7)
        self.assertEqual(
            [group["name"] for group in groups],
            sorted(epp.EXPECTED_HELPER_WEIGHTS),
        )

    def test_wrong_helper_count_raises(self) -> None:
        groups = epp.parse_helper_groups(
            _helpers(_single_projection_helper("scarf_root", "1.000000", "clavicle_l"))
        )
        with self.assertRaisesRegex(epp.PostProcessExtractError, "7 helpers"):
            epp.validate_echo_helpers(groups)


def _echo_constraints_document() -> dict:
    return {
        "format": ec.FORMAT,
        "id": "EPP_EchoPostProcess.constraints",
        "source": {
            "engine": "UE 5.8.4",
            "t3d": "artifacts/ue-echo-export/anim-audit/Echo_PostProcess_AnimBP.t3d",
            "t3dBlake3": "ab" * 32,
        },
        "constraints": [
            {
                "name": bone,
                "bone": bone,
                "transformType": "Parent",
                "offsetOption": "RefPose",
                "targets": [
                    {"bone": target, "weight": weight}
                    for target, weight in targets
                ],
            }
            for bone, targets in (
                ("C_Scarf_A_Root_Jnt", [("spine_05", 0.49), ("clavicle_l", 0.51)]),
                (
                    "R_Canister_A_Joint1_Jnt",
                    [("spine_02", 0.212), ("spine_01", 0.283), ("pelvis", 0.505)],
                ),
                (
                    "R_LargePouch_A_Joint1_Jnt",
                    [("spine_02", 0.05), ("spine_01", 0.25), ("pelvis", 0.7)],
                ),
            )
        ],
    }


class ConstraintSectionTests(unittest.TestCase):
    def test_parses_echo_constraints(self) -> None:
        constraints, digest = epp.parse_constraints_document(
            _echo_constraints_document()
        )
        self.assertEqual(len(constraints), 3)
        self.assertEqual(digest, "ab" * 32)

    def test_wrong_format_is_an_error(self) -> None:
        document = _echo_constraints_document()
        document["format"] = "vl.anim-constraints.v0"
        with self.assertRaisesRegex(epp.PostProcessExtractError, "format"):
            epp.parse_constraints_document(document)

    def test_empty_constraints_is_an_error(self) -> None:
        document = _echo_constraints_document()
        document["constraints"] = []
        with self.assertRaisesRegex(epp.PostProcessExtractError, "non-empty"):
            epp.parse_constraints_document(document)

    def test_missing_digest_is_an_error(self) -> None:
        document = _echo_constraints_document()
        del document["source"]["t3dBlake3"]
        with self.assertRaisesRegex(epp.PostProcessExtractError, "t3dBlake3"):
            epp.parse_constraints_document(document)

    def test_build_document_embeds_constraints_and_digests(self) -> None:
        constraints, digest = epp.parse_constraints_document(
            _echo_constraints_document()
        )
        asset = epp.build_document(
            [],
            "00" * 32,
            constraints=constraints,
            constraints_json_blake3="11" * 32,
            constraints_source_blake3=digest,
        )
        self.assertEqual(list(asset)[:3], ["format", "id", "source"])
        self.assertEqual(asset["constraints"], constraints)
        self.assertEqual(asset["source"]["constraintsBlake3"], "11" * 32)
        self.assertEqual(asset["source"]["constraintsSourceBlake3"], digest)
        self.assertEqual(asset["twist"], [])
        self.assertNotIn("helpers", asset)

    def test_empty_document_is_an_error(self) -> None:
        with self.assertRaisesRegex(epp.PostProcessExtractError, "at least one"):
            epp.build_document([], "00" * 32)

    @unittest.skipUnless(REAL_LINKS.exists(), f"missing real export: {REAL_LINKS}")
    def test_extract_with_constraints_writes_section(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            constraints_path = Path(directory) / "constraints.json"
            output_path = Path(directory) / "EPP.json"
            constraints_path.write_bytes(
                json_bytes(_echo_constraints_document())
            )

            asset, changed = epp.extract_post_process(
                REAL_LINKS,
                output_path,
                helpers_path=REAL_HELPERS if REAL_HELPERS.exists() else None,
                constraints_path=constraints_path,
            )
            self.assertTrue(changed)
            self.assertEqual(len(asset["twist"]), 8)
            self.assertEqual(len(asset["constraints"]), 3)
            self.assertEqual(
                asset["source"]["constraintsBlake3"],
                blake3_file(constraints_path),
            )
            self.assertEqual(
                asset["source"]["constraintsSourceBlake3"], "ab" * 32
            )

            _, changed_again = epp.extract_post_process(
                REAL_LINKS,
                output_path,
                helpers_path=REAL_HELPERS if REAL_HELPERS.exists() else None,
                constraints_path=constraints_path,
            )
            self.assertFalse(changed_again)


@unittest.skipUnless(REAL_LINKS.exists(), f"missing real export: {REAL_LINKS}")
class RealExportTests(unittest.TestCase):
    def test_real_echo_export(self) -> None:
        helpers_path = REAL_HELPERS if REAL_HELPERS.exists() else None
        asset, changed = epp.extract_post_process(
            REAL_LINKS, None, helpers_path=helpers_path
        )
        self.assertFalse(changed)
        self.assertEqual(len(asset["twist"]), 8)
        self.assertEqual(
            sum(len(group["targets"]) for group in asset["twist"]), 16
        )
        self.assertEqual(asset["source"]["rigJsonBlake3"], blake3_file(REAL_LINKS))

        expected_invert = {
            "upperarm_l": True,
            "upperarm_r": True,
            "thigh_l": True,
            "thigh_r": True,
            "hand_l": False,
            "hand_r": False,
            "foot_l": False,
            "foot_r": False,
        }
        self.assertEqual(
            {group["name"]: group["invert"] for group in asset["twist"]},
            expected_invert,
        )
        for group in asset["twist"]:
            self.assertEqual(
                [entry["bone"] for entry in group["targets"]],
                list(epp.EXPECTED_TWIST_BONES[group["name"]]),
            )

        if helpers_path is not None:
            self.assertEqual(len(asset["helpers"]), 7)
            self.assertEqual(
                asset["source"]["helpersJsonBlake3"],
                blake3_file(REAL_HELPERS),
            )
            by_name = {group["name"]: group for group in asset["helpers"]}
            self.assertEqual(set(by_name), set(epp.EXPECTED_HELPER_WEIGHTS))
            for name, expected_weight in epp.EXPECTED_HELPER_WEIGHTS.items():
                group = by_name[name]
                self.assertEqual(
                    group["targets"],
                    [{"bone": name, "weight": expected_weight}],
                )
            front = by_name["C_Skirt_A_FrontRoot_Jnt"]
            self.assertEqual(front["blendT"], 0.5)
            self.assertEqual(
                [projection["oldParent"] for projection in front["projections"]],
                ["L_Skirt_A_FrontRoot_Jnt", "R_Skirt_A_FrontRoot_Jnt"],
            )
            for name, group in by_name.items():
                if name == "C_Skirt_A_FrontRoot_Jnt":
                    continue
                self.assertEqual(len(group["projections"]), 1)
                self.assertEqual(group["blendT"], 0.0)


if __name__ == "__main__":
    unittest.main()
