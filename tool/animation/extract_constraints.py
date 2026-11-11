#!/usr/bin/env python3
"""Extract ``AnimNode_Constraint`` nodes from a UE T3D export.

The Echo PostProcess AnimBP pins three scarf/pouch/canister helper bones to a
weighted blend of spine bones.  UE stores the payload in the T3D object block as
a ``Node=(...)`` struct; this tool parses that text with the bracket-matching
parser shared with ``extract_pose_driver.py``, groups the AnimGraph and
ExecuteUbergraph copies by object name, and writes the stable
``vl.anim-constraints.v1`` asset.

UE defaults that are not serialized are resolved explicitly:

* ``FConstraint::OffsetOption`` defaults to ``Offset_RefPose`` (RefPose offset).
* ``FConstraint::PerAxis`` defaults to ``bX=bY=bZ=true``.
* ``FConstraint::TransformType`` defaults to ``Translation``; Echo only uses
  ``Parent``, and anything else is rejected instead of approximated.

Any value outside that contract is an error: this tool must not silently
approximate the PostProcess chain.

Duplicate copies
----------------

The T3D serializes each node twice: once in the ``AnimGraph`` page (the editor
graph) and once in the generated ``ExecuteUbergraph_*`` page.  For Echo's
``AnimGraphNode_Constraint_4`` those two copies disagree (``1,1,1`` versus
``0.05,0.25,0.7``).  Exporting the compiled ``Echo_PostProcess_AnimBP_C`` CDO
shows ``ConstraintWeights=(0.050000,0.250000,0.700000)``, so the Ubergraph copy
is the runtime truth and the AnimGraph page copy is stale.  The tool therefore
prefers the Ubergraph copy by default and records every divergence under
``source.divergences`` instead of dropping it silently.  ``--prefer-animgraph``
reproduces the stale authored values for comparison only.

Usage (from the repository root):

    py -3 tool/animation/extract_constraints.py
    py -3 tool/animation/extract_constraints.py --t3d <t3d> --output <json>
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any, Iterable

try:
    from .gasp_anim_common import blake3_file, json_bytes
    from .extract_pose_driver import (
        T3DParseError,
        T3DStruct,
        decode_t3d,
        parse_t3d_value,
    )
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from gasp_anim_common import blake3_file, json_bytes
    from extract_pose_driver import (
        T3DParseError,
        T3DStruct,
        decode_t3d,
        parse_t3d_value,
    )


FORMAT = "vl.anim-constraints.v1"
DEFAULT_ASSET_ID = "EPP_EchoPostProcess.constraints"
DEFAULT_ENGINE = "UE 5.8.4"
DEFAULT_T3D_LABEL = (
    "artifacts/ue-echo-export/anim-audit/Echo_PostProcess_AnimBP.t3d"
)

TRANSFORM_TYPE_PARENT = "Parent"
OFFSET_OPTION_REF_POSE = "RefPose"
CONTAINER_UBERGRAPH = "ubergraph"
CONTAINER_ANIMGRAPH = "animgraph"
PREFER_UBERGRAPH = "ubergraph"
PREFER_ANIMGRAPH = "animgraph"

# ``EConstraintOffsetOption`` only has ``None`` and ``Offset_RefPose``; the
# asset schema spells the accepted value ``RefPose``.
_REF_POSE_ALIASES = {"Offset_RefPose", "RefPose"}
_NON_REF_POSE = {"None", "No_Offset"}

# Exact Echo contract, asserted in bone order.  Values are the compiled CDO
# values (verified by exporting Echo_PostProcess_AnimBP_C), not the stale
# AnimGraph page copy.
EXPECTED_CONSTRAINTS: dict[str, tuple[tuple[str, float], ...]] = {
    "C_Scarf_A_Root_Jnt": (("spine_05", 0.49), ("clavicle_l", 0.51)),
    "R_Canister_A_Joint1_Jnt": (
        ("spine_02", 0.212),
        ("spine_01", 0.283),
        ("pelvis", 0.505),
    ),
    "R_LargePouch_A_Joint1_Jnt": (
        ("spine_02", 0.05),
        ("spine_01", 0.25),
        ("pelvis", 0.7),
    ),
}

_CONSTRAINT_NAME_RE = re.compile(r"^AnimGraphNode_Constraint_(?P<index>\d+)$")
_NODE_ASSIGNMENT_RE = re.compile(r"^[ \t]*Node[ \t]*=", re.MULTILINE)


class ConstraintsExtractError(RuntimeError):
    """Raised when the T3D does not match the expected constraint contract."""


# ---------------------------------------------------------------------------
# Object block discovery
# ---------------------------------------------------------------------------

def _block_attribute(header: str, name: str) -> str | None:
    match = re.search(rf'\b{name}="([^"]*)"', header)
    return match.group(1) if match else None


def _block_class(header: str) -> str | None:
    match = re.search(r"\bClass=(\S+)", header)
    return match.group(1) if match else None


def _container(header: str) -> str:
    """Classify a node block by its owning generated graph."""

    if ":ExecuteUbergraph_" in header:
        return CONTAINER_UBERGRAPH
    return CONTAINER_ANIMGRAPH


def iter_constraint_blocks(text: str) -> Iterable[tuple[str, str, str]]:
    """Yield ``(short_name, block_text, container)`` for Constraint blocks."""

    lines = text.splitlines(keepends=True)
    stack: list[tuple[str | None, str | None, int]] = []
    for line_index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("Begin Object"):
            header = stripped[len("Begin Object"):]
            stack.append(
                (_block_attribute(header, "Name"), _block_class(header), line_index)
            )
            continue
        if not stripped.startswith("End Object"):
            continue
        if not stack:
            raise ConstraintsExtractError(
                "'End Object' without a matching 'Begin Object'"
            )
        name, class_name, start = stack.pop()
        if name is None:
            continue
        match = _CONSTRAINT_NAME_RE.match(name)
        if match is None:
            continue
        if class_name is not None and not class_name.endswith(
            "AnimGraphNode_Constraint"
        ):
            continue
        block = "".join(lines[start:line_index + 1])
        yield (
            f"AnimGraphNode_Constraint_{match.group('index')}",
            block,
            _container(block.splitlines()[0]),
        )


def extract_node_payload(block: str) -> T3DStruct | None:
    """Return the ``Node=(...)`` struct from an object block, if present."""

    matches = list(_NODE_ASSIGNMENT_RE.finditer(block))
    if not matches:
        return None
    if len(matches) > 1:
        raise ConstraintsExtractError(
            "Constraint block contains more than one Node= payload"
        )
    value, end = parse_t3d_value(block, matches[0].end())
    if not isinstance(value, T3DStruct):
        raise ConstraintsExtractError("Constraint Node= payload must be a struct")
    trailing = block[end:].splitlines()[0] if block[end:] else ""
    if trailing.strip():
        raise ConstraintsExtractError(
            f"trailing text after Constraint Node= payload: "
            f"{trailing.strip()[:40]!r}"
        )
    return value


# ---------------------------------------------------------------------------
# Field conversion
# ---------------------------------------------------------------------------

def _require_struct(value: Any, context: str) -> T3DStruct:
    if not isinstance(value, T3DStruct):
        raise ConstraintsExtractError(f"{context} must be a struct")
    return value


def _require_string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise ConstraintsExtractError(f"{context} must be a non-empty string")
    return value


def _require_number(value: Any, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConstraintsExtractError(f"{context} must be numeric")
    number = float(value)
    if number != number or number in (float("inf"), float("-inf")):
        raise ConstraintsExtractError(f"{context} must be finite")
    return 0.0 if number == 0.0 else number


def _require_bool(value: Any, context: str) -> bool:
    if isinstance(value, bool):
        return value
    if value in (0, 1):
        return bool(value)
    raise ConstraintsExtractError(f"{context} must be a boolean")


def _struct_array(value: Any, context: str) -> list[T3DStruct]:
    """Flatten a UE struct array such as ``((..),(..))``."""

    array = _require_struct(value, context)
    if array.keyed():
        raise ConstraintsExtractError(
            f"{context} must be an array, got keyed members "
            f"{[key for key, _ in array.keyed()]}"
        )
    elements: list[T3DStruct] = []
    for position, element in enumerate(array.values()):
        elements.append(_require_struct(element, f"{context}[{position}]"))
    return elements


def _number_array(value: Any, context: str) -> list[float]:
    array = _require_struct(value, context)
    if array.keyed():
        raise ConstraintsExtractError(
            f"{context} must be an array, got keyed members "
            f"{[key for key, _ in array.keyed()]}"
        )
    return [
        _require_number(element, f"{context}[{position}]")
        for position, element in enumerate(array.values())
    ]


def _bone_name(value: Any, context: str) -> str:
    struct = _require_struct(value, context)
    return _require_string(struct.get("BoneName"), f"{context}.BoneName")


def _transform_type(target: T3DStruct, context: str) -> str:
    value = target.get("TransformType")
    # UE's FConstraint default is Translation, but Echo authors ``Parent``
    # explicitly; a missing value means the export drifted.
    if value is None:
        raise ConstraintsExtractError(f"{context}.TransformType is missing")
    transform_type = _require_string(value, f"{context}.TransformType")
    if transform_type != TRANSFORM_TYPE_PARENT:
        raise ConstraintsExtractError(
            f"{context}.TransformType must be {TRANSFORM_TYPE_PARENT!r}, "
            f"got {transform_type!r}"
        )
    return transform_type


def _offset_option(target: T3DStruct, context: str) -> str:
    value = target.get("OffsetOption")
    if value is None:
        # UE default: missing OffsetOption means Offset_RefPose.
        return OFFSET_OPTION_REF_POSE
    option = _require_string(value, f"{context}.OffsetOption")
    if option in _REF_POSE_ALIASES:
        return OFFSET_OPTION_REF_POSE
    if option in _NON_REF_POSE:
        raise ConstraintsExtractError(
            f"{context}.OffsetOption must be RefPose, got {option!r}"
        )
    raise ConstraintsExtractError(
        f"{context}.OffsetOption has unknown value {option!r}"
    )


def _per_axis_all_true(target: T3DStruct, context: str) -> None:
    value = target.get("PerAxis")
    if value is None:
        # UE default: FFilterOptionPerAxis(bX=bY=bZ=true).
        return
    per_axis = _require_struct(value, f"{context}.PerAxis")
    for axis in ("bX", "bY", "bZ"):
        axis_value = per_axis.get(axis)
        if axis_value is None:
            continue
        if not _require_bool(axis_value, f"{context}.PerAxis.{axis}"):
            raise ConstraintsExtractError(
                f"{context}.PerAxis.{axis} must be true; per-axis filtering is "
                f"not supported"
            )


def parse_constraint_node(node: T3DStruct, label: str) -> dict[str, Any]:
    """Convert one ``Node=(...)`` payload into the asset entry."""

    if not isinstance(node, T3DStruct):
        raise ConstraintsExtractError(f"{label} must be a struct")
    bone = _bone_name(node.get("BoneToModify"), f"{label}.BoneToModify")

    setups = _struct_array(node.get("ConstraintSetup"), f"{label}.ConstraintSetup")
    weights = _number_array(
        node.get("ConstraintWeights"), f"{label}.ConstraintWeights"
    )
    if not setups:
        raise ConstraintsExtractError(f"{label} has no constraint targets")
    if len(setups) != len(weights):
        raise ConstraintsExtractError(
            f"{label} has {len(setups)} targets but {len(weights)} weights"
        )

    targets: list[dict[str, Any]] = []
    seen: set[str] = set()
    offset_options: set[str] = set()
    for position, (setup, weight) in enumerate(zip(setups, weights)):
        context = f"{label}.ConstraintSetup[{position}]"
        target_bone = _bone_name(setup.get("TargetBone"), f"{context}.TargetBone")
        if target_bone == bone:
            raise ConstraintsExtractError(
                f"{label} constrains {bone!r} to itself"
            )
        if target_bone in seen:
            raise ConstraintsExtractError(
                f"{label} targets {target_bone!r} more than once"
            )
        seen.add(target_bone)
        _transform_type(setup, context)
        _per_axis_all_true(setup, context)
        offset_option = _offset_option(setup, context)
        offset_options.add(offset_option)
        if weight < 0.0 or weight > 1.0:
            raise ConstraintsExtractError(
                f"{label} weight for {target_bone!r} must be in [0, 1], "
                f"got {weight}"
            )
        targets.append({"bone": target_bone, "weight": weight})

    if len(offset_options) != 1:
        raise ConstraintsExtractError(
            f"{label} mixes offset options {sorted(offset_options)}"
        )
    offset_option = offset_options.pop()

    return {
        "name": bone,
        "bone": bone,
        "transformType": TRANSFORM_TYPE_PARENT,
        "offsetOption": offset_option,
        "targets": targets,
    }


def _natural_key(value: str) -> list[Any]:
    return [
        int(part) if part.isdigit() else part
        for part in re.split(r"(\d+)", value)
    ]


def parse_constraints(
    text: str,
    *,
    t3d_label: str = DEFAULT_T3D_LABEL,
    t3d_blake3: str = "0" * 64,
    asset_id: str = DEFAULT_ASSET_ID,
    engine: str = DEFAULT_ENGINE,
    validate: bool = True,
    expected_counts: dict[str, int] | None = None,
    prefer: str = PREFER_UBERGRAPH,
) -> dict[str, Any]:
    """Parse *text* into the constraint asset document.

    *prefer* selects which duplicate wins when the AnimGraph and Ubergraph
    copies disagree; the divergence is always recorded.
    """

    if prefer not in (PREFER_UBERGRAPH, PREFER_ANIMGRAPH):
        raise ConstraintsExtractError(
            f"prefer must be {PREFER_UBERGRAPH!r} or {PREFER_ANIMGRAPH!r}, "
            f"got {prefer!r}"
        )

    payloads: dict[str, dict[str, T3DStruct]] = {}
    counts: dict[str, int] = {}
    for name, block, container in iter_constraint_blocks(text):
        counts[name] = counts.get(name, 0) + 1
        node = extract_node_payload(block)
        if node is None:
            continue
        by_container = payloads.setdefault(name, {})
        existing = by_container.get(container)
        if existing is not None and existing != node:
            raise ConstraintsExtractError(
                f"{name} has conflicting Node= payloads inside the "
                f"{container} copy"
            )
        by_container[container] = node

    if expected_counts is not None and counts != expected_counts:
        raise ConstraintsExtractError(
            f"object-block counts mismatch; expected {expected_counts}, "
            f"got {counts}"
        )
    if not payloads:
        raise ConstraintsExtractError("no AnimGraphNode_Constraint Node= payloads")

    order = (PREFER_UBERGRAPH, PREFER_ANIMGRAPH)
    if prefer == PREFER_ANIMGRAPH:
        order = (PREFER_ANIMGRAPH, PREFER_UBERGRAPH)

    constraints: list[dict[str, Any]] = []
    divergences: list[dict[str, Any]] = []
    for name in sorted(payloads, key=_natural_key):
        by_container = payloads[name]
        chosen_container = next(
            (candidate for candidate in order if candidate in by_container),
            None,
        )
        if chosen_container is None:
            raise ConstraintsExtractError(f"{name} has no usable Node= payload")
        chosen = by_container[chosen_container]
        other_container = (
            CONTAINER_ANIMGRAPH
            if chosen_container == CONTAINER_UBERGRAPH
            else CONTAINER_UBERGRAPH
        )
        other = by_container.get(other_container)
        if other is not None and other != chosen:
            divergences.append(
                {
                    "node": name,
                    "chosen": chosen_container,
                    "chosenWeights": _weights_of(chosen),
                    "other": other_container,
                    "otherWeights": _weights_of(other),
                }
            )
        constraints.append(parse_constraint_node(chosen, f"{name}.Node"))

    constraints.sort(key=lambda entry: _natural_key(entry["bone"]))
    if validate:
        validate_echo_constraints(constraints)

    source: dict[str, Any] = {
        "engine": engine,
        "t3d": t3d_label,
        "t3dBlake3": t3d_blake3,
    }
    if divergences:
        source["divergences"] = divergences

    return {
        "format": FORMAT,
        "id": asset_id,
        "source": source,
        "constraints": constraints,
    }


def _weights_of(node: T3DStruct) -> list[float]:
    """Best-effort weight extraction for the divergence report."""

    try:
        return _number_array(
            node.get("ConstraintWeights"), "ConstraintWeights"
        )
    except ConstraintsExtractError:
        return []


def validate_echo_constraints(constraints: list[dict[str, Any]]) -> None:
    """Assert the exact Echo constraint bone/weight contract."""

    if len(constraints) != len(EXPECTED_CONSTRAINTS):
        raise ConstraintsExtractError(
            f"expected {len(EXPECTED_CONSTRAINTS)} constraints, got "
            f"{len(constraints)}: {[entry['bone'] for entry in constraints]}"
        )

    actual_names = [entry["bone"] for entry in constraints]
    expected_names = sorted(EXPECTED_CONSTRAINTS, key=_natural_key)
    if actual_names != expected_names:
        raise ConstraintsExtractError(
            f"constraint set mismatch; expected {expected_names}, "
            f"got {actual_names}"
        )

    for entry in constraints:
        name = entry["bone"]
        if entry["name"] != name:
            raise ConstraintsExtractError(
                f"constraint {name!r} has mismatched name {entry['name']!r}"
            )
        if entry["transformType"] != TRANSFORM_TYPE_PARENT:
            raise ConstraintsExtractError(
                f"constraint {name!r} transform type must be Parent"
            )
        if entry["offsetOption"] != OFFSET_OPTION_REF_POSE:
            raise ConstraintsExtractError(
                f"constraint {name!r} offset option must be RefPose"
            )

        actual_targets = [
            (target["bone"], target["weight"]) for target in entry["targets"]
        ]
        expected_targets = list(EXPECTED_CONSTRAINTS[name])
        if len(actual_targets) != len(expected_targets):
            raise ConstraintsExtractError(
                f"constraint {name!r} target count mismatch: "
                f"expected {len(expected_targets)}, got {len(actual_targets)}"
            )
        for (actual_bone, actual_weight), (expected_bone, expected_weight) in zip(
            actual_targets, expected_targets
        ):
            if actual_bone != expected_bone:
                raise ConstraintsExtractError(
                    f"constraint {name!r} target mismatch: expected "
                    f"{expected_bone!r}, got {actual_bone!r}"
                )
            if abs(actual_weight - expected_weight) > 1e-9:
                raise ConstraintsExtractError(
                    f"constraint {name!r} weight for {expected_bone!r} "
                    f"mismatch: expected {expected_weight}, got {actual_weight}"
                )


def _repo_relative_label(path: Path) -> str:
    repo_root = Path(__file__).resolve().parents[2]
    try:
        return path.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def extract_constraints(
    t3d_path: Path,
    output_path: Path | None = None,
    *,
    asset_id: str = DEFAULT_ASSET_ID,
    engine: str = DEFAULT_ENGINE,
    validate: bool = True,
    expected_counts: dict[str, int] | None = None,
    prefer: str = PREFER_UBERGRAPH,
) -> tuple[dict[str, Any], bool]:
    """Read *t3d_path*, build the asset and optionally write it.

    Returns ``(document, changed)``.  The JSON is UTF-8, 2-space indented, LF
    terminated and byte-stable across runs.
    """

    t3d_path = t3d_path.resolve()
    raw = t3d_path.read_bytes()
    try:
        text = decode_t3d(raw)
    except (UnicodeDecodeError, T3DParseError) as error:
        raise ConstraintsExtractError(f"cannot decode {t3d_path}: {error}") from error

    document = parse_constraints(
        text,
        t3d_label=_repo_relative_label(t3d_path),
        t3d_blake3=blake3_file(t3d_path),
        asset_id=asset_id,
        engine=engine,
        validate=validate,
        expected_counts=expected_counts,
        prefer=prefer,
    )
    if output_path is None:
        return document, False

    output_path = output_path.resolve()
    payload = json_bytes(document)
    changed = True
    if output_path.exists():
        changed = output_path.read_bytes() != payload
    if changed:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_bytes(payload)
    return document, changed


def _default_paths() -> tuple[Path, Path]:
    repo_root = Path(__file__).resolve().parents[2]
    return (
        repo_root / DEFAULT_T3D_LABEL,
        repo_root / "artifacts" / "ue-echo-export" / "constraints.json",
    )


def print_summary(
    document: dict[str, Any],
    output_path: Path | None,
    changed: bool,
) -> None:
    print(f"{'bone':<30}{'targets'}")
    print("-" * 84)
    for entry in document["constraints"]:
        targets = ",".join(
            f"{target['bone']}@{target['weight']:.6g}"
            for target in entry["targets"]
        )
        print(f"{entry['bone']:<30}{targets}")
    print("-" * 84)
    total = sum(len(entry["targets"]) for entry in document["constraints"])
    print(f"constraints={len(document['constraints'])} targets={total}")
    for divergence in document["source"].get("divergences", []):
        print(
            f"note: {divergence['node']} copies disagree; using "
            f"{divergence['chosen']} weights {divergence['chosenWeights']} "
            f"(other {divergence['other']} weights "
            f"{divergence['otherWeights']})"
        )
    if output_path is not None:
        state = "written" if changed else "unchanged"
        print(f"{state}: {output_path}")


def main(argv: list[str] | None = None) -> int:
    default_t3d, default_output = _default_paths()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--t3d", type=Path, default=default_t3d)
    parser.add_argument("--output", type=Path, default=default_output)
    parser.add_argument("--id", default=DEFAULT_ASSET_ID)
    parser.add_argument("--engine", default=DEFAULT_ENGINE)
    parser.add_argument(
        "--prefer",
        choices=(PREFER_UBERGRAPH, PREFER_ANIMGRAPH),
        default=PREFER_UBERGRAPH,
        help="duplicate copy to trust when the two T3D graphs disagree",
    )
    parser.add_argument(
        "--no-validate",
        action="store_true",
        help="skip the Echo bone/weight contract (comparison only)",
    )
    args = parser.parse_args(argv)

    try:
        document, changed = extract_constraints(
            args.t3d,
            args.output,
            asset_id=args.id,
            engine=args.engine,
            validate=not args.no_validate,
            prefer=args.prefer,
        )
    except (OSError, ConstraintsExtractError, T3DParseError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print_summary(document, args.output.resolve(), changed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
