"""Import the pinned AMD Female_Outside saved geometry into a formal VL map.

Requires numpy/scipy. This is deliberately not a general Maya scene evaluator:
only the audited archive SHA256, identity DAG transforms, saved non-rational
open curves and Orig polygon meshes are accepted. No MEL is executed.
"""

import argparse
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zipfile

import numpy as np
import scipy
from scipy.interpolate import BSpline


REVISION = "61f8dd9b51b88d41937ef8d73618aac778642f41"
MAYA_SHA256 = "ee1e944273f4b27004e65b2915117f281a9b585ed757f1e0a7f40f27eb71ed65"
MAP = Path("Maps/SC_tressfx_female")
NODE = re.compile(r"^createNode (\w+)\b([^;]*);", re.MULTILINE)
NAME = re.compile(r'-n "([^"]+)"')
PARENT = re.compile(r'-p "([^"]+)"')


@dataclass
class SavedNode:
    kind: str
    path: str
    parent: str
    body: str


def read_nodes(text):
    """Resolve DAG names while reading; duplicate short guide names stay distinct."""
    matches = list(NODE.finditer(text))
    nodes, latest = {}, {}
    for index, match in enumerate(matches):
        kind, options = match.groups()
        if kind not in ("transform", "joint", "mesh", "nurbsCurve"):
            continue
        name = NAME.search(options).group(1)
        parent_match = PARENT.search(options)
        parent = ""
        if parent_match:
            reference = parent_match.group(1)
            parent = reference if reference.startswith("|") else latest[reference]
            if parent not in nodes:
                raise ValueError(f"Unresolved DAG parent: {parent}")
        path = f"{parent}|{name}"
        if path in nodes:
            raise ValueError(f"Duplicate DAG path: {path}")
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        nodes[path] = SavedNode(kind, path, parent, text[match.end():end])
        latest[name] = path
    return nodes


def require_identity_chain(node, nodes):
    # CV 和 Orig 网格在此文件中已经位于厘米/Y-up 空间；只有整条 DAG 为恒等变换才可共用。
    # 非零 pivot 本身不变换几何，不能把它误当成平移。拒绝有值的 TRS/剪切/轴旋转。
    path = node.parent
    while path:
        parent = nodes[path]
        if parent.kind != "transform":
            raise ValueError(f"Unsupported transform ancestor: {path}")
        for attribute, default in (("t", [0, 0, 0]), ("r", [0, 0, 0]),
                                   ("s", [1, 1, 1]), ("sh", [0, 0, 0]),
                                   ("ra", [0, 0, 0])):
            for match in re.finditer(
                    rf'setAttr\s+"\.{attribute}"\s+-type "double3"\s+([^;]+);', parent.body):
                values = np.fromstring(match.group(1), sep=" ")
                if not np.array_equal(values, default):
                    raise ValueError(f"Nonidentity {attribute}: {path}")
        if 'offsetParentMatrix' in parent.body:
            raise ValueError(f"Unsupported matrix: {path}")
        path = parent.parent


def sample_curve(node, count):
    match = re.search(r'setAttr "\.cc" -type "nurbsCurve"\s+([^;]+);', node.body)
    if not match:
        raise ValueError(f"No saved curve: {node.path}")
    tokens = match.group(1).split()
    degree, spans, form = map(int, tokens[:3])
    rational, dimension = tokens[3], int(tokens[4])
    if degree != 2 or form != 0 or rational != "no" or dimension != 3:
        raise ValueError(f"Unsupported curve specification: {node.path}")
    knot_count = int(tokens[5])
    knots = np.array(tokens[6:6 + knot_count], dtype=np.float64)
    cv_count = int(tokens[6 + knot_count])
    cvs = np.array(tokens[7 + knot_count:], dtype=np.float64).reshape((-1, 3))
    if len(cvs) != cv_count or cv_count != spans + degree or knot_count != cv_count + degree - 1:
        raise ValueError(f"Malformed curve counts: {node.path}")
    if not np.isfinite(cvs).all() or not np.isfinite(knots).all() or np.any(np.diff(knots) < 0):
        raise ValueError(f"Invalid curve payload: {node.path}")
    # Maya 比标准 B-spline 少首尾各一个 knot；本源文件是端点夹持的开放曲线。
    # 用 SciPy 的 BSpline 求值，保留 exporter 的均匀参数采样，不把 CV 当折线。
    if knots[0] != 0 or not np.all(knots[:degree] == 0) or not np.all(knots[-degree:] == spans):
        raise ValueError(f"Unsupported unclamped/domain curve: {node.path}")
    full_knots = np.concatenate(([knots[0]], knots, [knots[-1]]))
    samples = BSpline(full_knots, cvs, degree, extrapolate=False)(np.linspace(0, spans, count))
    if not np.allclose(samples[[0, -1]], cvs[[0, -1]], atol=1e-10, rtol=0):
        raise ValueError(f"Curve endpoint mismatch: {node.path}")
    samples = samples.astype("<f4")
    if not np.isfinite(samples).all() or np.any(np.linalg.norm(np.diff(samples, axis=0), axis=1) == 0):
        raise ValueError(f"Degenerate sampled curve: {node.path}")
    return samples


