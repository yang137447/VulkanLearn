#!/usr/bin/env python3
"""Extract ``AnimNode_PoseDriver`` configuration from a UE T3D export.

The Echo post-process AnimBP stores nine PoseDriver nodes.  Each node appears
twice in the T3D: once as an AnimGraph template without a ``Node=`` payload and
once (twice, actually) as an ExecuteUbergraph subobject override that carries
the authoritative ``Node=(...)`` text.  This tool parses the text with a real
bracket-matching parser, deduplicates by node name, and writes the stable
``vl.anim-pose-driver.v1`` asset consumed by the animation runtime tooling.

Usage (from the repository root):

    py -3 tool/animation/extract_pose_driver.py
    py -3 tool/animation/extract_pose_driver.py --input <t3d> --output <json>
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any, Iterable

try:
    from .gasp_anim_common import blake3_file, json_bytes
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from gasp_anim_common import blake3_file, json_bytes


FORMAT = "vl.anim-pose-driver.v1"
DEFAULT_ASSET_ID = "EPD_EchoPostProcess"
DEFAULT_ENGINE = "UE 5.8.4"
DEFAULT_T3D_LABEL = "artifacts/ue-echo-export/anim-audit/Echo_PostProcess_AnimBP.t3d"

# The Echo export must contain exactly these nodes.  The target counts are
# asserted in name order and also checked against the observed table order.
EXPECTED_TARGET_COUNTS: dict[str, int] = {
    "PoseDriver_10": 5,
    "PoseDriver_11": 5,
    "PoseDriver_12": 5,
    "PoseDriver_13": 6,
    "PoseDriver_14": 6,
    "PoseDriver_16": 13,
    "PoseDriver_17": 6,
    "PoseDriver_18": 6,
    "PoseDriver_20": 13,
}

_POSE_DRIVER_NAME_RE = re.compile(r"^AnimGraphNode_PoseDriver_(?P<index>\d+)$")
_NODE_ASSIGNMENT_RE = re.compile(r"^[ \t]*Node[ \t]*=", re.MULTILINE)
_NUMBER_RE = re.compile(r"^[+-]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?$")


class T3DParseError(ValueError):
    """Raised when UE's nested struct text cannot be parsed."""


class PoseDriverExtractError(RuntimeError):
    """Raised when the T3D does not match the expected PoseDriver contract."""


class T3DStruct:
    """Ordered members of a parenthesised UE struct/array.

    A member is ``(key, value)`` for ``Key=Value`` entries and ``(None, value)``
    for array elements.  Keeping the order preserves ``PoseTargets`` and
    ``BoneTransforms`` order, which is semantically significant.
    """

    __slots__ = ("members",)

    def __init__(self, members: Iterable[tuple[str | None, Any]]) -> None:
        self.members = list(members)

    def get(self, key: str, default: Any = None) -> Any:
        for member_key, value in self.members:
            if member_key == key:
                return value
        return default

    def get_member(self, key: str) -> tuple[bool, Any]:
        for member_key, value in self.members:
            if member_key == key:
                return True, value
        return False, None

    def values(self) -> list[Any]:
        return [value for key, value in self.members if key is None]

    def keyed(self) -> list[tuple[str, Any]]:
        return [(key, value) for key, value in self.members if key is not None]

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, T3DStruct):
            return NotImplemented
        return self.members == other.members


# ---------------------------------------------------------------------------
# T3D text parsing
# ---------------------------------------------------------------------------

def decode_t3d(data: bytes) -> str:
    """Decode a T3D payload, preferring UTF-16 when a BOM is present."""

    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        return data.decode("utf-16")
    # BOM-less UTF-16LE is common in UE exports.  Only rely on the heuristic
    # when the NUL distribution is unambiguous; otherwise treat it as UTF-8.
    if len(data) >= 4:
        odd_nuls = data[1::2].count(0)
        even_nuls = data[0::2].count(0)
        if odd_nuls > len(data) // 8 and even_nuls < len(data) // 16:
            return data.decode("utf-16-le")
    return data.decode("utf-8-sig")


def _skip_ws(text: str, index: int) -> int:
    while index < len(text) and text[index].isspace():
        index += 1
    return index


def _parse_string(text: str, index: int) -> tuple[str, int]:
    if text[index] != '"':
        raise T3DParseError(f"expected string at offset {index}")
    index += 1
    chunks: list[str] = []
    while index < len(text):
        char = text[index]
        if char == '"':
            return "".join(chunks), index + 1
        if char == "\\" and index + 1 < len(text):
            escaped = text[index + 1]
            chunks.append({"n": "\n", "r": "\r", "t": "\t"}.get(escaped, escaped))
            index += 2
            continue
        chunks.append(char)
        index += 1
    raise T3DParseError("unterminated quoted string")


