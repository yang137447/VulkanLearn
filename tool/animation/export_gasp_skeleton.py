#!/usr/bin/env python3
"""Export Maps/<map>/Skeletons/SK_UEFN_Mannequin.json from the GASP FBX.

Authority model (plan §4.2): the FBX is the source of the bind hierarchy and
local rest transforms; the UE reference skeleton is an independent cross-check.
The script therefore:

1. reads the FBX tree exported by ``vl_animation_probe --fbx`` (the same Assimp
   settings and the same ``FbxToVlBasis`` conversion as
   ``AssimpSkeletalImporter::ReadMesh``);
2. builds the physical joints in depth-first order from ``root``, skipping
   ``VB `` virtual bones, and writes parent-space row-major centimetre
   ``restLocal`` matrices;
3. compares names, parents, edge lengths and root distances against the UE
   skeleton -- any mismatch aborts without writing;
4. registers the union of all float curve names used by the 22 GASP clips.

Re-running is refused unless ``--force`` is passed.  Output is UTF-8, two-space
indent, LF, stable field order.

Typical command (from the checkout root):

    py -3 tool/animation/export_gasp_skeleton.py
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path

from gasp_anim_common import (
    blake3_file,
    blake3_hex,
    load_json,
    preflight_targets,
    resource_relative,
    write_json,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RESOURCE_ROOT = REPO_ROOT.parent / "VukanLearnResources"
FBX_RELATIVE = "Maps/SC_gasp/Source/SKM_UEFN_Mannequin.fbx"
OUTPUT_RELATIVE = "Maps/SC_gasp/Skeletons/SK_UEFN_Mannequin.json"
DEFAULT_RAW = REPO_ROOT / "artifacts/anim-sequence-export/ue-anim-raw.json"
DEFAULT_FBX_INSPECT = REPO_ROOT / "artifacts/anim-sequence-export/fbx-skeleton.json"
DEFAULT_PROBE = REPO_ROOT / "build/bin/vl_animation_probe.exe"
SKELETON_ID = "SK_UEFN_Mannequin"
ROOT_JOINT = "root"
BASIS = (
    (1.0, 0.0, 0.0, 0.0),
    (0.0, 0.0, 1.0, 0.0),
    (0.0, -1.0, 0.0, 0.0),
    (0.0, 0.0, 0.0, 1.0),
)
# Same rejection thresholds as AssimpSkeletalImporter::Transform.
ORTHONORMAL_TOLERANCE = 1e-4
AFFINE_ROW_TOLERANCE = 1e-5
# Cross-check tolerances against the UE reference skeleton (centimetres).
EDGE_LENGTH_TOLERANCE = 1e-3
ROOT_DISTANCE_TOLERANCE = 1e-3


class ExportError(RuntimeError):
    pass


def identity4() -> list[list[float]]:
    return [[1.0 if row == column else 0.0 for column in range(4)] for row in range(4)]


def multiply(left: list[list[float]], right: list[list[float]]) -> list[list[float]]:
    return [[sum(left[row][k] * right[k][column] for k in range(4))
             for column in range(4)] for row in range(4)]


def transpose(matrix: list[list[float]]) -> list[list[float]]:
    return [[matrix[column][row] for column in range(4)] for row in range(4)]


def convert_to_vl(matrix: list[list[float]]) -> list[list[float]]:
    basis = [list(row) for row in BASIS]
    return multiply(multiply(basis, matrix), transpose(basis))


def node_matrix(node: dict) -> list[list[float]]:
    local = node.get("local")
    if not isinstance(local, list) or len(local) != 4:
        raise ExportError("FBX node %r has no 4x4 local matrix" % node.get("name"))
    result = []
    for row in local:
        if len(row) != 4:
            raise ExportError("FBX node %r has a malformed local matrix" % node.get("name"))
        result.append([float(value) for value in row])
    return result


def model_matrix(node: dict, nodes: list[dict]) -> list[list[float]]:
    """Scene-root-to-node product, matching AssimpSkeletalImporter::Model."""
    chain = []
    current = node
    while current is not None:
        chain.append(current)
        parent_index = current.get("parentIndex", -1)
        current = nodes[parent_index] if parent_index >= 0 else None
    result = identity4()
    for entry in reversed(chain):
        result = multiply(result, node_matrix(entry))
    return result


def validate_rest_matrix(name: str, matrix: list[list[float]]) -> None:
    for row in matrix:
        for value in row:
            if not math.isfinite(value):
                raise ExportError("Non-finite rest transform for joint %r" % name)
    for column in range(4):
        if abs(matrix[3][column] - (1.0 if column == 3 else 0.0)) > AFFINE_ROW_TOLERANCE:
            raise ExportError("Non-affine rest transform (last row) for joint %r" % name)
    linear = [[matrix[row][column] for column in range(3)] for row in range(3)]
    columns = [[linear[row][column] for row in range(3)] for column in range(3)]
    for left in range(3):
        for right in range(3):
            dot = sum(columns[left][axis] * columns[right][axis] for axis in range(3))
            expected = 1.0 if left == right else 0.0
            if abs(dot - expected) > ORTHONORMAL_TOLERANCE:
                raise ExportError("Sheared rest transform for joint %r" % name)
    determinant = (
        linear[0][0] * (linear[1][1] * linear[2][2] - linear[1][2] * linear[2][1])
        - linear[0][1] * (linear[1][0] * linear[2][2] - linear[1][2] * linear[2][0])
        + linear[0][2] * (linear[1][0] * linear[2][1] - linear[1][1] * linear[2][0])
    )
    if determinant <= 0:
        raise ExportError("Reflected rest transform for joint %r" % name)


def translation(matrix: list[list[float]]) -> list[float]:
    return [matrix[0][3], matrix[1][3], matrix[2][3]]


def distance(left: list[float], right: list[float]) -> float:
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(left, right)))


def significant(value: float, digits: int = 9) -> float:
    """Round to *digits* significant decimal digits; keeps JSON reviewable.

    The plan asks for 9 significant digits.  For centimetre rest transforms
    this is far below the importer's 1e-5 element tolerance, and it removes the
    float64 tails that Assimp's float32 values would otherwise print.
    """
    if value == 0.0:
        return 0.0
    exponent = math.floor(math.log10(abs(value)))
    rounded = round(value, digits - 1 - exponent)
    return 0.0 if rounded == 0.0 else rounded


def build_fbx_hierarchy(inspection: dict) -> tuple[list[dict], list[list[float]]]:
    metadata = inspection.get("fbxMetadata", {})
    expected = {
        "UnitScaleFactor": 1,
        "UpAxis": 2,
        "UpAxisSign": 1,
        "FrontAxis": 1,
        "FrontAxisSign": -1,
        "CoordAxis": 0,
        "CoordAxisSign": 1,
    }
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise ExportError(
                "Unexpected FBX metadata %s=%r (expected %r); SK rest units/axes are not verified"
                % (key, metadata.get(key), value))
    nodes = inspection.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        raise ExportError("FBX inspection has no nodes")
    scene_roots = [node for node in nodes if node.get("parentIndex", -1) < 0]
    if len(scene_roots) != 1:
        raise ExportError("FBX inspection must have exactly one scene root")
    root_matches = [node for node in nodes if node.get("name") == ROOT_JOINT]
    if len(root_matches) != 1:
        raise ExportError("FBX must contain exactly one %r joint" % ROOT_JOINT)
    root = root_matches[0]
    children: dict[int, list[dict]] = {}
    for node in nodes:
        children.setdefault(int(node.get("parentIndex", -1)), []).append(node)
    for group in children.values():
        group.sort(key=lambda node: int(node.get("index", 0)))
    joints: list[dict] = []

    def visit(node: dict) -> None:
        name = str(node.get("name", ""))
        if not name:
            raise ExportError("FBX joint with empty name")
        if name.startswith("VB "):
            raise ExportError("Virtual bone %r unexpectedly appears in the FBX mesh hierarchy" % name)
        joints.append(node)
        for child in children.get(int(node.get("index", -1)), []):
            visit(child)

    visit(root)
    matrices = []
    for joint in joints:
        name = str(joint["name"])
        source = model_matrix(joint, nodes) if joint is root else node_matrix(joint)
        matrix = convert_to_vl(source)
        validate_rest_matrix(name, matrix)
        matrices.append(matrix)
    return joints, matrices


def build_ue_hierarchy(skeleton: dict) -> tuple[list[dict], list[dict]]:
    bones = skeleton.get("bones")
    if not isinstance(bones, list) or not bones:
        raise ExportError("UE skeleton export has no bones")
    physical = [bone for bone in bones if not str(bone["name"]).startswith("VB ")]
    virtual = [bone for bone in bones if str(bone["name"]).startswith("VB ")]
    return physical, virtual


def cross_check(fbx_joints: list[dict], fbx_matrices: list[list[list[float]]],
                ue_physical: list[dict], ue_virtual: list[dict]) -> dict:
    fbx_names = [str(joint["name"]) for joint in fbx_joints]
    ue_names = [str(bone["name"]) for bone in ue_physical]
    if fbx_names != ue_names:
        missing = sorted(set(fbx_names) - set(ue_names))
        extra = sorted(set(ue_names) - set(fbx_names))
        if missing or extra:
            raise ExportError(
                "FBX/UE joint set mismatch: missing in UE=%s, extra in UE=%s"
                % (missing[:8], extra[:8]))
        raise ExportError("FBX/UE joint order mismatch (both contain the same names)")
    if len(set(fbx_names)) != len(fbx_names):
        raise ExportError("Duplicate joint names in FBX")

    fbx_index = {name: index for index, name in enumerate(fbx_names)}
    ue_index = {str(bone["name"]): index for index, bone in enumerate(ue_physical)}
    for index, joint in enumerate(fbx_joints):
        parent = str(joint.get("parent", ""))
        expected = fbx_index.get(parent, -1)
        ue_parent = str(ue_physical[index].get("parent", "None"))
        actual = -1 if ue_parent == "None" else ue_index.get(ue_parent, -2)
        if expected != actual:
            raise ExportError(
                "Parent mismatch for joint %r: FBX=%r(%d), UE=%r(%d)"
                % (joint["name"], parent, expected, ue_parent, actual))

    # Positions from the FBX rest hierarchy vs UE reference model positions.
    fbx_models: list[list[list[float]]] = []
    for index, joint in enumerate(fbx_joints):
        parent = fbx_index.get(str(joint.get("parent", "")), -1)
        fbx_models.append(
            fbx_matrices[index] if parent < 0 else multiply(fbx_models[parent], fbx_matrices[index]))
    fbx_positions = [translation(matrix) for matrix in fbx_models]
    ue_positions = [bone["referenceModel"]["translation"] for bone in ue_physical]

    edge_errors = []
    max_edge_error = 0.0
    for index, joint in enumerate(fbx_joints):
        parent = fbx_index.get(str(joint.get("parent", "")), -1)
        if parent < 0:
            continue
        fbx_length = distance(fbx_positions[index], fbx_positions[parent])
        ue_length = distance(ue_positions[index], ue_positions[parent])
        error = abs(fbx_length - ue_length)
        max_edge_error = max(max_edge_error, error)
        if error > EDGE_LENGTH_TOLERANCE:
            edge_errors.append(
                (joint["name"], fbx_length, ue_length, fbx_length - ue_length))
    if edge_errors:
        raise ExportError(
            "FBX/UE rest edge-length mismatch (cm): %s" % (edge_errors[:8],))

    root_distance_errors = []
    for index, joint in enumerate(fbx_joints):
        fbx_distance = distance(fbx_positions[index], fbx_positions[0])
        ue_distance = distance(ue_positions[index], ue_positions[0])
        if abs(fbx_distance - ue_distance) > ROOT_DISTANCE_TOLERANCE:
            root_distance_errors.append(
                (joint["name"], fbx_distance, ue_distance, fbx_distance - ue_distance))
    if root_distance_errors:
        raise ExportError(
            "FBX/UE rest root-distance mismatch (cm): %s" % (root_distance_errors[:8],))

    for bone in ue_virtual:
        parent = str(bone.get("parent", "None"))
        if parent != "None" and parent not in fbx_index:
            raise ExportError("Virtual bone %r has unknown parent %r" % (bone["name"], parent))

    return {
        "jointCount": len(fbx_joints),
        "virtualBoneCount": len(ue_virtual),
        "maxEdgeLengthErrorCm": max_edge_error,
    }


def collect_curves(raw: dict) -> tuple[dict[str, str], dict[str, list[str]]]:
    """Union of float curve names, case-insensitive, first spelling preserved.

    GASP itself is inconsistent: ``Phase`` appears on Walk_Loop while ``phase``
    appears on Walk_Start/Run/Sprint.  UE curve lookup is case-insensitive, so
    the registry keeps the first spelling and records the variant for the report
    instead of rejecting the whole skeleton.
    """
    registry: dict[str, str] = {}
    spellings: dict[str, str] = {}
    variants: dict[str, list[str]] = {}
    for clip in raw.get("clips", []):
        for curve in clip.get("curves", []):
            name = str(curve.get("name", ""))
            if not name or len(name) > 64:
                raise ExportError("Invalid curve name %r in clip %r" % (name, clip.get("name")))
            key = name.casefold()
            if key in spellings:
                if spellings[key] != name:
                    variants.setdefault(spellings[key], [])
                    if name not in variants[spellings[key]]:
                        variants[spellings[key]].append(name)
                continue
            spellings[key] = name
            registry[name] = "Float"
    ordered = {name: registry[name]
               for name in sorted(registry, key=lambda value: (value.casefold(), value))}
    return ordered, variants


def probe_fbx(fbx: Path, output: Path, probe: Path) -> None:
    if not probe.is_file():
        raise ExportError(
            "Missing FBX inspection %s and probe %s; rerun with --fbx-inspect" % (output, probe))
    if output.exists():
        raise ExportError("FBX inspection output already exists: " + str(output))
    output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [str(probe), "--fbx", str(fbx), "--output", str(output)],
        capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        raise ExportError(
            "FBX inspection failed (%d): %s%s" % (result.returncode, result.stdout, result.stderr))


def self_test() -> None:
    vectors = {
        b"": "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262",
        b"abc": "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85",
    }
    for data, expected in vectors.items():
        actual = blake3_hex(data)
        if actual != expected:
            raise ExportError("BLAKE3 self-test failed for %r: %s" % (data, actual))


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path,
                        default=Path(os.environ.get("VL_GASP_RESOURCE_ROOT", DEFAULT_RESOURCE_ROOT)),
                        help="VukanLearnResources checkout")
    parser.add_argument("--ue-raw", type=Path, default=DEFAULT_RAW,
                        help="UE raw animation dump from export_gasp_ue_raw.py")
    parser.add_argument("--fbx", type=Path, default=None,
                        help="skeleton FBX (default: <resource-root>/" + FBX_RELATIVE + ")")
    parser.add_argument("--fbx-inspect", type=Path, default=DEFAULT_FBX_INSPECT,
                        help="Assimp FBX inspection JSON")
    parser.add_argument("--probe", type=Path, default=DEFAULT_PROBE,
                        help="vl_animation_probe.exe used to refresh --fbx-inspect")
    parser.add_argument("--output", type=Path, default=None,
                        help="target SK JSON (default: <resource-root>/" + OUTPUT_RELATIVE + ")")
    parser.add_argument("--force", action="store_true", help="replace an existing SK JSON")
    parser.add_argument("--self-test", action="store_true", help="check the bundled BLAKE3 vectors")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    arguments = parse_arguments(argv)
    try:
        if arguments.self_test:
            self_test()
            print("BLAKE3 self-test OK")
            return 0
        resource_root = arguments.resource_root.resolve()
        fbx = (arguments.fbx or (resource_root / FBX_RELATIVE)).resolve()
        output = (arguments.output or (resource_root / OUTPUT_RELATIVE)).resolve()
        if not fbx.is_file():
            raise ExportError("Missing skeleton FBX: " + str(fbx))
        raw = load_json(arguments.ue_raw.resolve())
        if raw.get("format") != "vl.gasp.ue-animation-raw.v1" or raw.get("status") != "complete":
            raise ExportError("UE raw animation dump is incomplete: " + str(arguments.ue_raw))
        if not arguments.fbx_inspect.exists():
            probe_fbx(fbx, arguments.fbx_inspect.resolve(), arguments.probe.resolve())
        else:
            inspection_path = arguments.fbx_inspect.resolve()
            if inspection_path.stat().st_mtime + 1 < fbx.stat().st_mtime:
                raise ExportError(
                    "FBX inspection %s is older than %s; generate a fresh inspection"
                    % (inspection_path, fbx))
        inspection = load_json(arguments.fbx_inspect.resolve())
        if inspection.get("format") != "vl.animation.fbx-inspection.v1":
            raise ExportError("Unexpected FBX inspection format: " + str(arguments.fbx_inspect))
        if Path(inspection.get("source", "")).resolve() != fbx:
            raise ExportError(
                "FBX inspection was generated from %r, not %r"
                % (inspection.get("source"), str(fbx)))

        fbx_joints, fbx_matrices = build_fbx_hierarchy(inspection)
        ue_physical, ue_virtual = build_ue_hierarchy(raw["skeleton"])
        cross = cross_check(fbx_joints, fbx_matrices, ue_physical, ue_virtual)
        curves, curve_case_variants = collect_curves(raw)
        slots = raw["skeleton"].get("slots") or ["DefaultSlot"]
        if not slots:
            slots = ["DefaultSlot"]
        if len(set(slots)) != len(slots):
            raise ExportError("Duplicate skeleton slot names: %s" % slots)
        if "DefaultSlot" not in slots:
            raise ExportError("Skeleton slots must contain DefaultSlot: %s" % slots)

        joints_document = []
        parent_index_by_name = {str(joint["name"]): index
                                for index, joint in enumerate(fbx_joints)}
        for index, joint in enumerate(fbx_joints):
            raw_flat = [value for row in fbx_matrices[index] for value in row]
            flat = [significant(value) for value in raw_flat]
            if any(abs(rounded - original) > 1e-5
                   for rounded, original in zip(flat, raw_flat)):
                raise ExportError("restLocal rounding exceeded 1e-5 for joint %r" % joint["name"])
            joints_document.append({
                "name": str(joint["name"]),
                "parent": parent_index_by_name.get(str(joint.get("parent", "")), -1),
                "restLocal": flat,
            })
        virtual_document = [
            {"name": str(bone["name"]), "parent": str(bone.get("parent", "None"))}
            for bone in ue_virtual
        ]
        document = {
            "format": "vl.skeleton.v1",
            "skeleton": SKELETON_ID,
            "root": ROOT_JOINT,
            "joints": joints_document,
            "slots": slots,
            "curves": curves,
            "virtualBones": virtual_document,
            "additiveReferencePose": None,
            "source": {
                "fbx": resource_relative(fbx, resource_root),
                "fbxBlake3": blake3_file(fbx),
                "tool": "export_gasp_skeleton.py",
            },
        }
        if len(document["joints"]) > 1024:
            raise ExportError("Skeleton exceeds 1024 joints")
        preflight_targets([output], arguments.force)
        size = write_json(output, document, arguments.force)
        print(json.dumps({
            "format": document["format"],
            "skeleton": document["skeleton"],
            "joints": len(document["joints"]),
            "slots": document["slots"],
            "curves": len(document["curves"]),
            "virtualBones": len(document["virtualBones"]),
            "curveCaseVariants": curve_case_variants,
            "bytes": size,
            "output": str(output),
            "crossCheck": cross,
        }, ensure_ascii=False, indent=2))
        return 0
    except (ExportError, FileExistsError, OSError, ValueError, KeyError) as error:
        print("export_gasp_skeleton.py: error: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