def read_array(body, attribute, width, dtype):
    count_match = re.search(rf'setAttr -s (\d+)\s+"\.{attribute}";', body)
    if not count_match:
        raise ValueError(f"Missing array size: {attribute}")
    count = int(count_match.group(1))
    result = np.empty((count, width), dtype=dtype)
    filled = np.zeros(count, dtype=bool)
    for match in re.finditer(rf'setAttr "\.{attribute}\[(\d+):(\d+)\]"\s+([^;]+);', body):
        first, last = int(match.group(1)), int(match.group(2))
        values = np.fromstring(match.group(3), sep=" ", dtype=dtype)
        if first > last or last >= count or filled[first:last + 1].any() or len(values) != (last - first + 1) * width:
            raise ValueError(f"Malformed array range: {attribute}")
        result[first:last + 1] = values.reshape((-1, width))
        filled[first:last + 1] = True
    if not filled.all() or not np.isfinite(result).all():
        raise ValueError(f"Incomplete/nonfinite array: {attribute}")
    return result


def mesh_geometry(node):
    vertices = read_array(node.body, "vt", 3, np.float64)
    edges = read_array(node.body, "ed", 3, np.int64)[:, :2]
    if edges.min() < 0 or edges.max() >= len(vertices):
        raise ValueError(f"Out-of-range mesh edge: {node.path}")
    count = int(re.search(r'setAttr -s (\d+) -ch \d+ "\.fc";', node.body).group(1))
    faces = []
    consumed = 0
    for block in re.finditer(r'setAttr "\.fc\[(\d+):(\d+)\]"(?: -type "polyFaces")?\s+([^;]+);', node.body):
        first, last, payload = int(block.group(1)), int(block.group(2)), block.group(3)
        if first != consumed or re.search(r"^\s*h\s", payload, re.MULTILINE):
            raise ValueError(f"Noncontiguous/holed mesh faces: {node.path}")
        block_faces = []
        for face in re.finditer(r"^\s*f (\d+) ([^\r\n]+)", payload, re.MULTILINE):
            edge_ids = np.fromstring(face.group(2), sep=" ", dtype=np.int64)
            if len(edge_ids) != int(face.group(1)) or len(edge_ids) < 3:
                raise ValueError(f"Unsupported polygon: {node.path}")
            # Maya 的负边索引为 -(index+1)，表示沿反向遍历；必须验证闭环，不能只取第一顶点。
            indices = np.where(edge_ids < 0, -edge_ids - 1, edge_ids)
            if indices.max() >= len(edges):
                raise ValueError(f"Invalid face edge: {node.path}")
            directed = edges[indices].copy()
            directed[edge_ids < 0] = directed[edge_ids < 0, ::-1]
            if not np.array_equal(directed[:, 1], np.roll(directed[:, 0], -1)):
                raise ValueError(f"Broken face loop: {node.path}")
            block_faces.append(directed[:, 0].tolist())
        if len(block_faces) != last - first + 1:
            raise ValueError(f"Incomplete face block: {node.path}")
        faces.extend(block_faces)
        consumed = last + 1
    if consumed != count:
        raise ValueError(f"Incomplete face stream: {node.path}")
    return vertices, faces


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=4, allow_nan=False) + "\n", encoding="utf-8")


def write_obj(path, label, vertices, faces):
    # 原包不带贴图，当前只迁移静态几何；不伪造 UV/原材质。Assimp 负责三角化和生成平滑法线。
    with path.open("w", encoding="ascii", newline="\n") as stream:
        stream.write(f"# AMD Female_Outside saved rest geometry, centimeters, Y-up\n"
                     f"# Source revision {REVISION}; see ATTRIBUTION.md\n"
                     f"o {label}\ns 1\n")
        for vertex in vertices:
            stream.write("v %.9g %.9g %.9g\n" % tuple(vertex))
        for face in faces:
            stream.write("f " + " ".join(str(index + 1) for index in face) + "\n")


