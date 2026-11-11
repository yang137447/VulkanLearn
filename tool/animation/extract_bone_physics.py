#!/usr/bin/env python3
"""Extract UE PhysicsAsset T3D exports into ``vl.bone-physics.v1``.

Echo's PostProcess chain drives four secondary-motion physics assets
(``PA_Echo_Canister``, ``PA_Echo_Pouch``, ``PA_Echo_ShoulderPad`` and
``PA_Echo_Skirt``).  This tool reads the ``unreal.ObjectExporterT3D`` dumps,
walks the nested ``Begin Object`` / ``End Object`` blocks with an explicit
stack, and writes one deterministic JSON asset.

Data conventions:

* ``SkeletalBodySetups(i)`` / ``ConstraintSetup(i)`` define body and constraint
  order.  Block order in the T3D is *not* used.
* Positions, capsule centers and frames stay in raw UE space; rotations are
  converted with UE's own ``FRotator::Quaternion()`` formula.  The runtime owns
  the UE -> VL axis conversion.
* ``FKSphylElem`` defaults (``Radius=1``, ``Length=1``) are applied when the T3D
  omits them; ``halfHeight`` is the cylinder half length (``Length / 2``).
* ``FConeConstraint`` / ``FTwistConstraint`` default to 45 degrees and free
  motion.  Missing ``Stiffness`` / ``Damping`` export as 0, matching the asset
  contract used by the Echo runtime.
* ``ConstraintBone1`` is documented by UE as the child bone and
  ``ConstraintBone2`` as the parent, but the Echo assets mix the order for
  three joints.  The exporter resolves the child as the *simulated* body and
  the parent as the *kinematic* body, then moves each Pos/Axis frame with its
  body.  Anything else is an error instead of a silent approximation.

Source anomaly: ``PA_Echo_Skirt`` marks ``C_Skirt_A_Back1_Jnt`` as
``PhysType_Kinematic`` even though it carries simulated-style mass and damping,
so its first ``UserConstraint`` joins two kinematic bodies.  The exporter keeps
that constraint (the asset contract counts six) and reports it as a warning;
``--strict`` turns the warning into an error.  The runtime should skip joints
without a simulated body instead of inventing one.

Usage (from the repository root):

    py -3 tool/animation/extract_bone_physics.py
    py -3 tool/animation/extract_bone_physics.py --dir <dir> --output <json>
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

try:
    from .gasp_anim_common import blake3_file, json_bytes
    from .extract_pose_driver import T3DParseError, T3DStruct, parse_t3d_value
except ImportError:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from gasp_anim_common import blake3_file, json_bytes
    from extract_pose_driver import T3DParseError, T3DStruct, parse_t3d_value


FORMAT = "vl.bone-physics.v1"
DEFAULT_ASSET_ID = "BPH_EchoPostProcess"

# Stable output order and the exact Echo contract asserted on every run.
ASSET_SPECS: tuple[tuple[str, str, int, int], ...] = (
    ("PA_Echo_Canister", "canister", 5, 1),
    ("PA_Echo_Pouch", "pouch", 2, 1),
    ("PA_Echo_ShoulderPad", "shoulderPad", 2, 1),
    ("PA_Echo_Skirt", "skirt", 12, 6),
)

BASE_BONE = "root"
GRAVITY = [0.0, -980.0, 0.0]

_PHYSICS_TYPE = {
    "PhysType_Kinematic": "kinematic",
    "PhysType_Simulated": "simulated",
}

_MOTION = {
    "ACM_Free": "free",
    "ACM_Limited": "limited",
    "ACM_Locked": "locked",
}

_CAPSULE_KEY = "SphylElems"
_UNSUPPORTED_SHAPE_KEYS = (
    "BoxElems",
    "SphereElems",
    "ConvexElems",
    "TaperedCapsuleElems",
    "LevelSetElems",
)

_DEFAULT_LIMIT_DEGREES = 45.0

_PROPERTY_LINE_RE = re.compile(
    r"^[ \t]*([A-Za-z_][A-Za-z0-9_]*)(?:\(([0-9]+)\))?[ \t]*="
)
_REFERENCE_RE = re.compile(r"'([^']+)'[ \t]*$")


class BonePhysicsExtractError(RuntimeError):
    """Raised when a PhysicsAsset does not match the expected contract."""


# ---------------------------------------------------------------------------
# T3D object-tree parsing
# ---------------------------------------------------------------------------

@dataclass
class ObjectBlock:
    """One ``Begin Object`` block with its direct properties and children."""

    header: str
    own_lines: list[str] = field(default_factory=list)
    children: list["ObjectBlock"] = field(default_factory=list)
    _properties: dict[str, Any] | None = None
    _arrays: dict[str, dict[int, Any]] | None = None

    @property
    def name(self) -> str | None:
        match = re.search(r'\bName="([^"]*)"', self.header)
        return match.group(1) if match else None

    @property
    def class_name(self) -> str | None:
        match = re.search(r"\bClass=(\S+)", self.header)
        if match:
            return match.group(1).rsplit(".", 1)[-1]
        # Instance blocks omit Class= but keep the class in ExportPath.
        match = re.search(
            r"ExportPath=\"/Script/(?:[^./\"]+\.)?([^'\".\s]+)", self.header
        )
        return match.group(1) if match else None

    def properties(self) -> dict[str, Any]:
        if self._properties is None:
            self._properties, self._arrays = _parse_properties(self.own_lines)
        return self._properties

    def arrays(self) -> dict[str, dict[int, Any]]:
        if self._arrays is None:
            self._properties, self._arrays = _parse_properties(self.own_lines)
        return self._arrays


def parse_object_tree(text: str) -> list[ObjectBlock]:
    """Build the nested object tree with balanced Begin/End counting."""

    roots: list[ObjectBlock] = []
    stack: list[ObjectBlock] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("Begin Object") and (
            stripped == "Begin Object" or stripped.startswith("Begin Object ")
        ):
            block = ObjectBlock(header=stripped)
            if stack:
                stack[-1].children.append(block)
            else:
                roots.append(block)
            stack.append(block)
            continue
        if stripped == "End Object":
            if not stack:
                raise BonePhysicsExtractError(
                    f"unbalanced End Object at line {line_number}"
                )
            stack.pop()
            continue
        if stack:
            stack[-1].own_lines.append(line)

    if stack:
        raise BonePhysicsExtractError(
            f"unterminated object block starting at {stack[-1].header!r}"
        )
    return roots


def _scan_parens(
    text: str,
    depth: int,
    in_quote: bool,
    escaped: bool,
) -> tuple[int, bool, bool]:
    """Track parentheses outside quoted strings."""

    for char in text:
        if in_quote:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_quote = False
            continue
        if char == '"':
            in_quote = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth < 0:
                raise BonePhysicsExtractError("unbalanced ')' in property value")
    return depth, in_quote, escaped


def _parse_properties(
    lines: Iterable[str],
) -> tuple[dict[str, Any], dict[str, dict[int, Any]]]:
    """Split direct block lines into ``key=value`` properties.

    A property may span several lines as long as its parentheses are open.
    Nested struct fields such as ``Radius=`` are therefore never mistaken for
    top-level properties.
    """

    properties: dict[str, Any] = {}
    arrays: dict[str, dict[int, Any]] = {}
    key: str | None = None
    index: int | None = None
    chunks: list[str] = []
    depth = 0
    in_quote = False
    escaped = False

    def flush() -> None:
        nonlocal key, index, chunks, depth, in_quote, escaped
        if key is None:
            return
        raw = "\n".join(chunks).strip()
        if not raw:
            raise BonePhysicsExtractError(f"property {key!r} has no value")
        try:
            value, consumed = parse_t3d_value(raw, 0)
        except T3DParseError as error:
            raise BonePhysicsExtractError(
                f"cannot parse property {key!r}: {error}"
            ) from error
        if raw[consumed:].strip():
            raise BonePhysicsExtractError(
                f"trailing text after property {key!r}: "
                f"{raw[consumed:].strip()[:40]!r}"
            )
        if index is None:
            if key in properties:
                raise BonePhysicsExtractError(f"duplicate property {key!r}")
            properties[key] = value
        else:
            bucket = arrays.setdefault(key, {})
            if index in bucket:
                raise BonePhysicsExtractError(
                    f"duplicate indexed property {key}({index})"
                )
            bucket[index] = value
        key = None
        index = None
        chunks = []
        depth = 0
        in_quote = False
        escaped = False

    for line in lines:
        if key is None:
            if not line.strip():
                continue
            match = _PROPERTY_LINE_RE.match(line)
            if not match:
                raise BonePhysicsExtractError(
                    f"unexpected object line {line.strip()[:60]!r}"
                )
            key = match.group(1)
            index = int(match.group(2)) if match.group(2) is not None else None
            rest = line[match.end():]
            chunks = [rest]
            depth, in_quote, escaped = _scan_parens(rest, 0, False, False)
        else:
            chunks.append(line)
            depth, in_quote, escaped = _scan_parens(
                line, depth, in_quote, escaped
            )
        if depth == 0 and not in_quote:
            flush()

    if key is not None:
        raise BonePhysicsExtractError(f"unterminated property {key!r}")
    return properties, arrays


def _index_blocks(
    blocks: Iterable[ObjectBlock],
    label: str,
) -> dict[str, ObjectBlock]:
    """Deduplicate stub/full copies of the same object block."""

    indexed: dict[str, ObjectBlock] = {}
    for block in blocks:
        name = block.name
        if not name:
            raise BonePhysicsExtractError(f"{label} block has no Name=")
        existing = indexed.get(name)
        if existing is None:
            indexed[name] = block
            continue
        existing_props = existing.properties()
        incoming_props = block.properties()
        if not existing_props and incoming_props:
            indexed[name] = block
        elif existing_props and not incoming_props:
            continue
        elif existing_props != incoming_props:
            raise BonePhysicsExtractError(
                f"{label} {name!r} has conflicting duplicate copies"
            )
    return indexed


def _reference_order(block: ObjectBlock, key: str) -> list[str]:
    bucket = block.arrays().get(key, {})
    if not bucket:
        return []
    indexes = sorted(bucket)
    if indexes != list(range(len(indexes))):
        raise BonePhysicsExtractError(
            f"{block.name}:{key} indexes are not contiguous: {indexes}"
        )
    names: list[str] = []
    for index in indexes:
        value = bucket[index]
        if not isinstance(value, str):
            raise BonePhysicsExtractError(
                f"{block.name}:{key}({index}) must be an object reference string"
            )
        match = _REFERENCE_RE.search(value)
        if not match:
            raise BonePhysicsExtractError(
                f"{block.name}:{key}({index}) is not a quoted reference: {value!r}"
            )
        names.append(match.group(1))
    return names


def _ordered_blocks(
    block: ObjectBlock,
    key: str,
    indexed: dict[str, ObjectBlock],
    label: str,
) -> list[ObjectBlock]:
    references = _reference_order(block, key)
    if len(set(references)) != len(references):
        raise BonePhysicsExtractError(f"{key} contains duplicate references")
    missing = [name for name in references if name not in indexed]
    if missing:
        raise BonePhysicsExtractError(
            f"{key} references unknown {label} block(s): {', '.join(missing)}"
        )
    unreferenced = sorted(set(indexed) - set(references))
    if unreferenced:
        raise BonePhysicsExtractError(
            f"{label} block(s) are not referenced by {key}: "
            f"{', '.join(unreferenced)}"
        )
    return [indexed[name] for name in references]


# ---------------------------------------------------------------------------
# Value conversion
# ---------------------------------------------------------------------------

def _require_struct(value: Any, context: str) -> T3DStruct:
    if not isinstance(value, T3DStruct):
        raise BonePhysicsExtractError(f"{context} must be a struct")
    return value


def _require_string(value: Any, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise BonePhysicsExtractError(f"{context} must be a non-empty string")
    return value


def _require_number(value: Any, context: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BonePhysicsExtractError(f"{context} must be numeric")
    number = float(value)
    if not math.isfinite(number):
        raise BonePhysicsExtractError(f"{context} must be finite")
    return 0.0 if number == 0.0 else number


def _number_or(value: Any, context: str, default: float) -> float:
    if value is None:
        return default
    return _require_number(value, context)


def _vector(
    value: Any,
    context: str,
    default: tuple[float, float, float] = (0.0, 0.0, 0.0),
) -> list[float]:
    if value is None:
        return [0.0 if item == 0.0 else item for item in default]
    struct = _require_struct(value, context)
    return [
        _require_number(struct.get("X", 0.0), f"{context}.X"),
        _require_number(struct.get("Y", 0.0), f"{context}.Y"),
        _require_number(struct.get("Z", 0.0), f"{context}.Z"),
    ]


def _rotator_quaternion(value: Any, context: str) -> list[float]:
    """UE ``FRotator::Quaternion()`` in raw UE axes."""

    if value is None:
        return [0.0, 0.0, 0.0, 1.0]
    struct = _require_struct(value, context)
    pitch = math.radians(_require_number(struct.get("Pitch", 0.0), f"{context}.Pitch") / 2.0)
    yaw = math.radians(_require_number(struct.get("Yaw", 0.0), f"{context}.Yaw") / 2.0)
    roll = math.radians(_require_number(struct.get("Roll", 0.0), f"{context}.Roll") / 2.0)
    sp, cp = math.sin(pitch), math.cos(pitch)
    sy, cy = math.sin(yaw), math.cos(yaw)
    sr, cr = math.sin(roll), math.cos(roll)
    quat = [
        cr * sp * sy - sr * cp * cy,
        -cr * sp * cy - sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    ]
    return _canonical_quaternion(quat, context)


def _canonical_quaternion(quat: list[float], context: str) -> list[float]:
    length = math.sqrt(sum(component * component for component in quat))
    if length < 1e-9:
        return [0.0, 0.0, 0.0, 1.0]
    normalized = [
        (0.0 if component == 0.0 else component / length) for component in quat
    ]
    if normalized[3] < 0.0:
        normalized = [-component for component in normalized]
    return [0.0 if component == 0.0 else component for component in normalized]


def _dot(a: list[float], b: list[float]) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a: list[float], b: list[float]) -> list[float]:
    return [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]


def _normalize(vector: list[float], context: str) -> list[float]:
    length = math.sqrt(_dot(vector, vector))
    if length < 1e-9:
        raise BonePhysicsExtractError(f"{context} is a zero-length axis")
    return [component / length for component in vector]


def _basis_quaternion(
    primary: list[float],
    secondary: list[float],
    context: str,
) -> list[float]:
    """Build an orthonormal frame (X=primary, Y=secondary, Z=X×Y)."""

    axis_x = _normalize(primary, f"{context}.PriAxis")
    projection = _dot(axis_x, secondary)
    axis_y = [
        secondary[0] - axis_x[0] * projection,
        secondary[1] - axis_x[1] * projection,
        secondary[2] - axis_x[2] * projection,
    ]
    axis_y = _normalize(axis_y, f"{context}.SecAxis")
    axis_z = _cross(axis_x, axis_y)

    # Column-major rotation matrix built from the three axes.
    m00, m01, m02 = axis_x[0], axis_y[0], axis_z[0]
    m10, m11, m12 = axis_x[1], axis_y[1], axis_z[1]
    m20, m21, m22 = axis_x[2], axis_y[2], axis_z[2]

    trace = m00 + m11 + m22
    if trace > 0.0:
        scale = math.sqrt(trace + 1.0) * 2.0
        quat = [
            (m21 - m12) / scale,
            (m02 - m20) / scale,
            (m10 - m01) / scale,
            0.25 * scale,
        ]
    elif m00 > m11 and m00 > m22:
        scale = math.sqrt(1.0 + m00 - m11 - m22) * 2.0
        quat = [
            0.25 * scale,
            (m01 + m10) / scale,
            (m02 + m20) / scale,
            (m21 - m12) / scale,
        ]
    elif m11 > m22:
        scale = math.sqrt(1.0 + m11 - m00 - m22) * 2.0
        quat = [
            (m01 + m10) / scale,
            0.25 * scale,
            (m12 + m21) / scale,
            (m02 - m20) / scale,
        ]
    else:
        scale = math.sqrt(1.0 + m22 - m00 - m11) * 2.0
        quat = [
            (m02 + m20) / scale,
            (m12 + m21) / scale,
            0.25 * scale,
            (m10 - m01) / scale,
        ]
    return _canonical_quaternion(quat, context)


def _frame(instance: T3DStruct, suffix: str, context: str) -> dict[str, Any]:
    position = _vector(instance.get(f"Pos{suffix}"), f"{context}.Pos{suffix}")
    primary = instance.get(f"PriAxis{suffix}")
    secondary = instance.get(f"SecAxis{suffix}")
    if primary is None and secondary is None:
        rotation = [0.0, 0.0, 0.0, 1.0]
    elif primary is None or secondary is None:
        raise BonePhysicsExtractError(
            f"{context} has only one of PriAxis{suffix}/SecAxis{suffix}"
        )
    else:
        rotation = _basis_quaternion(
            _vector(primary, f"{context}.PriAxis{suffix}"),
            _vector(secondary, f"{context}.SecAxis{suffix}"),
            context,
        )
    return {"position": position, "rotation": rotation}


def _element_list(value: Any, context: str) -> list[T3DStruct]:
    """Flatten UE array syntax ``((...),(...))`` and accept a single struct."""

    struct = _require_struct(value, context)
    if not struct.members:
        return []
    if any(key is None for key, _ in struct.members):
        elements: list[T3DStruct] = []
        for key, item in struct.members:
            if key is not None:
                raise BonePhysicsExtractError(
                    f"{context} mixes keyed and array elements"
                )
            elements.append(_require_struct(item, f"{context}[{len(elements)}]"))
        return elements
    return [struct]


def _parse_shapes(body: ObjectBlock) -> list[dict[str, Any]]:
    aggregate = _require_struct(
        body.properties().get("AggGeom"), f"{body.name}.AggGeom"
    )
    unsupported: list[str] = []
    for key in _UNSUPPORTED_SHAPE_KEYS:
        value = aggregate.get(key)
        if value is not None and _element_list(
            value, f"{body.name}.AggGeom.{key}"
        ):
            unsupported.append(key)
    if unsupported:
        raise BonePhysicsExtractError(
            f"{body.name}.AggGeom uses unsupported shape type(s): "
            f"{', '.join(unsupported)}"
        )

    capsule_value = aggregate.get(_CAPSULE_KEY)
    if capsule_value is None:
        raise BonePhysicsExtractError(
            f"{body.name}.AggGeom has no {_CAPSULE_KEY} capsule"
        )

    shapes: list[dict[str, Any]] = []
    for index, element in enumerate(
        _element_list(capsule_value, f"{body.name}.AggGeom.{_CAPSULE_KEY}")
    ):
        context = f"{body.name}.AggGeom.{_CAPSULE_KEY}[{index}]"
        radius = _number_or(element.get("Radius"), f"{context}.Radius", 1.0)
        length = _number_or(element.get("Length"), f"{context}.Length", 1.0)
        if radius <= 0.0:
            raise BonePhysicsExtractError(f"{context}.Radius must be positive")
        if length < 0.0:
            raise BonePhysicsExtractError(f"{context}.Length must be non-negative")
        shapes.append(
            {
                "type": "capsule",
                "radius": radius,
                "halfHeight": length / 2.0,
                "center": _vector(element.get("Center"), f"{context}.Center"),
                "rotation": _rotator_quaternion(
                    element.get("Rotation"), f"{context}.Rotation"
                ),
            }
        )
    if not shapes:
        raise BonePhysicsExtractError(
            f"{body.name}.AggGeom has an empty {_CAPSULE_KEY} array"
        )
    return shapes


def _parse_body(body: ObjectBlock) -> dict[str, Any]:
    properties = body.properties()
    bone = _require_string(properties.get("BoneName"), f"{body.name}.BoneName")
    physics_type = _require_string(
        properties.get("PhysicsType"), f"{body.name}.PhysicsType"
    )
    if physics_type not in _PHYSICS_TYPE:
        raise BonePhysicsExtractError(
            f"{body.name}.PhysicsType is unsupported: {physics_type}"
        )

    mass: float | None = None
    linear_damping = 0.0
    angular_damping = 0.0
    instance_value = properties.get("DefaultInstance")
    if instance_value is not None:
        instance = _require_struct(instance_value, f"{body.name}.DefaultInstance")
        mass_value = instance.get("MassInKgOverride")
        if mass_value is not None:
            mass_override = _require_number(
                mass_value, f"{body.name}.DefaultInstance.MassInKgOverride"
            )
            mass = mass_override if mass_override > 0.0 else None
        linear_damping = _number_or(
            instance.get("LinearDamping"),
            f"{body.name}.DefaultInstance.LinearDamping",
            0.0,
        )
        angular_damping = _number_or(
            instance.get("AngularDamping"),
            f"{body.name}.DefaultInstance.AngularDamping",
            0.0,
        )

    return {
        "bone": bone,
        "type": _PHYSICS_TYPE[physics_type],
        "shapes": _parse_shapes(body),
        "mass": mass,
        "linearDamping": linear_damping,
        "angularDamping": angular_damping,
    }


def _motion(value: Any, context: str) -> str:
    if value is None:
        return "free"
    text = _require_string(value, context)
    if text not in _MOTION:
        raise BonePhysicsExtractError(f"{context} is unsupported: {text}")
    return _MOTION[text]


def _limit(value: Any, context: str) -> dict[str, Any]:
    return {
        "limit": _number_or(value, context, _DEFAULT_LIMIT_DEGREES),
    }


def _parse_constraint(
    constraint: ObjectBlock,
    body_types: dict[str, str],
    warnings: list[str],
    *,
    strict: bool,
) -> dict[str, Any]:
    properties = constraint.properties()
    instance = _require_struct(
        properties.get("DefaultInstance"), f"{constraint.name}.DefaultInstance"
    )
    bone1 = _require_string(
        instance.get("ConstraintBone1"), f"{constraint.name}.ConstraintBone1"
    )
    bone2 = _require_string(
        instance.get("ConstraintBone2"), f"{constraint.name}.ConstraintBone2"
    )
    for bone in (bone1, bone2):
        if bone not in body_types:
            raise BonePhysicsExtractError(
                f"{constraint.name} references body-less bone {bone!r}"
            )

    type1, type2 = body_types[bone1], body_types[bone2]
    if type1 == "simulated" and type2 == "kinematic":
        child, parent = bone1, bone2
        child_frame_suffix, parent_frame_suffix = "1", "2"
    elif type1 == "kinematic" and type2 == "simulated":
        child, parent = bone2, bone1
        child_frame_suffix, parent_frame_suffix = "2", "1"
    elif type1 == "kinematic" and type2 == "kinematic":
        message = (
            f"{constraint.name} joins two kinematic bodies "
            f"({bone1}, {bone2}); UE cannot simulate this joint"
        )
        if strict:
            raise BonePhysicsExtractError(message)
        warnings.append(message)
        child, parent = bone1, bone2
        child_frame_suffix, parent_frame_suffix = "1", "2"
    else:
        raise BonePhysicsExtractError(
            f"{constraint.name} must connect one simulated child to one "
            f"kinematic parent; got {bone1}={type1}, {bone2}={type2}"
        )

    profile = instance.get("ProfileInstance")
    cone = None
    twist = None
    stiffness = 0.0
    damping = 0.0
    if profile is not None:
        profile_struct = _require_struct(profile, f"{constraint.name}.ProfileInstance")
        cone_value = profile_struct.get("ConeLimit")
        if cone_value is not None:
            cone = _require_struct(cone_value, f"{constraint.name}.ConeLimit")
        twist_value = profile_struct.get("TwistLimit")
        if twist_value is not None:
            twist = _require_struct(twist_value, f"{constraint.name}.TwistLimit")

    if cone is None:
        swing1 = {"limit": _DEFAULT_LIMIT_DEGREES, "motion": "free"}
        swing2 = {"limit": _DEFAULT_LIMIT_DEGREES, "motion": "free"}
    else:
        swing1 = _limit(
            cone.get("Swing1LimitDegrees"), f"{constraint.name}.Swing1LimitDegrees"
        )
        swing1["motion"] = _motion(
            cone.get("Swing1Motion"), f"{constraint.name}.Swing1Motion"
        )
        swing2 = _limit(
            cone.get("Swing2LimitDegrees"), f"{constraint.name}.Swing2LimitDegrees"
        )
        swing2["motion"] = _motion(
            cone.get("Swing2Motion"), f"{constraint.name}.Swing2Motion"
        )
        stiffness = _number_or(
            cone.get("Stiffness"), f"{constraint.name}.Stiffness", 0.0
        )
        damping = _number_or(
            cone.get("Damping"), f"{constraint.name}.Damping", 0.0
        )

    if twist is None:
        twist_limit = {"limit": _DEFAULT_LIMIT_DEGREES, "motion": "free"}
    else:
        twist_limit = _limit(
            twist.get("TwistLimitDegrees"), f"{constraint.name}.TwistLimitDegrees"
        )
        twist_limit["motion"] = _motion(
            twist.get("TwistMotion"), f"{constraint.name}.TwistMotion"
        )

    name = instance.get("JointName")
    if not isinstance(name, str) or not name:
        name = child

    return {
        "name": name,
        "child": child,
        "parent": parent,
        "swing1": swing1,
        "swing2": swing2,
        "twist": twist_limit,
        "stiffness": stiffness,
        "damping": damping,
        "childFrame": _frame(instance, child_frame_suffix, f"{constraint.name}.childFrame"),
        "parentFrame": _frame(instance, parent_frame_suffix, f"{constraint.name}.parentFrame"),
    }


# ---------------------------------------------------------------------------
# Asset assembly
# ---------------------------------------------------------------------------

def _asset_block(roots: Iterable[ObjectBlock], asset: str) -> ObjectBlock:
    blocks = [block for block in roots if block.class_name == "PhysicsAsset"]
    if len(blocks) != 1:
        raise BonePhysicsExtractError(
            f"{asset}.t3d must contain exactly one PhysicsAsset root; "
            f"found {len(blocks)}"
        )
    block = blocks[0]
    if block.name != asset:
        raise BonePhysicsExtractError(
            f"{asset}.t3d contains PhysicsAsset {block.name!r}"
        )
    return block


def parse_physics_asset(
    text: str,
    asset: str,
    *,
    strict_constraints: bool = False,
) -> dict[str, Any]:
    roots = parse_object_tree(text)
    root = _asset_block(roots, asset)

    bodies = _index_blocks(
        (child for child in root.children if child.class_name == "SkeletalBodySetup"),
        "SkeletalBodySetup",
    )
    constraints = _index_blocks(
        (
            child
            for child in root.children
            if child.class_name == "PhysicsConstraintTemplate"
        ),
        "PhysicsConstraintTemplate",
    )

    ordered_bodies = _ordered_blocks(
        root, "SkeletalBodySetups", bodies, "body"
    )
    ordered_constraints = _ordered_blocks(
        root, "ConstraintSetup", constraints, "constraint"
    )

    parsed_bodies = [_parse_body(body) for body in ordered_bodies]
    simulated = [body for body in parsed_bodies if body["type"] == "simulated"]
    if not simulated:
        raise BonePhysicsExtractError(f"{asset} has no simulated body")

    body_types = {body["bone"]: body["type"] for body in parsed_bodies}
    if len(body_types) != len(parsed_bodies):
        raise BonePhysicsExtractError(f"{asset} assigns one bone to two bodies")

    warnings: list[str] = []
    parsed_constraints = [
        _parse_constraint(
            constraint,
            body_types,
            warnings,
            strict=strict_constraints,
        )
        for constraint in ordered_constraints
    ]
    return {
        "bodies": parsed_bodies,
        "constraints": parsed_constraints,
        "warnings": warnings,
    }


def _set_name(asset: str) -> str:
    for candidate, name, _, _ in ASSET_SPECS:
        if candidate == asset:
            return name
    raise BonePhysicsExtractError(f"unexpected physics asset {asset!r}")


def extract_bone_physics(
    directory: Path,
    output_path: Path | None = None,
    *,
    asset_id: str = DEFAULT_ASSET_ID,
    validate: bool = True,
    strict_constraints: bool = False,
) -> tuple[dict[str, Any], bool]:
    """Read every expected ``PA_*.t3d`` and optionally write the asset."""

    directory = directory.resolve()
    expected = {asset for asset, _, _, _ in ASSET_SPECS}
    found = {path.stem for path in directory.glob("PA_*.t3d")}
    missing = sorted(expected - found)
    if missing:
        raise BonePhysicsExtractError(
            f"missing physics T3D export(s) in {directory}: {', '.join(missing)}"
        )
    extra = sorted(found - expected)
    if extra:
        raise BonePhysicsExtractError(
            f"unexpected physics T3D export(s) in {directory}: {', '.join(extra)}"
        )

    sets: list[dict[str, Any]] = []
    sources: list[dict[str, str]] = []
    for asset, name, expected_bodies, expected_constraints in ASSET_SPECS:
        path = directory / f"{asset}.t3d"
        text = path.read_text(encoding="utf-8-sig")
        parsed = parse_physics_asset(
            text, asset, strict_constraints=strict_constraints
        )
        body_count = len(parsed["bodies"])
        constraint_count = len(parsed["constraints"])
        if validate and (body_count, constraint_count) != (
            expected_bodies,
            expected_constraints,
        ):
            raise BonePhysicsExtractError(
                f"{asset} has {body_count} bodies / {constraint_count} "
                f"constraints; expected {expected_bodies} / "
                f"{expected_constraints}"
            )
        for warning in parsed["warnings"]:
            print(f"warning: {asset}: {warning}", file=sys.stderr)
        sets.append(
            {
                "name": name,
                "asset": asset,
                "baseBone": BASE_BONE,
                "gravity": list(GRAVITY),
                "bodies": parsed["bodies"],
                "constraints": parsed["constraints"],
            }
        )
        sources.append({"asset": asset, "blake3": blake3_file(path)})

    if validate:
        for entry in sets:
            simulated = [
                body for body in entry["bodies"] if body["type"] == "simulated"
            ]
            if not simulated:
                raise BonePhysicsExtractError(
                    f"{entry['asset']} has no simulated body"
                )
            for constraint in entry["constraints"]:
                child = next(
                    body
                    for body in entry["bodies"]
                    if body["bone"] == constraint["child"]
                )
                parent = next(
                    body
                    for body in entry["bodies"]
                    if body["bone"] == constraint["parent"]
                )
                if child["type"] != "simulated" or parent["type"] != "kinematic":
                    if child["type"] == "kinematic" and parent["type"] == "kinematic":
                        continue
                    raise BonePhysicsExtractError(
                        f"{entry['asset']}:{constraint['name']} is not a "
                        "simulated-child/kinematic-parent joint"
                    )

    document = {
        "format": FORMAT,
        "id": asset_id,
        "sources": sources,
        "sets": sets,
    }
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
        repo_root / "artifacts" / "ue-echo-export" / "physics-t3d",
        sibling_resources / "Maps" / "SC_valley_echo" / "Rig"
        / "BPH_EchoPostProcess.json",
    )


def print_summary(
    document: dict[str, Any],
    output_path: Path | None,
    changed: bool,
) -> None:
    print(
        f"{'set':<14}{'bodies':>7}{'sim':>5}{'kin':>5}"
        f"{'joints':>8}{'shapes':>8}"
    )
    print("-" * 52)
    for entry in document["sets"]:
        simulated = sum(1 for body in entry["bodies"] if body["type"] == "simulated")
        shapes = sum(len(body["shapes"]) for body in entry["bodies"])
        print(
            f"{entry['name']:<14}{len(entry['bodies']):>7}{simulated:>5}"
            f"{len(entry['bodies']) - simulated:>5}"
            f"{len(entry['constraints']):>8}{shapes:>8}"
        )
    print("-" * 52)
    print(
        f"sets={len(document['sets'])} "
        f"bodies={sum(len(entry['bodies']) for entry in document['sets'])} "
        f"constraints="
        f"{sum(len(entry['constraints']) for entry in document['sets'])}"
    )
    if output_path is not None:
        state = "written" if changed else "unchanged"
        print(f"{state}: {output_path}")


def main(argv: list[str] | None = None) -> int:
    default_directory, default_output = _default_paths()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dir", type=Path, default=default_directory)
    parser.add_argument("--output", type=Path, default=default_output)
    parser.add_argument("--id", default=DEFAULT_ASSET_ID)
    parser.add_argument(
        "--no-validate",
        action="store_true",
        help="skip the Echo body/constraint counts (comparison only)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="reject constraints that do not join simulated child to kinematic parent",
    )
    args = parser.parse_args(argv)

    try:
        document, changed = extract_bone_physics(
            args.dir,
            args.output,
            asset_id=args.id,
            validate=not args.no_validate,
            strict_constraints=args.strict,
        )
    except (OSError, BonePhysicsExtractError, T3DParseError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    print_summary(document, args.output.resolve(), changed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
