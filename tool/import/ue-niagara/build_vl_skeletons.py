"""Build vl.skeleton.v1 JSON from the Assimp FBX inspection.

The UE FBX exporter wraps the skeleton in one or two scene nodes.  The VL
importer expects the skeleton root itself to be the first joint and includes
the wrapper transform in the root restLocal matrix.
"""

from __future__ import annotations

import json
import math
from pathlib import Path


RESOURCE_ROOT = Path(
    r"D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/niagara-particle-system/"
    r"VukanLearnResources"
)
INSPECT_ROOT = RESOURCE_ROOT / "Generated/Import/ue_niagara_assets/inspect"
FBX_ROOT = RESOURCE_ROOT / "Common/Source/UE/NiagaraExamples"
OUTPUT_ROOT = RESOURCE_ROOT / "Common/Skeletons"

BASIS = (
    (1.0, 0.0, 0.0, 0.0),
    (0.0, 0.0, 1.0, 0.0),
    (0.0, -1.0, 0.0, 0.0),
    (0.0, 0.0, 0.0, 1.0),
)

SOURCES = (
    ("DinoDragon", "SK_DinoDragon", "SKM_DinoDragon"),
    ("Mannequins", "SKM_Manny_Simple", "SKM_Manny_Simple"),
    ("Mannequins", "SKM_Quinn_Simple", "SKM_Quinn_Simple"),
)


def multiply(left, right):
    return [[sum(left[row][k] * right[k][column] for k in range(4))
             for column in range(4)] for row in range(4)]


def transpose(matrix):
    return [[matrix[column][row] for column in range(4)] for row in range(4)]


def convert_to_vl(matrix):
    basis = [list(row) for row in BASIS]
    return multiply(multiply(basis, matrix), transpose(basis))


def validate(name, matrix):
    for row in matrix:
        for value in row:
            if not math.isfinite(value):
                raise RuntimeError("non-finite rest transform for " + name)
    for column in range(4):
        expected = 1.0 if column == 3 else 0.0
        if abs(matrix[3][column] - expected) > 1e-5:
            raise RuntimeError("non-affine rest transform for " + name)
    linear = [[matrix[row][column] for column in range(3)] for row in range(3)]
    columns = [[linear[row][column] for row in range(3)] for column in range(3)]
    for left in range(3):
        for right in range(3):
            dot = sum(columns[left][axis] * columns[right][axis] for axis in range(3))
            expected = 1.0 if left == right else 0.0
            if abs(dot - expected) > 1e-4:
                raise RuntimeError("sheared rest transform for " + name)
    determinant = (
        linear[0][0] * (linear[1][1] * linear[2][2] - linear[1][2] * linear[2][1])
        - linear[0][1] * (linear[1][0] * linear[2][2] - linear[1][2] * linear[2][0])
        + linear[0][2] * (linear[1][0] * linear[2][1] - linear[1][1] * linear[2][0])
    )
    if determinant <= 0:
        raise RuntimeError("reflected rest transform for " + name)


def significant(value, digits=9):
    if value == 0.0:
        return 0.0
    exponent = math.floor(math.log10(abs(value)))
    rounded = round(value, digits - 1 - exponent)
    return 0.0 if rounded == 0.0 else rounded


def matrix_from_node(node, use_model):
    key = "model" if use_model else "local"
    value = node.get(key)
    if not isinstance(value, list) or len(value) != 4:
        raise RuntimeError("node %r has no %s matrix" % (node.get("name"), key))
    return [[float(value[row][column]) for column in range(4)] for row in range(4)]


def build(inspection_path: Path, fbx_path: Path, skeleton_id: str, output: Path):
    inspection = json.loads(inspection_path.read_text(encoding="utf-8"))
    nodes = inspection.get("nodes")
    if not isinstance(nodes, list) or not nodes:
        raise RuntimeError("inspection has no nodes: " + str(inspection_path))
    roots = [node for node in nodes if node.get("name") == "root"]
    if len(roots) != 1:
        raise RuntimeError("expected exactly one 'root' node, got %d" % len(roots))
    root = roots[0]
    children = {}
    for node in nodes:
        children.setdefault(int(node.get("parentIndex", -1)), []).append(node)
    for group in children.values():
        group.sort(key=lambda entry: int(entry.get("index", 0)))

    joints = []

    def visit(node):
        name = str(node.get("name", ""))
        if not name:
            raise RuntimeError("empty joint name")
        if name.startswith("VB "):
            return
        joints.append(node)
        for child in children.get(int(node.get("index", -1)), []):
            visit(child)

    visit(root)
    if len(joints) < 2:
        raise RuntimeError("skeleton has too few joints")

    by_name = {str(node["name"]): index for index, node in enumerate(joints)}
    if len(by_name) != len(joints):
        raise RuntimeError("duplicate joint names")

    joints_document = []
    for index, node in enumerate(joints):
        name = str(node["name"])
        source = matrix_from_node(node, index == 0)
        matrix = convert_to_vl(source)
        validate(name, matrix)
        flat = [significant(matrix[row][column]) for row in range(4) for column in range(4)]
        parent_name = str(node.get("parent", ""))
        parent = by_name.get(parent_name, -1)
        joints_document.append({"name": name, "parent": parent, "restLocal": flat})

    document = {
        "format": "vl.skeleton.v1",
        "skeleton": skeleton_id,
        "root": "root",
        "joints": joints_document,
        "slots": ["DefaultSlot"],
        "curves": {},
        "virtualBones": [],
        "additiveReferencePose": None,
        "source": {
            "fbx": str(fbx_path.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
            "tool": "build_vl_skeletons.py",
        },
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(document, indent=2, ensure_ascii=False) + "\n",
                      encoding="utf-8")
    return {
        "skeleton": skeleton_id,
        "joints": len(joints_document),
        "output": str(output.relative_to(RESOURCE_ROOT)).replace("\\", "/"),
    }


def main():
    results = []
    for folder, stem, skeleton_id in SOURCES:
        inspection = INSPECT_ROOT / (stem + ".json")
        fbx_path = FBX_ROOT / "SkeletalMesh" / folder / (stem + ".fbx")
        output = OUTPUT_ROOT / (skeleton_id + ".json")
        if not inspection.is_file():
            raise RuntimeError("missing inspection: " + str(inspection))
        if not fbx_path.is_file():
            raise RuntimeError("missing FBX: " + str(fbx_path))
        results.append(build(inspection, fbx_path, skeleton_id, output))
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