def import_scene(reference, resource_root):
    target = resource_root / MAP
    if target.exists():
        raise ValueError(f"Refusing to overwrite existing content: {target}")
    archive = reference / "Asset/Maya2018/Female_Outside/Female_Outside.zip"
    with zipfile.ZipFile(archive) as source:
        raw = source.read("Female_Outside.ma")
    if hashlib.sha256(raw).hexdigest() != MAYA_SHA256:
        raise ValueError("Unsupported source SHA256; re-audit before importing a different scene")
    text = raw.decode("cp936")
    if "currentUnit -l centimeter -a degree -t film;" not in text:
        raise ValueError("Source must explicitly declare centimeters")
    nodes = read_nodes(text)
    guides = [node for node in nodes.values() if node.kind == "nurbsCurve" and
              node.path.startswith("|xgGroom|xgCurvesFromGuides1|")]
    excluded = [node for node in nodes.values() if node.kind == "nurbsCurve" and
                node.path.startswith("|xgGroom|xgCurvesFromGuides2|")]
    if len(guides) != 40334 or len(excluded) != 308:
        raise ValueError(f"Unexpected guide groups: {len(guides)}, {len(excluded)}")
    print(f"Sampling {len(guides)} saved curves; separate group2 ({len(excluded)}) excluded.", flush=True)
    points = 32
    samples = np.empty((len(guides), points, 4), dtype="<f4")
    specs = Counter()
    for index, guide in enumerate(guides):
        require_identity_chain(guide, nodes)
        samples[index, :, :3] = sample_curve(guide, points)
        samples[index, :, 3] = 1
        samples[index, :2, 3] = 0
        specs[re.search(r'"nurbsCurve"\s+(\d+ \d+)', guide.body).group(1)] += 1
    meshes = {}
    for label, path in (("body", "|female_mod|BodyModel|BodyModelShapeOrig"),
                        ("eyes", "|female_mod|yanjing|yanjingShapeOrig")):
        node = nodes[path]
        require_identity_chain(node, nodes)
        meshes[label] = mesh_geometry(node)
    # 所有源验证和几何求值成功后才创建新内容域；从不覆盖已有场景或执行 Maya 的连接/蒙皮图。
    source_folder = target / "Source"
    source_folder.mkdir(parents=True, exist_ok=False)
    header = struct.pack("<fIIIIIII32I", 4, len(guides), points, 160, 0, 0, 0, 0, *([0] * 32))
    (source_folder / "female_guides.tfx").write_bytes(header + samples.tobytes())
    prefix = MAP.as_posix()
    for label, (vertices, faces) in meshes.items():
        write_obj(source_folder / f"female_{label}.obj", label, vertices, faces)
        write_json(target / f"Meshes/SM_female_{label}.json", {
            "name": f"Female {label} saved rest geometry", "type": "mesh",
            "modelDataPath": f"{prefix}/Source/female_{label}.obj",
            "importOptions": {"generateSmoothNormals": True},
            "materialSlots": [{"name": "neutral", "materialInstancePath": f"{prefix}/Materials/MI_female_{label}.json"}],
        })
        write_json(target / f"Materials/MI_female_{label}.json", {
            "name": f"Female {label} neutral geometry preview", "type": "materialInstance",
            "material": "shader/glsl/M_pbr.json",
            "parameters": {"u_tintColor": [0.42, 0.45, 0.48, 1] if label == "body" else [0.1, 0.11, 0.12, 1],
                           "u_pbrFactors": [0.7, 0, 1, 0]},
        })
    write_json(target / "Meshes/SM_female_hair.json", {
        "name": "Female Outside saved guide curves", "type": "tressfx",
        "modelDataPath": f"{prefix}/Source/female_guides.tfx", "strandRadius": 0.012,
        "materialSlots": [{"name": "hair", "materialInstancePath": f"{prefix}/Materials/MI_female_hair.json"}],
    })
    write_json(target / "Materials/MI_female_hair.json", {
        "name": "Female pink Unlit guide preview", "type": "materialInstance",
        "material": "shader/glsl/hair/M_strandProbe.json",
        "parameters": {"u_tintColor": [0.65, 0.08, 0.22, 1]},
    })
    objects = [{"name": f"Female_{label}", "type": "mesh",
                "modelPath": f"{prefix}/Meshes/SM_female_{label}.json",
                "position": [0, 0, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1]}
               for label in ("body", "eyes", "hair")]
    objects.extend([
        {"name": "PortraitCamera", "type": "camera", "position": [50, 165, 210],
         "look_at": [0, 151, 0], "rotation": [0, 0, 0], "scale": [1, 1, 1],
         "fov": 38, "near_clip": 0.5, "far_clip": 1000,
         "exposure": {"mode": "ev100", "ev100": 0, "compensation": 0,
                      "aperture": 4, "shutterSeconds": 0.008, "iso": 100,
                      "autoMinEv100": -10, "autoMaxEv100": 20}},
        {"name": "PortraitKey", "type": "directionalLight", "position": [0, 200, 100],
         "rotation": [-35, -30, 0], "color": [1, 0.96, 0.92], "intensity": 3,
         "intensityUnit": "lux", "shadow": {"dynamicShadowDistance": 500}},
        {"name": "NeutralStudio", "type": "environment", "environment": {
            "type": "hdri", "hdrPath": "Common/Environments/UE_AssetsPreview/EpicQuadPanorama_Gray.hdr",
            "cubeSize": 256, "intensity": 1}},
    ])
    write_json(target / "SC_tressfx_female.json", {
        "name": "SC_tressfx_female", "type": "scene", "worldUnit": "cm",
        "description": "TressFX5-5.4.4 Female_Outside 真实资产静态审阅关卡："
                       "从 Maya 保存的 xgCurvesFromGuides1 曲线与角色 Orig 网格离线提取，"
                       "40334 guides、每根32点、厘米/Y-up，不求值绑定和模拟。"
                       "头发为粉色 Unlit 可见性材质，原包未带皮肤/眼睛贴图，角色使用中性材质。"
                       "学习与资产加载检查，不作为 Marschner、抗锯齿或 UE 成片复现验收。",
        "objects": objects,
    })
    notice = (
        "# AMD Female_Outside static saved geometry\n\n"
        f"Source: GPUOpenSoftware/UnrealEngine, TressFX5-5.4.4, revision {REVISION}.\n"
        "Input: Asset/Maya2018/Female_Outside/Female_Outside.zip / Female_Outside.ma.\n"
        f"Source member SHA256: {MAYA_SHA256}.\n\n"
        "40334 curves from xgCurvesFromGuides1, 32 uniform knot-domain samples each;\n"
        "308 curves in the separate xgCurvesFromGuides2 group are excluded, not deduplicated.\n"
        "Maya shortened clamped knots are expanded for scipy.interpolate.BSpline.\n"
        "Centimeters, Y-up, no axis swap, no UV/skin/collision/follow export.\n"
        "Body and eyes use saved Orig input geometry, not evaluated Maya rig output.\n"
        "Source texture paths are unresolved external files; neutral VL materials are authored.\n"
        "Pink radius/color, camera and lights are VL preview choices, not recovered UE settings.\n"
        "No Maya runtime/Arnold/XGen evaluation is claimed. Original Maya source is not bundled.\n\n"
        "Reproduce with VulkanLearn/tool/hair/import_female_scene.py; requires numpy/scipy.\n"
        "Import audit: Generated/Import/SC_tressfx_female/import-report.json (not a runtime dependency).\n\n"
        "## Source License\n\n"
    )
    (source_folder / "ATTRIBUTION.md").write_text(
        notice + (reference / "LICENSE.txt").read_text(encoding="utf-8"), encoding="utf-8")
    report = {
        "sourceRevision": REVISION, "mayaSha256": MAYA_SHA256,
        "archiveSha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "numpyVersion": np.__version__, "scipyVersion": scipy.__version__,
        "guideCount": len(guides), "excludedGroup2Count": len(excluded),
        "pointsPerStrand": points, "sourceDegreeSpans": dict(specs),
        "hairBounds": {"min": samples[:, :, :3].min(axis=(0, 1)).tolist(),
                       "max": samples[:, :, :3].max(axis=(0, 1)).tolist()},
        "meshes": {label: {"vertices": len(v), "polygons": len(f),
                           "boundsMin": v.min(axis=0).tolist(), "boundsMax": v.max(axis=0).tolist()}
                   for label, (v, f) in meshes.items()},
        "missingSourceTextures": re.findall(r'setAttr "\.ftn" -type "string" "([^"]+)";', text),
        "outputs": {p.relative_to(resource_root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(target.rglob("*")) if p.is_file()},
    }
    write_json(resource_root / "Generated/Import/SC_tressfx_female/import-report.json", report)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-root", type=Path, required=True)
    parser.add_argument("--resource-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(import_scene(args.reference_root.resolve(), args.resource_root.resolve()), indent=2))
    except (ValueError, OSError, KeyError, zipfile.BadZipFile) as error:
        print(f"Female import failed: {error}", file=sys.stderr)
        sys.exit(1)
