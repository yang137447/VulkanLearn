#!/usr/bin/env python3
"""Extract the Echo ControlRig post-process chains into a post-process asset.

``Echo_Twist_CtrlRig`` distributes the swing/twist of a driver bone across two
twist helper bones.  The exported ControlRig graph uses this chain:

    获取变换 (bind)  -> 设为相对.Parent
    获取变换 (pose)  -> 设为相对.Global
    设为相对.Rotation -> 到摇摆和扭曲.Input
    到摇摆和扭曲.Twist -> [反转.Value] -> 设置旋转.Value

Each ``设置旋转`` writes one target bone with a local-space slerp weight.  This
tool resolves those links into the stable ``vl.anim-post-process.v1`` asset
consumed by the animation runtime.

``Echo_Helpers_CtrlRig`` is exported separately (``helpers-graph.json``).  Its
``设置旋转`` nodes write scarf/skirt helper bones in global space, with the value
coming either straight from one ``投射到新父项`` (project a transform onto a new
parent) or from an ``插值`` of two such projections.  Those nodes are resolved
into the sibling ``helpers`` array.

``AnimNode_Constraint`` nodes live in the AnimBP T3D, not in a ControlRig
export.  ``extract_constraints.py`` writes them to ``constraints.json``; when
that file exists its ``constraints`` array is embedded verbatim and the
constraint/T3D digests are recorded in ``source``.

Usage (from the repository root):

    py -3 tool/animation/extract_post_process.py
    py -3 tool/animation/extract_post_process.py --links <links.json> --output <json>
    py -3 tool/animation/extract_post_process.py --helpers <helpers-graph.json>
    py -3 tool/animation/extract_post_process.py --constraints <constraints.json>
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

try:
    from .gasp_anim_common import blake3_file, json_bytes
    from .extract_constraints import (
        FORMAT as CONSTRAINTS_FORMAT,
        ConstraintsExtractError,
        validate_echo_constraints,
    )
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from gasp_anim_common import blake3_file, json_bytes
    from extract_constraints import (
        FORMAT as CONSTRAINTS_FORMAT,
        ConstraintsExtractError,
        validate_echo_constraints,
    )


FORMAT = "vl.anim-post-process.v1"
DEFAULT_ASSET_ID = "EPP_EchoPostProcess"
DEFAULT_ENGINE = "UE 5.8.4"
DEFAULT_RIG = "Echo_Twist_CtrlRig"
DEFAULT_LINKS_LABEL = "artifacts/ue-echo-export/ctrlrig-links3.json"
DEFAULT_HELPERS_LABEL = "artifacts/ue-echo-export/helpers-graph.json"
DEFAULT_CONSTRAINTS_LABEL = "artifacts/ue-echo-export/constraints.json"

TITLE_GET_TRANSFORM = "获取变换"
TITLE_SET_RELATIVE = "设为相对"
TITLE_SWING_AND_TWIST = "到摇摆和扭曲"
TITLE_INVERT = "反转"
TITLE_SET_ROTATION = "设置旋转"
TITLE_PROJECT_TO_NEW_PARENT = "投射到新父项"
TITLE_INTERPOLATE = "插值"

TWIST_AXIS_X = "X"

# ``设置旋转`` in the helpers rig must write the evaluated global rotation and
# let children inherit it; anything else means the exported graph drifted.
HELPER_SPACE_GLOBAL = "GlobalSpace"

# ``links3.json`` does not serialize ``到摇摆和扭曲.TwistAxis``.  The graph uses
# the ControlRig default (1,0,0), which is the local X axis.
EXPECTED_TWIST_BONES: dict[str, tuple[str, str]] = {
    "upperarm_l": ("upperarm_twist_01_l", "upperarm_twist_02_l"),
    "upperarm_r": ("upperarm_twist_01_r", "upperarm_twist_02_r"),
    "thigh_l": ("thigh_twist_01_l", "thigh_twist_02_l"),
    "thigh_r": ("thigh_twist_01_r", "thigh_twist_02_r"),
    "hand_l": ("lowerarm_twist_01_l", "lowerarm_twist_02_l"),
    "hand_r": ("lowerarm_twist_01_r", "lowerarm_twist_02_r"),
    "foot_l": ("calf_twist_01_l", "calf_twist_02_l"),
    "foot_r": ("calf_twist_01_r", "calf_twist_02_r"),
}

# The arm/thigh chain inverts the swing/twist quaternion; the hand/foot chain
# drives the forearm/calf twist directly.
EXPECTED_INVERT: dict[str, bool] = {
    "upperarm_l": True,
    "upperarm_r": True,
    "thigh_l": True,
    "thigh_r": True,
    "hand_l": False,
    "hand_r": False,
    "foot_l": False,
    "foot_r": False,
}

EXPECTED_GROUP_COUNT = 8
EXPECTED_TARGET_COUNT = 16

# Exact Echo helper contract.  ``C_Skirt_A_FrontRoot_Jnt`` is the only helper
# that blends two parent projections; everything else is a single projection.
EXPECTED_HELPER_WEIGHTS: dict[str, float] = {
    "C_Scarf_A_Root_Jnt": 1.0,
    "C_Scarf_A_Joint2_Jnt": 0.5,
    "C_Scarf_A_Joint3_Jnt": 0.5,
    "L_Skirt_A_BackRoot_Jnt": 1.0,
    "R_Skirt_A_BackRoot_Jnt": 1.0,
    "C_Skirt_A_BackRoot_Jnt": 1.0,
    "C_Skirt_A_FrontRoot_Jnt": 1.0,
}

EXPECTED_FRONT_PROJECTION_PARENTS = (
    "L_Skirt_A_FrontRoot_Jnt",
    "R_Skirt_A_FrontRoot_Jnt",
)
EXPECTED_FRONT_BLEND_T = 0.5
EXPECTED_HELPER_COUNT = 7


class PostProcessExtractError(RuntimeError):
    """Raised when the ControlRig link export violates the known chain."""


def _as_node_list(document: Any, label: str) -> list[dict[str, Any]]:
    if not isinstance(document, dict):
        raise PostProcessExtractError(f"{label} must be a JSON object")
    nodes = document.get("twist")
    if not isinstance(nodes, list):
        raise PostProcessExtractError(f"{label}.twist must be an array")
    result: list[dict[str, Any]] = []
    for position, node in enumerate(nodes):
        if not isinstance(node, dict):
            raise PostProcessExtractError(f"{label}.twist[{position}] must be an object")
        result.append(node)
    return result


def _node_index(node: dict[str, Any]) -> int:
    index = node.get("index")
    if not isinstance(index, int) or isinstance(index, bool):
        raise PostProcessExtractError(f"node index must be an integer: {node!r}")
    return index


def _node_title(node: dict[str, Any]) -> str:
    title = node.get("title")
    if not isinstance(title, str) or not title:
        raise PostProcessExtractError(f"node {_node_index(node)} has no title")
    return title


def _node_item(node: dict[str, Any]) -> str:
    item = node.get("item", "")
    if not isinstance(item, str):
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) item must be a string"
        )
    return item.strip()


def _build_index(nodes: list[dict[str, Any]]) -> dict[int, dict[str, Any]]:
    index: dict[int, dict[str, Any]] = {}
    for node in nodes:
        node_id = _node_index(node)
        if node_id in index:
            raise PostProcessExtractError(f"duplicate node index {node_id}")
        index[node_id] = node
    return index


def _input(node: dict[str, Any], pin: str) -> dict[str, Any]:
    inputs = node.get("inputs")
    if not isinstance(inputs, list):
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) has no inputs array"
        )
    matches = [
        entry for entry in inputs
        if isinstance(entry, dict) and entry.get("pin") == pin
    ]
    if not matches:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) has no {pin!r} input"
        )
    if len(matches) > 1:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) has duplicate {pin!r} inputs"
        )
    return matches[0]


def _source_node(
    node: dict[str, Any],
    pin: str,
    index: dict[int, dict[str, Any]],
) -> dict[str, Any]:
    entry = _input(node, pin)
    source_index = entry.get("from_index")
    if not isinstance(source_index, int) or isinstance(source_index, bool):
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) {pin!r} "
            f"has no integer from_index"
        )
    source = index.get(source_index)
    if source is None:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) {pin!r} "
            f"references missing node {source_index}"
        )
    expected_title = entry.get("from_title")
    if isinstance(expected_title, str) and expected_title and expected_title != _node_title(source):
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) {pin!r} says "
            f"{expected_title!r}, but node {source_index} is {_node_title(source)!r}"
        )
    return source


def _require_title(node: dict[str, Any], expected: str, label: str) -> None:
    actual = _node_title(node)
    if actual != expected:
        raise PostProcessExtractError(
            f"{label}: expected {expected!r}, got {actual!r} "
            f"(node {_node_index(node)})"
        )


def _weight_value(raw: Any, node: dict[str, Any]) -> float:
    if not isinstance(raw, str) or not raw.strip():
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) has no weight"
        )
    try:
        weight = float(raw)
    except ValueError as error:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) "
            f"has invalid weight {raw!r}"
        ) from error
    if not math.isfinite(weight):
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) "
            f"has non-finite weight {raw!r}"
        )
    if weight < 0.0:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) "
            f"has negative weight {weight}"
        )
    return weight


def _parse_weight(node: dict[str, Any]) -> float:
    return _weight_value(node.get("weight", ""), node)


def _pin(node: dict[str, Any], name: str) -> dict[str, Any]:
    pins = node.get("pins")
    if not isinstance(pins, list):
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) has no pins array"
        )
    matches = [
        entry for entry in pins
        if isinstance(entry, dict) and entry.get("name") == name
    ]
    if not matches:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) has no {name!r} pin"
        )
    if len(matches) > 1:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) has duplicate {name!r} pins"
        )
    return matches[0]


def _pin_default(node: dict[str, Any], name: str) -> str:
    default = _pin(node, name).get("default")
    if not isinstance(default, str):
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) {name!r} default "
            f"must be a string"
        )
    return default.strip()


def _pin_bone(node: dict[str, Any], name: str) -> str:
    sub = _pin(node, name).get("sub")
    bone = sub.get("Name") if isinstance(sub, dict) else None
    if not isinstance(bone, str) or not bone.strip():
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) {name!r} is not a bone"
        )
    return bone.strip()


def _pin_bool(node: dict[str, Any], name: str) -> bool:
    raw = _pin_default(node, name)
    if raw in ("True", "true"):
        return True
    if raw in ("False", "false", ""):
        return False
    raise PostProcessExtractError(
        f"node {_node_index(node)} ({_node_title(node)}) {name!r} has "
        f"invalid boolean {raw!r}"
    )


def _pin_weight(node: dict[str, Any]) -> float:
    return _weight_value(_pin_default(node, "Weight"), node)


def _driver_bone(relative_node: dict[str, Any], index: dict[int, dict[str, Any]]) -> str:
    global_node = _source_node(relative_node, "Global", index)
    parent_node = _source_node(relative_node, "Parent", index)
    _require_title(global_node, TITLE_GET_TRANSFORM, "设为相对.Global")
    _require_title(parent_node, TITLE_GET_TRANSFORM, "设为相对.Parent")

    global_bone = _node_item(global_node)
    parent_bone = _node_item(parent_node)
    if not global_bone or not parent_bone:
        raise PostProcessExtractError(
            f"设为相对 (node {_node_index(relative_node)}) has an empty "
            f"驱动骨: Global={global_bone!r}, Parent={parent_bone!r}"
        )
    if global_bone != parent_bone:
        raise PostProcessExtractError(
            f"设为相对 (node {_node_index(relative_node)}) 驱动骨 mismatch: "
            f"Global={global_bone!r}, Parent={parent_bone!r}"
        )
    return global_bone


def _natural_key(value: str) -> list[Any]:
    return [
        int(part) if part.isdigit() else part
        for part in re.split(r"(\d+)", value)
    ]


def parse_twist_groups(document: Any, label: str = "links") -> list[dict[str, Any]]:
    """Resolve every ``设置旋转`` into a driver-grouped twist target."""

    nodes = _as_node_list(document, label)
    index = _build_index(nodes)
    groups: dict[str, dict[str, Any]] = {}

    for node in nodes:
        if _node_title(node) != TITLE_SET_ROTATION:
            continue

        target_bone = _node_item(node)
        if not target_bone:
            raise PostProcessExtractError(
                f"{TITLE_SET_ROTATION} (node {_node_index(node)}) has an empty item"
            )
        weight = _parse_weight(node)

        value_source = _source_node(node, "Value", index)
        value_title = _node_title(value_source)
        if value_title == TITLE_INVERT:
            invert = True
            swing_node = _source_node(value_source, "Value", index)
            _require_title(swing_node, TITLE_SWING_AND_TWIST, f"{TITLE_INVERT}.Value")
        elif value_title == TITLE_SWING_AND_TWIST:
            invert = False
            swing_node = value_source
        else:
            raise PostProcessExtractError(
                f"{TITLE_SET_ROTATION} (node {_node_index(node)}) Value must come "
                f"from {TITLE_INVERT!r} or {TITLE_SWING_AND_TWIST!r}, got {value_title!r}"
            )

        relative_node = _source_node(swing_node, "Input", index)
        _require_title(relative_node, TITLE_SET_RELATIVE, f"{TITLE_SWING_AND_TWIST}.Input")
        driver = _driver_bone(relative_node, index)

        group = groups.get(driver)
        if group is None:
            group = {
                "name": driver,
                "driverBone": driver,
                "twistAxis": TWIST_AXIS_X,
                "invert": invert,
                "targets": [],
            }
            groups[driver] = group
        elif group["invert"] != invert:
            raise PostProcessExtractError(
                f"driver {driver!r} mixes invert={group['invert']} and invert={invert}"
            )

        if any(entry["bone"] == target_bone for entry in group["targets"]):
            raise PostProcessExtractError(
                f"driver {driver!r} drives target {target_bone!r} more than once"
            )
        group["targets"].append({"bone": target_bone, "weight": weight})

    result: list[dict[str, Any]] = []
    for driver in sorted(groups):
        group = groups[driver]
        group["targets"].sort(key=lambda entry: _natural_key(entry["bone"]))
        result.append(group)
    return result


def validate_echo_twists(groups: list[dict[str, Any]]) -> None:
    """Assert the exact Echo driver/target contract."""

    target_count = sum(len(group["targets"]) for group in groups)
    if len(groups) != EXPECTED_GROUP_COUNT:
        raise PostProcessExtractError(
            f"expected {EXPECTED_GROUP_COUNT} twist groups, got {len(groups)}: "
            f"{[group['name'] for group in groups]}"
        )
    if target_count != EXPECTED_TARGET_COUNT:
        raise PostProcessExtractError(
            f"expected {EXPECTED_TARGET_COUNT} twist targets, got {target_count}"
        )

    actual_drivers = {group["name"] for group in groups}
    expected_drivers = set(EXPECTED_TWIST_BONES)
    if actual_drivers != expected_drivers:
        missing = sorted(expected_drivers - actual_drivers)
        extra = sorted(actual_drivers - expected_drivers)
        raise PostProcessExtractError(
            f"Echo driver set mismatch; missing={missing} extra={extra}"
        )

    for group in groups:
        driver = group["name"]
        actual_targets = [entry["bone"] for entry in group["targets"]]
        expected_targets = list(EXPECTED_TWIST_BONES[driver])
        if actual_targets != expected_targets:
            raise PostProcessExtractError(
                f"driver {driver!r} targets mismatch: "
                f"expected {expected_targets}, got {actual_targets}"
            )
        expected_invert = EXPECTED_INVERT[driver]
        if group["invert"] != expected_invert:
            raise PostProcessExtractError(
                f"driver {driver!r} invert mismatch: "
                f"expected {expected_invert}, got {group['invert']}"
            )


def _as_helper_node_list(document: Any, label: str) -> list[dict[str, Any]]:
    if not isinstance(document, dict):
        raise PostProcessExtractError(f"{label} must be a JSON object")
    nodes = document.get("helpers")
    if not isinstance(nodes, list):
        raise PostProcessExtractError(f"{label}.helpers must be an array")
    result: list[dict[str, Any]] = []
    for position, node in enumerate(nodes):
        if not isinstance(node, dict):
            raise PostProcessExtractError(
                f"{label}.helpers[{position}] must be an object"
            )
        result.append(node)
    return result


def _projection(node: dict[str, Any], label: str) -> dict[str, Any]:
    _require_title(node, TITLE_PROJECT_TO_NEW_PARENT, label)
    return {
        "child": _pin_bone(node, "Child"),
        "oldParent": _pin_bone(node, "OldParent"),
        "newParent": _pin_bone(node, "NewParent"),
        "childInitial": _pin_bool(node, "bChildInitial"),
        "oldParentInitial": _pin_bool(node, "bOldParentInitial"),
        "newParentInitial": _pin_bool(node, "bNewParentInitial"),
    }


def _blend_t(node: dict[str, Any]) -> float:
    raw = _pin_default(node, "T")
    try:
        blend = float(raw)
    except ValueError as error:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) has invalid "
            f"blend T {raw!r}"
        ) from error
    if not math.isfinite(blend) or blend < 0.0 or blend > 1.0:
        raise PostProcessExtractError(
            f"node {_node_index(node)} ({_node_title(node)}) blend T must be "
            f"in [0, 1], got {raw!r}"
        )
    return blend


def parse_helper_groups(
    document: Any,
    label: str = "helpers",
) -> list[dict[str, Any]]:
    """Resolve every helper ``设置旋转`` into a projection-backed target."""

    nodes = _as_helper_node_list(document, label)
    index = _build_index(nodes)
    groups: dict[str, dict[str, Any]] = {}

    for node in nodes:
        if _node_title(node) != TITLE_SET_ROTATION:
            continue

        target_bone = _pin_bone(node, "Item")
        space = _pin_default(node, "Space")
        if space != HELPER_SPACE_GLOBAL:
            raise PostProcessExtractError(
                f"{TITLE_SET_ROTATION} (node {_node_index(node)}) must use "
                f"{HELPER_SPACE_GLOBAL!r}, got {space!r}"
            )
        propagate = _pin_default(node, "bPropagateToChildren")
        if propagate.lower() != "true":
            raise PostProcessExtractError(
                f"{TITLE_SET_ROTATION} (node {_node_index(node)}) must set "
                f"bPropagateToChildren=true, got {propagate!r}"
            )
        weight = _pin_weight(node)

        value_source = _source_node(node, "Value", index)
        value_title = _node_title(value_source)
        if value_title == TITLE_PROJECT_TO_NEW_PARENT:
            projections = [_projection(value_source, f"{TITLE_SET_ROTATION}.Value")]
            blend_t = 0.0
        elif value_title == TITLE_INTERPOLATE:
            left = _source_node(value_source, "A", index)
            right = _source_node(value_source, "B", index)
            projections = [
                _projection(left, f"{TITLE_INTERPOLATE}.A"),
                _projection(right, f"{TITLE_INTERPOLATE}.B"),
            ]
            blend_t = _blend_t(value_source)
        else:
            raise PostProcessExtractError(
                f"{TITLE_SET_ROTATION} (node {_node_index(node)}) Value must come "
                f"from {TITLE_PROJECT_TO_NEW_PARENT!r} or {TITLE_INTERPOLATE!r}, "
                f"got {value_title!r}"
            )

        for projection in projections:
            if projection["child"] != target_bone:
                raise PostProcessExtractError(
                    f"helper {target_bone!r} projects child "
                    f"{projection['child']!r}; the projected child must be the "
                    f"target bone"
                )

        group = groups.get(target_bone)
        if group is None:
            group = {
                "name": target_bone,
                "projections": projections,
                "blendT": blend_t,
                "targets": [],
            }
            groups[target_bone] = group
        elif group["projections"] != projections or group["blendT"] != blend_t:
            raise PostProcessExtractError(
                f"helper {target_bone!r} has conflicting projections or blendT"
            )

        if any(entry["bone"] == target_bone for entry in group["targets"]):
            raise PostProcessExtractError(
                f"helper {target_bone!r} is driven more than once"
            )
        group["targets"].append({"bone": target_bone, "weight": weight})

    return [groups[name] for name in sorted(groups)]


def validate_echo_helpers(groups: list[dict[str, Any]]) -> None:
    """Assert the exact Echo helper bone/weight/projection contract."""

    if len(groups) != EXPECTED_HELPER_COUNT:
        raise PostProcessExtractError(
            f"expected {EXPECTED_HELPER_COUNT} helpers, got {len(groups)}: "
            f"{[group['name'] for group in groups]}"
        )

    actual_names = {group["name"] for group in groups}
    expected_names = set(EXPECTED_HELPER_WEIGHTS)
    if actual_names != expected_names:
        missing = sorted(expected_names - actual_names)
        extra = sorted(actual_names - expected_names)
        raise PostProcessExtractError(
            f"Echo helper set mismatch; missing={missing} extra={extra}"
        )

    for group in groups:
        name = group["name"]
        if len(group["targets"]) != 1:
            raise PostProcessExtractError(
                f"helper {name!r} must have exactly one target, "
                f"got {len(group['targets'])}"
            )
        target = group["targets"][0]
        if target["bone"] != name:
            raise PostProcessExtractError(
                f"helper {name!r} drives {target['bone']!r}"
            )
        expected_weight = EXPECTED_HELPER_WEIGHTS[name]
        if not math.isclose(target["weight"], expected_weight, abs_tol=1e-9):
            raise PostProcessExtractError(
                f"helper {name!r} weight mismatch: "
                f"expected {expected_weight}, got {target['weight']}"
            )

        projections = group["projections"]
        if name == "C_Skirt_A_FrontRoot_Jnt":
            if len(projections) != 2:
                raise PostProcessExtractError(
                    f"helper {name!r} must have 2 projections, "
                    f"got {len(projections)}"
                )
            actual_parents = tuple(
                projection["oldParent"] for projection in projections
            )
            if actual_parents != EXPECTED_FRONT_PROJECTION_PARENTS:
                raise PostProcessExtractError(
                    f"helper {name!r} projection parents mismatch: "
                    f"expected {list(EXPECTED_FRONT_PROJECTION_PARENTS)}, "
                    f"got {list(actual_parents)}"
                )
            for projection in projections:
                if projection["newParent"] != projection["oldParent"]:
                    raise PostProcessExtractError(
                        f"helper {name!r} projection must keep the parent, "
                        f"got old={projection['oldParent']!r} "
                        f"new={projection['newParent']!r}"
                    )
            if not math.isclose(
                group["blendT"], EXPECTED_FRONT_BLEND_T, abs_tol=1e-9
            ):
                raise PostProcessExtractError(
                    f"helper {name!r} blendT mismatch: expected "
                    f"{EXPECTED_FRONT_BLEND_T}, got {group['blendT']}"
                )
        else:
            if len(projections) != 1:
                raise PostProcessExtractError(
                    f"helper {name!r} must have 1 projection, "
                    f"got {len(projections)}"
                )
            projection = projections[0]
            if projection["oldParent"] != projection["newParent"]:
                raise PostProcessExtractError(
                    f"helper {name!r} must project onto its own parent, "
                    f"got old={projection['oldParent']!r} "
                    f"new={projection['newParent']!r}"
                )
            if group["blendT"] != 0.0:
                raise PostProcessExtractError(
                    f"helper {name!r} single projection must use blendT=0.0, "
                    f"got {group['blendT']}"
                )


def parse_constraints_document(
    document: Any,
    label: str = "constraints",
    *,
    validate: bool = True,
) -> tuple[list[dict[str, Any]], str]:
    """Validate the ``vl.anim-constraints.v1`` document and return its array.

    Returns ``(constraints, t3d_blake3)``.  The entries are copied verbatim so
    the embedded section stays byte-for-byte equal to the extractor output.
    """

    if not isinstance(document, dict):
        raise PostProcessExtractError(f"{label} must be a JSON object")
    fmt = document.get("format")
    if fmt != CONSTRAINTS_FORMAT:
        raise PostProcessExtractError(
            f"{label}.format must be {CONSTRAINTS_FORMAT!r}, got {fmt!r}"
        )
    constraints = document.get("constraints")
    if not isinstance(constraints, list) or not constraints:
        raise PostProcessExtractError(
            f"{label}.constraints must be a non-empty array"
        )
    for position, entry in enumerate(constraints):
        if not isinstance(entry, dict):
            raise PostProcessExtractError(
                f"{label}.constraints[{position}] must be an object"
            )
        unknown = set(entry) - {"name", "bone", "transformType", "offsetOption", "targets"}
        if unknown:
            raise PostProcessExtractError(
                f"{label}.constraints[{position}] has unknown keys {sorted(unknown)}"
            )
        targets = entry.get("targets")
        if not isinstance(targets, list) or not targets:
            raise PostProcessExtractError(
                f"{label}.constraints[{position}].targets must be a non-empty array"
            )
        for target_position, target in enumerate(targets):
            if not isinstance(target, dict) or set(target) != {"bone", "weight"}:
                raise PostProcessExtractError(
                    f"{label}.constraints[{position}].targets"
                    f"[{target_position}] must be {{bone, weight}}"
                )
    if validate:
        try:
            validate_echo_constraints(constraints)
        except ConstraintsExtractError as error:
            raise PostProcessExtractError(
                f"{label} does not match the Echo contract: {error}"
            ) from error

    source = document.get("source")
    t3d_blake3 = source.get("t3dBlake3") if isinstance(source, dict) else None
    if not isinstance(t3d_blake3, str) or not t3d_blake3:
        raise PostProcessExtractError(f"{label}.source.t3dBlake3 is missing")
    return constraints, t3d_blake3


def build_document(
    groups: list[dict[str, Any]],
    rig_json_blake3: str,
    *,
    helpers: list[dict[str, Any]] | None = None,
    helpers_json_blake3: str | None = None,
    constraints: list[dict[str, Any]] | None = None,
    constraints_json_blake3: str | None = None,
    constraints_source_blake3: str | None = None,
    asset_id: str = DEFAULT_ASSET_ID,
    engine: str = DEFAULT_ENGINE,
    rig: str = DEFAULT_RIG,
) -> dict[str, Any]:
    """Build the ordered asset document from validated groups."""

    source: dict[str, Any] = {
        "engine": engine,
        "rig": rig,
        "rigJsonBlake3": rig_json_blake3,
    }
    if helpers_json_blake3 is not None:
        source["helpersJsonBlake3"] = helpers_json_blake3
    if constraints_json_blake3 is not None:
        source["constraintsBlake3"] = constraints_json_blake3
    if constraints_source_blake3 is not None:
        source["constraintsSourceBlake3"] = constraints_source_blake3

    document: dict[str, Any] = {
        "format": FORMAT,
        "id": asset_id,
        "source": source,
        "twist": groups,
    }
    if helpers is not None:
        document["helpers"] = helpers
    if constraints is not None:
        document["constraints"] = constraints
    if not groups and not helpers and not constraints:
        raise PostProcessExtractError(
            "post-process asset requires at least one twist, helper or "
            "constraint entry"
        )
    return document


def extract_post_process(
    links_path: Path,
    output_path: Path | None = None,
    *,
    helpers_path: Path | None = None,
    constraints_path: Path | None = None,
    asset_id: str = DEFAULT_ASSET_ID,
    engine: str = DEFAULT_ENGINE,
    rig: str = DEFAULT_RIG,
    validate: bool = True,
) -> tuple[dict[str, Any], bool]:
    """Read *links_path*, build the asset and optionally write it.

    Returns ``(document, changed)``.  The JSON is UTF-8, 2-space indented, LF
    terminated and byte-stable across runs.  When *helpers_path* or
    *constraints_path* is omitted the corresponding section is absent.
    """

    links_path = links_path.resolve()
    raw = links_path.read_bytes()
    try:
        document = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PostProcessExtractError(f"cannot parse {links_path}: {error}") from error

    groups = parse_twist_groups(document, str(links_path))
    if validate:
        validate_echo_twists(groups)

    helpers: list[dict[str, Any]] | None = None
    helpers_digest: str | None = None
    if helpers_path is not None:
        helpers_path = helpers_path.resolve()
        if not helpers_path.is_file():
            raise FileNotFoundError(f"missing helpers export: {helpers_path}")
        try:
            helpers_document = json.loads(
                helpers_path.read_bytes().decode("utf-8-sig")
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise PostProcessExtractError(
                f"cannot parse {helpers_path}: {error}"
            ) from error
        helpers = parse_helper_groups(helpers_document, str(helpers_path))
        if validate:
            validate_echo_helpers(helpers)
        helpers_digest = blake3_file(helpers_path)

    constraints: list[dict[str, Any]] | None = None
    constraints_digest: str | None = None
    constraints_source_digest: str | None = None
    if constraints_path is not None:
        constraints_path = constraints_path.resolve()
        if not constraints_path.is_file():
            raise FileNotFoundError(f"missing constraints export: {constraints_path}")
        try:
            constraints_document = json.loads(
                constraints_path.read_bytes().decode("utf-8-sig")
            )
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise PostProcessExtractError(
                f"cannot parse {constraints_path}: {error}"
            ) from error
        constraints, constraints_source_digest = parse_constraints_document(
            constraints_document, str(constraints_path), validate=validate
        )
        constraints_digest = blake3_file(constraints_path)

    asset = build_document(
        groups,
        blake3_file(links_path),
        helpers=helpers,
        helpers_json_blake3=helpers_digest,
        constraints=constraints,
        constraints_json_blake3=constraints_digest,
        constraints_source_blake3=constraints_source_digest,
        asset_id=asset_id,
        engine=engine,
        rig=rig,
    )
    if output_path is None:
        return asset, False

    output_path = output_path.resolve()
    payload = json_bytes(asset)
    changed = True
    if output_path.exists():
        changed = output_path.read_bytes() != payload
    if changed:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(payload)
    return asset, changed


def _default_paths() -> tuple[Path, Path, Path, Path]:
    repo_root = Path(__file__).resolve().parents[2]
    sibling_resources = repo_root.parent / "VukanLearnResources"
    return (
        repo_root / DEFAULT_LINKS_LABEL,
        repo_root / DEFAULT_HELPERS_LABEL,
        repo_root / DEFAULT_CONSTRAINTS_LABEL,
        sibling_resources / "Maps" / "SC_valley_echo" / "Rig" / "EPP_EchoPostProcess.json",
    )


def print_summary(
    document: dict[str, Any],
    output_path: Path | None,
    changed: bool,
) -> None:
    print(f"{'driver':<16}{'invert':<8}{'targets':<40}{'weights'}")
    print("-" * 84)
    for group in document["twist"]:
        targets = ",".join(entry["bone"] for entry in group["targets"])
        weights = ",".join(f"{entry['weight']:.6g}" for entry in group["targets"])
        print(f"{group['name']:<16}{str(group['invert']):<8}{targets:<40}{weights}")
    total_targets = sum(len(group["targets"]) for group in document["twist"])
    print("-" * 84)
    print(f"groups={len(document['twist'])} targets={total_targets}")
    if "helpers" in document:
        print()
        print(f"{'helper':<28}{'projections':<52}{'blendT':<9}{'targets'}")
        print("-" * 84)
        for group in document["helpers"]:
            parents = " | ".join(
                f"{projection['oldParent']}->{projection['newParent']}"
                if projection["oldParent"] != projection["newParent"]
                else projection["newParent"]
                for projection in group["projections"]
            )
            targets = ",".join(
                f"{entry['bone']}@{entry['weight']:.6g}"
                for entry in group["targets"]
            )
            print(
                f"{group['name']:<28}{parents:<52}"
                f"{group['blendT']:<9.6g}{targets}"
            )
        print("-" * 84)
        print(f"helpers={len(document['helpers'])}")
    if "constraints" in document:
        print()
        print(f"{'constraint':<30}{'targets'}")
        print("-" * 84)
        for entry in document["constraints"]:
            targets = ",".join(
                f"{target['bone']}@{target['weight']:.6g}"
                for target in entry["targets"]
            )
            print(f"{entry['bone']:<30}{targets}")
        print("-" * 84)
        print(f"constraints={len(document['constraints'])}")
    print(f"blake3={document['source'].get('rigJsonBlake3', '<missing>')}")
    if "helpersJsonBlake3" in document["source"]:
        print(f"helpersBlake3={document['source']['helpersJsonBlake3']}")
    if "constraintsBlake3" in document["source"]:
        print(f"constraintsBlake3={document['source']['constraintsBlake3']}")
        print(
            f"constraintsSourceBlake3="
            f"{document['source']['constraintsSourceBlake3']}"
        )
    if output_path is not None:
        state = "written" if changed else "unchanged"
        print(f"{state}: {output_path}")


def main(argv: list[str] | None = None) -> int:
    default_links, default_helpers, default_constraints, default_output = (
        _default_paths()
    )
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--links", type=Path, default=default_links)
    parser.add_argument("--helpers", type=Path, default=default_helpers)
    parser.add_argument("--constraints", type=Path, default=default_constraints)
    parser.add_argument("--output", type=Path, default=default_output)
    parser.add_argument("--id", default=DEFAULT_ASSET_ID)
    parser.add_argument("--engine", default=DEFAULT_ENGINE)
    parser.add_argument("--rig", default=DEFAULT_RIG)
    args = parser.parse_args(argv)

    helpers_path = args.helpers.resolve()
    if not helpers_path.is_file():
        print(f"note: helpers export not found, writing twist-only asset: "
              f"{helpers_path}")
        helpers_path = None

    constraints_path = args.constraints.resolve()
    if not constraints_path.is_file():
        print(f"note: constraints export not found, skipping constraints: "
              f"{constraints_path}")
        constraints_path = None

    try:
        document, changed = extract_post_process(
            args.links,
            args.output,
            helpers_path=helpers_path,
            constraints_path=constraints_path,
            asset_id=args.id,
            engine=args.engine,
            rig=args.rig,
        )
    except (OSError, PostProcessExtractError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print_summary(document, args.output.resolve(), changed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