def _convert_bare(token: str) -> Any:
    if token == "True":
        return True
    if token == "False":
        return False
    if _NUMBER_RE.match(token):
        try:
            return float(token)
        except ValueError:
            return token
    return token


def _parse_bare(text: str, index: int) -> tuple[Any, int]:
    start = index
    while index < len(text):
        char = text[index]
        if char in ",()=" or char.isspace():
            break
        index += 1
    if index == start:
        raise T3DParseError(f"expected value at offset {index}")
    return _convert_bare(text[start:index]), index


def _try_parse_key(text: str, index: int) -> tuple[str, int] | None:
    """Return ``(key, index_after_equals)`` when a keyed member starts here."""

    index = _skip_ws(text, index)
    if index >= len(text):
        return None
    if text[index] == '"':
        key, after = _parse_string(text, index)
        after = _skip_ws(text, after)
        if after < len(text) and text[after] == "=":
            return key, after + 1
        return None
    if text[index] in ",()=":
        return None
    start = index
    while index < len(text):
        char = text[index]
        if char in "=,()" or char.isspace():
            break
        index += 1
    if index == start:
        return None
    after = _skip_ws(text, index)
    if after < len(text) and text[after] == "=":
        return text[start:index], after + 1
    return None


def _parse_struct(text: str, index: int) -> tuple[T3DStruct, int]:
    if text[index] != "(":
        raise T3DParseError(f"expected '(' at offset {index}")
    index += 1
    members: list[tuple[str | None, Any]] = []
    while True:
        index = _skip_ws(text, index)
        if index >= len(text):
            raise T3DParseError("unterminated struct")
        if text[index] == ")":
            return T3DStruct(members), index + 1

        keyed = _try_parse_key(text, index)
        if keyed is not None:
            key, value_index = keyed
            value, index = _parse_value(text, value_index)
            members.append((key, value))
        else:
            value, index = _parse_value(text, index)
            members.append((None, value))

        index = _skip_ws(text, index)
        if index >= len(text):
            raise T3DParseError("unterminated struct")
        if text[index] == ",":
            index += 1
            continue
        if text[index] == ")":
            return T3DStruct(members), index + 1
        raise T3DParseError(
            f"expected ',' or ')' at offset {index}, found {text[index]!r}"
        )


def _parse_value(text: str, index: int) -> tuple[Any, int]:
    index = _skip_ws(text, index)
    if index >= len(text):
        raise T3DParseError("expected value at end of input")
    if text[index] == "(":
        return _parse_struct(text, index)
    if text[index] == '"':
        return _parse_string(text, index)
    return _parse_bare(text, index)


def parse_t3d_value(text: str, index: int = 0) -> tuple[Any, int]:
    """Parse one UE struct-text value starting at *index*."""

    return _parse_value(text, index)


# ---------------------------------------------------------------------------
# Object block discovery
# ---------------------------------------------------------------------------

def _block_attribute(header: str, name: str) -> str | None:
    match = re.search(rf'\b{name}="([^"]*)"', header)
    return match.group(1) if match else None


def _block_class(header: str) -> str | None:
    match = re.search(r"\bClass=(\S+)", header)
    return match.group(1) if match else None


def iter_pose_driver_blocks(text: str) -> Iterable[tuple[str, str]]:
    """Yield ``(short_name, block_text)`` for every PoseDriver object block."""

    lines = text.splitlines(keepends=True)
    stack: list[tuple[str | None, str | None, int]] = []
    for line_index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("Begin Object"):
            header = stripped[len("Begin Object"):]
            stack.append((_block_attribute(header, "Name"), _block_class(header), line_index))
            continue
        if not stripped.startswith("End Object"):
            continue
        if not stack:
            raise PoseDriverExtractError("'End Object' without a matching 'Begin Object'")
        name, class_name, start = stack.pop()
        if name is None:
            continue
        match = _POSE_DRIVER_NAME_RE.match(name)
        if match is None:
            continue
        if class_name is not None and not class_name.endswith("AnimGraphNode_PoseDriver"):
            continue
        block = "".join(lines[start:line_index + 1])
        yield f"PoseDriver_{match.group('index')}", block


