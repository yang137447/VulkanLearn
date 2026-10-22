"""Extract the pinned AMD Maya bind geometry without executing MEL.

Produces offline interchange JSON for vl_female_fbx, never a runtime format.
Requires the same NumPy/SciPy environment as import_female_scene.py.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

import numpy as np

from import_female_scene import (
    MAYA_SHA256, NODE, NAME, REVISION, mesh_geometry, read_nodes,
    require_identity_chain, write_json,
)


ROOT = "|Group|DeformationSystem|Root_M"


def matrix(body, attr):
    match = re.search(rf'setAttr "{re.escape(attr)}" -type "matrix"\s+([^;]+);', body)
    if not match:
        raise ValueError(f"Missing saved bind matrix: {attr}")
    values = np.fromstring(match.group(1), sep=" ")
    if len(values) != 16 or not np.isfinite(values).all():
        raise ValueError(f"Invalid saved bind matrix: {attr}")
    # Maya 使用行向量；离线交换和 Eigen 一律使用列向量，平移位于最后一列。
    return values.reshape(4, 4).T


def skin(body, name, text, count, joint_models):
    influences = {}
    for joint, index in re.findall(
            rf'connectAttr "([^"]+)\.wm" "{name}\.ma\[(\d+)\]";', text):
        index = int(index)
        if index in influences or joint not in joint_models:
            raise ValueError(f"Ambiguous/outside deformation tree: {name}/{joint}")
        influences[index] = joint
    inverse_binds = {}
    error = 0.0
    geometry = matrix(body, ".gm")
    if not np.allclose(geometry, np.eye(4), atol=1e-10, rtol=0):
        raise ValueError("Pinned mesh geometry matrix must be identity")
    for index, joint in influences.items():
        inverse = matrix(body, f".pm[{index}]")
        residual = joint_models[joint] @ inverse - geometry
        if np.max(np.abs(residual)) > 1e-6:
            raise ValueError(f"Bind pose and inverse bind disagree: {name}/{joint}")
        error = max(error, float(np.max(np.abs(residual))))
        inverse_binds[joint] = inverse.flatten().tolist()
    declared = int(re.search(r'setAttr -s (\d+) "\.wl";', body).group(1))
    if count != declared:
        raise ValueError(f"Geometry/skin vertex count mismatch: {name}")
    saved = [dict() for _ in range(count)]
    covered = np.zeros(count, dtype=bool)
    negative_residue = []
    for block in re.finditer(r'setAttr "\.wl\[(\d+):(\d+)\]\.w"\s+([^;]+);', body):
        first, last = int(block[1]), int(block[2])
        if first < 0 or last >= count or first > last:
            raise ValueError(f"Invalid skin range: {name}")
        # compound 数组每顶点先写影响数，再写逻辑 index/weight；不能按行切分。
        # Maya 会在命令边界拆开一个顶点的 compound；重叠顶点必须按逻辑骨索引合并。
        tokens = iter(block[3].split())
        for vertex_index in range(first, last + 1):
            vertex = saved[vertex_index]
            covered[vertex_index] = True
            for _ in range(int(next(tokens))):
                index, weight = int(next(tokens)), float(next(tokens))
                if index not in influences or index in vertex or weight < -1e-8 or not np.isfinite(weight):
                    raise ValueError(f"Invalid skin influence: {name}/vertex={vertex_index}/"
                                     f"index={index}/weight={weight}")
                vertex[index] = weight
                if weight < 0:
                    # 固定源有 12 个约 1e-9 的负残差；仅在离线转换丢弃并逐项留证，不改变运行时契约。
                    negative_residue.append({"vertex": vertex_index, "joint": influences[index],
                                             "weight": weight})
        if next(tokens, None) is not None:
            raise ValueError(f"Trailing skin tokens: {name}")
    if not covered.all():
        raise ValueError(f"Incomplete skin: {name}")
    weights = [[[influences[index], weight] for index, weight in vertex.items() if weight > 0]
               for vertex in saved]
    max_sum_error = max(abs(sum(weight for _, weight in vertex) - 1) for vertex in weights)
    if any(not vertex for vertex in weights) or max_sum_error > 1e-4:
        raise ValueError(f"Unweighted/unnormalized source vertex: {name}")
    return weights, inverse_binds, {
        "vertices": count, "declaredInfluences": len(influences),
        "maxInfluences": max(map(len, weights)), "maxWeightSumError": max_sum_error,
        "maxBindMatrixError": error,
        "discardedNegativeResidue": negative_residue,
    }


def uv_data(node, faces, channel):
    attr = rf"\.uvst\[{channel}\]\.uvsp"
    count = int(re.search(rf'setAttr -s (\d+) "{attr}";', node.body).group(1))
    uv = np.empty((count, 2))
    filled = np.zeros(count, dtype=bool)
    for match in re.finditer(
            rf'setAttr "{attr}\[(\d+):(\d+)\]"(?: -type "float2")?\s+([^;]+);', node.body):
        first, last = int(match[1]), int(match[2])
        values = np.fromstring(match[3], sep=" ")
        if last >= count or filled[first:last + 1].any() or len(values) != 2 * (last - first + 1):
            raise ValueError("Invalid UV range")
        uv[first:last + 1] = values.reshape(-1, 2)
        filled[first:last + 1] = True
    if not filled.all() or not np.isfinite(uv).all():
        raise ValueError("Incomplete UV coordinates")
    assignments = []
    current = -1
    for block in re.finditer(r'setAttr "\.fc\[\d+:\d+\]"(?: -type "polyFaces")?\s+([^;]+);', node.body):
        for line in block[1].splitlines():
            tokens = line.split()
            if not tokens:
                continue
            if tokens[0] == "f":
                current += 1
                assignments.append([-1] * len(faces[current]))
            elif tokens[0] == "mu" and int(tokens[1]) == channel:
                indices = list(map(int, tokens[3:]))
                if len(indices) != int(tokens[2]) or len(indices) != len(faces[current]):
                    raise ValueError("UV polygon arity mismatch")
                if min(indices) < 0 or max(indices) >= count:
                    raise ValueError("UV index out of bounds")
                assignments[current] = indices
    if len(assignments) != len(faces):
        raise ValueError("UV/face count mismatch")
    return uv, assignments


def mesh_payload(node, label, weights, inverse_binds, uv_channel):
    positions, faces = mesh_geometry(node)
    uv, assignments = uv_data(node, faces, uv_channel)
    # 源文件有未分配 UV 的面；用明确的缺失哨兵分离顶点，不能借邻面 UV 冒充作者数据。
    # FBX 要求完整 UV 流，哨兵写 (0,0)，缺失面数量进入审计；纹理接入前须修复这些面。
    vertices, texcoords, expanded_weights, polygons = [], [], [], []
    mapping = {}
    missing = 0
    for face, uv_face in zip(faces, assignments):
        polygon = []
        missing += int(all(index < 0 for index in uv_face))
        for source, uv_index in zip(face, uv_face):
            key = (source, uv_index)
            if key not in mapping:
                mapping[key] = len(vertices)
                vertices.append(positions[source].tolist())
                texcoords.append(uv[uv_index].tolist() if uv_index >= 0 else [0, 0])
                expanded_weights.append(weights[source])
            polygon.append(mapping[key])
        polygons.append(polygon)
    return {
        "name": label, "vertices": vertices, "uv": texcoords, "faces": polygons,
        "weights": expanded_weights, "inverseBinds": inverse_binds,
    }, {"sourceVertices": len(positions), "polygons": len(faces), "uvSet": uv_channel,
        "missingUvFaces": missing, "seamVertices": len(vertices)}


def extract(archive, output):
    if output.exists():
        raise ValueError(f"Refusing to overwrite: {output}")
    with zipfile.ZipFile(archive) as zipped:
        raw = zipped.read("Female_Outside.ma")
    if hashlib.sha256(raw).hexdigest() != MAYA_SHA256:
        raise ValueError("Unsupported Maya source digest")
    text = raw.decode("cp936")
    if "currentUnit -l centimeter -a degree -t film;" not in text:
        raise ValueError("Source units differ")
    nodes = read_nodes(text)
    models = {}
    joints = [{"name": "FemaleRoot", "parent": -1, "restLocal": np.eye(4).flatten().tolist()}]
    indices = {}
    for path, node in nodes.items():
        if path != ROOT and not path.startswith(ROOT + "|"):
            continue
        if node.kind != "joint":
            raise ValueError("Non-joint deformation descendant")
        name = path.rsplit("|", 1)[-1]
        model = matrix(node.body, ".bps")
        parent_name = node.parent.rsplit("|", 1)[-1]
        parent_model = models[parent_name] if path != ROOT else np.eye(4)
        local = np.linalg.inv(parent_model) @ model
        if name in models:
            raise ValueError("Duplicate deformation joint")
        models[name] = model
        indices[name] = len(joints)
        joints.append({"name": name, "parent": indices[parent_name] if path != ROOT else 0,
                       "restLocal": local.flatten().tolist()})
    if len(joints) != 105:
        raise ValueError("Pinned deformation skeleton changed")
    blocks = list(NODE.finditer(text))
    clusters = {}
    for index, match in enumerate(blocks):
        if match[1] == "skinCluster":
            end = blocks[index + 1].start() if index + 1 < len(blocks) else len(text)
            clusters[NAME.search(match[2])[1]] = text[match.end():end]
    meshes, report = [], {}
    for name, path, cluster, channel, count in (
        ("body", "|female_mod|BodyModel|BodyModelShapeOrig", "skinCluster4", 0, 235490),
        ("eyes", "|female_mod|yanjing|yanjingShapeOrig", "skinCluster3", 1, 2118),
        ("scalpProxy", "|tfxmesh|tfxmeshShapeOrig", "skinCluster5", None, 15226),
    ):
        print(f"Extracting {name} real skin...", flush=True)
        node = nodes[path]
        require_identity_chain(node, nodes)
        weights, inverse, audit = skin(clusters[cluster], cluster, text, count, models)
        if channel is not None:
            payload, geometry_audit = mesh_payload(node, name, weights, inverse, channel)
            meshes.append(payload)
            audit.update(geometry_audit)
        else:
            # scalpProxy 是无 UV 的碰撞/发根参考，不把它叠加成角色的第三层可见皮肤。
            proxy_positions, proxy_faces = mesh_geometry(node)
            if len(proxy_positions) != count:
                raise ValueError("Proxy vertex count mismatch")
            audit["polygons"] = len(proxy_faces)
            audit["exported"] = False
        report[name] = audit
    output.parent.mkdir(parents=True, exist_ok=True)
    write_json(output, {
        "format": "vl.female-bind-interchange.v1", "sourceSha256": MAYA_SHA256,
        "sourceRevision": REVISION, "root": "FemaleRoot", "joints": joints,
        "meshes": meshes, "audit": report,
    })
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    extract(args.archive, args.output)