def extract_node_payload(block: str) -> T3DStruct | None:
    """Return the ``Node=(...)`` struct from an object block, if present."""

    matches = list(_NODE_ASSIGNMENT_RE.finditer(block))
    if not matches:
        return None
    if len(matches) > 1:
        raise PoseDriverExtractError("PoseDriver block contains more than one Node= payload")
    value, end = parse_t3d_value(block, matches[0].end())
    if not isinstance(value, T3DStruct):
        raise PoseDriverExtractError("PoseDriver Node= payload must be a struct")
    trailing = block[end:].splitlines()[0] if block[end:] else ""
    if trailing.strip():
        raise PoseDriverExtractError(
            f"trailing text after PoseDriver Node= payload: {trailing.strip()[:40]!r}"
        )
    return value


# ---------------------------------------------------------------------------
# Field conversion
# ---------------------------------------------------------------------------

def _require_struct(value: Any, context: str) -> T3DStruct:
    if not isinstance(value, T3DStruct):
        raise PoseDriverExtractError(f"{context} must be a struct")
    return value


def _require_string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise PoseDriverExtractError(f"{context} must be a non-empty string")
    return value


def _require_number(value: Any, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise PoseDriverExtractError(f"{context} must be numeric")
    number = float(value)
    return 0.0 if number == 0.0 else number


def _require_bool(value: Any, context: str) -> bool:
    if isinstance(value, bool):
        return value
    if value in (0, 1):
        return bool(value)
    raise PoseDriverExtractError(f"{context} must be a boolean")


def _enum(value: Any, context: str, default: str | None = None) -> str:
    if value is None:
        if default is None:
            raise PoseDriverExtractError(f"{context} is missing")
        return default
    return _require_string(value, context)


def _vector(value: Any, context: str, *, default: tuple[float, float, float]) -> list[float]:
    if value is None:
        return list(default)
    struct = _require_struct(value, context)
    return [
        _require_number(struct.get("X"), f"{context}.X"),
        _require_number(struct.get("Y"), f"{context}.Y"),
        _require_number(struct.get("Z"), f"{context}.Z"),
    ]


def _struct_list(value: Any, context: str, single_key: str) -> list[T3DStruct]:
    """Flatten a UE struct array such as ``((...),(...))``."""

    struct = _require_struct(value, context)
    if struct.get(single_key) is not None:
        return [struct]
    items = struct.values()
    for item in items:
        if not isinstance(item, T3DStruct):
            raise PoseDriverExtractError(f"{context} contains a non-struct element")
    return items


def _bone_names(value: Any, context: str) -> list[str]:
    if value is None:
        raise PoseDriverExtractError(f"{context} is missing")
    if isinstance(value, str):
        return [_require_string(value, context)]
    return [
        _require_string(item.get("BoneName"), f"{context}.BoneName")
        for item in _struct_list(value, context, "BoneName")
    ]


def _target_transforms(value: Any, context: str) -> list[dict[str, Any]]:
    transforms: list[dict[str, Any]] = []
    for index, item in enumerate(_struct_list(value, context, "TargetTranslation")):
        translation = _require_struct(
            item.get("TargetTranslation"), f"{context}[{index}].TargetTranslation"
        )
        rotation = _require_struct(
            item.get("TargetRotation"), f"{context}[{index}].TargetRotation"
        )
        transforms.append(
            {
                "translation": [
                    _require_number(translation.get("X"), f"{context}[{index}].X"),
                    _require_number(translation.get("Y"), f"{context}[{index}].Y"),
                    _require_number(translation.get("Z"), f"{context}[{index}].Z"),
                ],
                "rotation": {
                    "pitch": _require_number(rotation.get("Pitch"), f"{context}[{index}].Pitch"),
                    "yaw": _require_number(rotation.get("Yaw"), f"{context}[{index}].Yaw"),
                    "roll": _require_number(rotation.get("Roll"), f"{context}[{index}].Roll"),
                },
            }
        )
    return transforms


def _normalize_drive_output(value: str) -> str:
    normalized = value[len("Drive"):] if value.startswith("Drive") else value
    if normalized == "Poses":
        raise PoseDriverExtractError(
            "PoseDriver driveOutput is 'Poses'; only DriveCurves is supported"
        )
    if normalized != "Curves":
        raise PoseDriverExtractError(f"unsupported PoseDriver driveOutput {value!r}")
    return normalized


def driver_from_node(name: str, node: T3DStruct) -> dict[str, Any]:
    """Convert one parsed ``Node=`` struct into the asset driver schema."""

    source_bones = _bone_names(node.get("SourceBones"), f"{name}.SourceBones")

    eval_space = node.get("EvalSpaceBone")
    if eval_space is None:
        eval_space_bone = ""
    elif isinstance(eval_space, str):
        eval_space_bone = _require_string(eval_space, f"{name}.EvalSpaceBone")
    else:
        eval_space_struct = _require_struct(eval_space, f"{name}.EvalSpaceBone")
        eval_space_bone = _require_string(
            eval_space_struct.get("BoneName"), f"{name}.EvalSpaceBone.BoneName"
        )

    rbf_value = node.get("RBFParams")
    if rbf_value is None:
        raise PoseDriverExtractError(f"{name}.RBFParams is missing")
    rbf = _require_struct(rbf_value, f"{name}.RBFParams")

    targets: list[dict[str, Any]] = []
    target_values = _struct_list(node.get("PoseTargets"), f"{name}.PoseTargets", "BoneTransforms")
    for target_index, target in enumerate(target_values):
        context = f"{name}.PoseTargets[{target_index}]"
        transforms = _target_transforms(
            target.get("BoneTransforms"), f"{context}.BoneTransforms"
        )
        if len(transforms) != len(source_bones):
            raise PoseDriverExtractError(
                f"{context} has {len(transforms)} transforms for "
                f"{len(source_bones)} source bones"
            )
        targets.append(
            {
                "drivenName": _require_string(target.get("DrivenName"), f"{context}.DrivenName"),
                "scale": _require_number(target.get("TargetScale", 1.0), f"{context}.TargetScale"),
                "distanceMethod": _enum(
                    target.get("DistanceMethod"), f"{context}.DistanceMethod", "Default"
                ),
                "function": _enum(
                    target.get("FunctionType"), f"{context}.FunctionType", "Default"
                ),
                "applyCustomCurve": _require_bool(
                    target.get("bApplyCustomCurve", False), f"{context}.bApplyCustomCurve"
                ),
                "transforms": transforms,
            }
        )

    return {
        "name": name,
        "sourceBones": source_bones,
        "evalSpaceBone": eval_space_bone,
        "evalFromRefPose": _require_bool(
            node.get("bEvalFromRefPose", False), f"{name}.bEvalFromRefPose"
        ),
        "driveSource": _enum(node.get("DriveSource"), f"{name}.DriveSource", "Rotation"),
        "driveOutput": _normalize_drive_output(
            _enum(node.get("DriveOutput"), f"{name}.DriveOutput")
        ),
        "rbf": {
            "solverType": _enum(rbf.get("SolverType"), f"{name}.RBFParams.SolverType"),
            "radius": _require_number(rbf.get("Radius"), f"{name}.RBFParams.Radius"),
            "automaticRadius": _require_bool(
                rbf.get("AutomaticRadius", False), f"{name}.RBFParams.AutomaticRadius"
            ),
            "function": _enum(rbf.get("Function"), f"{name}.RBFParams.Function"),
            "distanceMethod": _enum(
                rbf.get("DistanceMethod"), f"{name}.RBFParams.DistanceMethod", "SwingAngle"
            ),
            "twistAxis": _enum(rbf.get("TwistAxis"), f"{name}.RBFParams.TwistAxis", "X"),
            "weightThreshold": _require_number(
                rbf.get("WeightThreshold", 0.0001), f"{name}.RBFParams.WeightThreshold"
            ),
            "normalizeMethod": _enum(
                rbf.get("NormalizeMethod"),
                f"{name}.RBFParams.NormalizeMethod",
                "OnlyNormalizeAboveOne",
            ),
            "medianReference": _vector(
                rbf.get("MedianReference"),
                f"{name}.RBFParams.MedianReference",
                default=(0.0, 0.0, 0.0),
            ),
            "medianMin": _require_number(
                rbf.get("MedianMin", 45.0), f"{name}.RBFParams.MedianMin"
            ),
            "medianMax": _require_number(
                rbf.get("MedianMax", 60.0), f"{name}.RBFParams.MedianMax"
            ),
        },
        "targets": targets,
    }


# ---------------------------------------------------------------------------
# Document assembly
# ---------------------------------------------------------------------------

def _driver_sort_key(driver: dict[str, Any]) -> int:
    return int(driver["name"].rsplit("_", 1)[1])


def validate_drivers(
    drivers: list[dict[str, Any]], expected_counts: dict[str, int]
) -> None:
    observed = {driver["name"]: len(driver["targets"]) for driver in drivers}
    if set(observed) != set(expected_counts):
        missing = sorted(set(expected_counts) - set(observed))
        extra = sorted(set(observed) - set(expected_counts))
        raise PoseDriverExtractError(
            "PoseDriver set mismatch: "
            f"observed {sorted(observed)}, expected {sorted(expected_counts)}; "
            f"missing={missing}, extra={extra}"
        )
    mismatched = [
        f"{name}: observed {observed[name]}, expected {expected_counts[name]}"
        for name in expected_counts
        if observed[name] != expected_counts[name]
    ]
    if mismatched:
        raise PoseDriverExtractError("PoseDriver target count mismatch: " + "; ".join(mismatched))


def parse_pose_drivers(
    text: str,
    *,
    expected_counts: dict[str, int] | None = None,
    engine: str = DEFAULT_ENGINE,
    t3d_label: str = DEFAULT_T3D_LABEL,
    t3d_blake3: str | None = None,
    asset_id: str = DEFAULT_ASSET_ID,
) -> dict[str, Any]:
    """Parse a decoded T3D and return the full ``vl.anim-pose-driver.v1`` document."""

    by_name: dict[str, dict[str, Any]] = {}
    for name, block in iter_pose_driver_blocks(text):
        node = extract_node_payload(block)
        if node is None:
            # AnimGraph template block: the authoritative payload is only in
            # the ExecuteUbergraph copies.
            continue
        driver = driver_from_node(name, node)
        previous = by_name.get(name)
        if previous is not None and previous != driver:
            raise PoseDriverExtractError(
                f"duplicate PoseDriver {name} blocks disagree"
            )
        by_name[name] = driver

    drivers = sorted(by_name.values(), key=_driver_sort_key)
    validate_drivers(
        drivers, EXPECTED_TARGET_COUNTS if expected_counts is None else expected_counts
    )

    source: dict[str, Any] = {"engine": engine, "t3d": t3d_label}
    if t3d_blake3 is not None:
        source["t3dBlake3"] = t3d_blake3
    return {
        "format": FORMAT,
        "id": asset_id,
        "source": source,
        "drivers": drivers,
    }


def extract_pose_driver(
    t3d_path: Path,
    output_path: Path | None = None,
    *,
    expected_counts: dict[str, int] | None = None,
    engine: str = DEFAULT_ENGINE,
    t3d_label: str | None = None,
    asset_id: str = DEFAULT_ASSET_ID,
) -> tuple[dict[str, Any], bool]:
    """Read *t3d_path*, build the asset document and optionally write it.

    Returns ``(document, changed)``.  The JSON is UTF-8, 2-space indented, LF
    terminated and byte-stable across runs.
    """

    t3d_path = t3d_path.resolve()
    data = t3d_path.read_bytes()
    text = decode_t3d(data)
    document = parse_pose_drivers(
        text,
        expected_counts=expected_counts,
        engine=engine,
        t3d_label=t3d_label if t3d_label is not None else DEFAULT_T3D_LABEL,
        t3d_blake3=blake3_file(t3d_path),
        asset_id=asset_id,
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
    sibling_resources = repo_root.parent / "VukanLearnResources"
    return (
        repo_root / DEFAULT_T3D_LABEL,
        sibling_resources / "Maps" / "SC_valley_echo" / "Rig" / "EPD_EchoPostProcess.json",
    )


def print_summary(document: dict[str, Any], output_path: Path | None, changed: bool) -> None:
    print(f"{'driver':<16}{'sourceBone':<16}{'radius':>8}  {'solver':<16}{'targets':>8}")
    print("-" * 68)
    for driver in document["drivers"]:
        source = ",".join(driver["sourceBones"])
        print(
            f"{driver['name']:<16}{source:<16}{driver['rbf']['radius']:>8.1f}  "
            f"{driver['rbf']['solverType']:<16}{len(driver['targets']):>8}"
        )
    total_targets = sum(len(driver["targets"]) for driver in document["drivers"])
    print("-" * 68)
    print(f"drivers={len(document['drivers'])} targets={total_targets}")
    print(f"blake3={document['source'].get('t3dBlake3', '<missing>')}")
    if output_path is not None:
        state = "written" if changed else "unchanged"
        print(f"{state}: {output_path}")


def main(argv: list[str] | None = None) -> int:
    default_input, default_output = _default_paths()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=default_input)
    parser.add_argument("--output", type=Path, default=default_output)
    parser.add_argument("--engine", default=DEFAULT_ENGINE)
    parser.add_argument("--id", default=DEFAULT_ASSET_ID)
    parser.add_argument("--t3d-label", default=None)
    args = parser.parse_args(argv)

    try:
        document, changed = extract_pose_driver(
            args.input,
            args.output,
            engine=args.engine,
            t3d_label=args.t3d_label,
            asset_id=args.id,
        )
    except (OSError, T3DParseError, PoseDriverExtractError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print_summary(document, args.output.resolve(), changed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
