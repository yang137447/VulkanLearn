"""CPU-only junction_0_0 audit; native mesh vertices and shipped production bytes."""
import hashlib
import json
import math
import re
from pathlib import Path
import subprocess
import sys

sys.dont_write_bytecode = True
import numpy as np
from PIL import Image
from rvt_author_seam_audit import bilinear, normalized, top4, dense4

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "artifacts/terrain-rvt-production/geometry-seam-audit"
BASE = ROOT.parent / "p5g/tourr4"
FAMILY = Path("Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_CoverageAtlas")
INPUTS = {}


def raw(path):
    path = path.resolve()
    data = path.read_bytes()
    INPUTS[str(path)] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    return data


def read(path):
    return json.loads(raw(path).decode("utf-8-sig"))


def save(name, data):
    (OUT / name).write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")


def maximum(first, second):
    return float(np.max(np.abs(np.asarray(first) - np.asarray(second))))


def main():
    candidate = "--candidate" in sys.argv
    OUT.mkdir(parents=True, exist_ok=True)
    plan = read(ROOT / "artifacts/terrain-rvt-production/p5/full-tour-tourr4-w2-smoke-r4/plan.json")
    pose = next(shot for shot in plan["shots"] if shot["name"] == "junction_0_0")
    resources = BASE / "resources"
    scene = read(resources / FAMILY / "SC_composited_vt_4x4.json")
    terrains, placements, materials, requests = {}, {}, {}, []
    schema = read(BASE / "shader/glsl/M_terrain.json")
    defaults = {key: value["default"] for key, value in schema["parameters"].items()}
    for placement in scene["objects"]:
        if placement.get("name") not in pose["surfaces"]:
            continue
        number = int(placement["name"].split("_")[-1])
        placements[number] = placement
        terrain = read(resources / placement["terrainPath"])
        terrains[number] = terrain
        materials[number] = {}
        for field in ("materialInstancePath", "detailMaterialInstancePath"):
            material = read(resources / terrain[field])
            materials[number][field] = {"parameters": defaults | material.get("parameters", {}),
                                        "macros": material.get("macros", {})}
        raw(resources / terrain["heightmapPath"])
        requests.append({"id": number, "tr": str(resources / placement["terrainPath"]),
                         "height": str(resources / terrain["heightmapPath"]),
                         "chunk": [3 if number in (0, 4) else 0, 3 if number in (0, 1) else 0]})
    save("native-request.json", requests)
    native = json.loads(subprocess.check_output([str(OUT / "probe.exe"), str(OUT / "native-request.json")]))
    save("native-vertices.json", native)
    native = {tile["id"]: tile for tile in native}
    manifest_path = resources / terrains[0]["compositedVT"]["packagePath"]
    manifest = read(manifest_path)
    production = manifest["production"]
    surfaces = {int(item["surfaceId"].split("_")[-1]): item for item in production["surfaces"]}
    objects = {item["objectId"]: item for item in production["objects"]}
    images = {}
    if candidate:
        consumer = raw(ROOT / "shader/glsl/materialFunction/mf_terrainLayerBlend.glsl").decode("utf-8")
        assert re.search(r"coverageUv\s*=\s*clamp\(context\.texCoord1,", consumer)
        assert "context.texCoord * (weightSpan - 2.0)" in consumer
        assert "float(topIds[slot])" not in consumer

    def image(object_id):
        if object_id not in images:
            obj = objects[object_id]
            desc = obj["image"]
            data = raw(manifest_path.parent / obj["path"])
            fmt = desc["format"]
            dtype = "<f4" if "32-float" in fmt else "<u2" if "16-unorm" in fmt else "u1"
            channels = 4 if fmt.startswith("rgba") else 2 if fmt.startswith("rg") else 1
            values = np.frombuffer(data, dtype=dtype)
            stride = sum(max(1, desc["width"] >> mip) * max(1, desc["height"] >> mip) * channels
                         for mip in range(desc["mipLevels"]))
            plane = desc["width"] * desc["height"] * channels
            array = np.stack([values[layer * stride:layer * stride + plane].reshape(
                desc["height"], desc["width"], channels) for layer in range(desc["layers"])])
            images[object_id] = array.astype(float) / (65535 if dtype == "<u2" else 255 if dtype == "u1" else 1)
        return images[object_id]

    def coverage(number, uv):
        ids = surfaces[number]["inputs"]
        table, id_atlas, weight_atlas = [image(ids[index])[0] for index in range(3)]
        size = np.array(table.shape[1::-1]) * 128
        position = np.clip(uv * size - .5, 0, size - 1)
        base = np.floor(position).astype(int)
        fraction = position - base
        dense = np.zeros((len(uv), 7))
        for dx, dz in ((0, 0), (1, 0), (0, 1), (1, 1)):
            texel = np.clip(base + [dx, dz], 0, size - 1)
            page = texel // 128
            mapping = table[page[:, 1], page[:, 0]]
            assert np.all(mapping[:, 3] >= .5)
            physical = np.rint(mapping[:, :2] * 255).astype(int) * 128 + texel % 128
            tap_ids = np.rint(id_atlas[physical[:, 1], physical[:, 0]] * 255).astype(int)
            tap_weights = weight_atlas[physical[:, 1], physical[:, 0]]
            factor = (fraction[:, 0] if dx else 1 - fraction[:, 0]) * (
                fraction[:, 1] if dz else 1 - fraction[:, 1])
            for slot in range(4):
                np.add.at(dense, (np.arange(len(uv)), tap_ids[:, slot]), tap_weights[:, slot] * factor)
        return dense4(*top4(dense))

    def displacement(number, uv, weights):
        ids = surfaces[number]["inputs"]
        atlas = image(ids[11])[0]
        mapping = image(ids[12])[0][0]
        catalog = image(ids[4])[0]
        params = materials[number]["detailMaterialInstancePath"]["parameters"]
        total = np.zeros(len(uv))
        for layer in range(7):
            frequency = params["u_uvScale"] * catalog[1, layer, 2] * params["u_detailUvScale"]
            source_size = mapping[layer, 2:4]
            if np.any(source_size < 1):
                continue
            position = np.mod(uv * frequency, 1) * source_size
            page = np.floor(position / 128).astype(int)
            physical = int(mapping[layer, 0]) + page[:, 1] * int(mapping[layer, 1]) + page[:, 0]
            origin = np.column_stack((physical % (atlas.shape[1] // 136), physical // (atlas.shape[1] // 136))) * 136
            height = bilinear(atlas, origin + 4 + position - .5 - page * 128)[:, 0]
            total += weights[:, layer] * (height - .5) * catalog[3, layer, 3] * 100 * params["u_layerDisplacementScale"]
        return total

    eye = np.array(pose["position"])
    forward = normalized(np.array(pose["look_at"]) - eye)
    right = normalized(np.cross(forward, [0, 1, 0]))
    up = np.cross(right, forward)
    focal = 640 / math.tan(math.radians(32.5))

    def project(points):
        relative = points - eye
        depth = relative @ forward
        return np.column_stack((640 + focal * (relative @ right) / depth,
                                360 - focal * (relative @ up) / depth))

    def boundary(number, axis, side, level):
        vertices = native[number]["levels"][level]["edgeVertices"]
        selected = {tuple(vertex["uv"]): vertex for vertex in vertices if vertex["uv"][axis] == side}
        selected = sorted(selected.values(), key=lambda vertex: vertex["uv"][1 - axis])
        return selected

    def samples(number, axis, side, parameters):
        vertices = boundary(number, axis, side, 0)
        positions = np.array([vertex["uv"][1 - axis] for vertex in vertices])
        interpolated = {}
        for field in ("position", "normal", "uv", "uv1"):
            array = np.array([vertex[field] for vertex in vertices])
            interpolated[field] = np.column_stack([np.interp(parameters, positions, array[:, channel])
                                                   for channel in range(array.shape[1])])
        interpolated["position"] += placements[number]["position"]
        interpolated["normal"] = normalized(interpolated["normal"])
        return interpolated

    report = {"scope": "CPU native production mesh + float64 shipped TES-input emulation; no GPU",
              "pose": pose, "materials": materials, "edges": [], "lod": [],
              "shaderIdentity": {}}
    for name in ("engine/passTemplate/base.tesc.glsl", "engine/passTemplate/base.tese.glsl",
                 "materialFunction/mf_terrainLayerBlend.glsl", "materialFunction/terrainCoverage.glsl",
                 "materialFunction/terrainHeightAtlas.glsl"):
        report["shaderIdentity"][name] = {}
        for label, root in (("checkout", ROOT), ("tourr4", BASE), ("C009a", ROOT.parent / "p5g/C009a"),
                            ("C008", ROOT.parent / "p5s/C008")):
            path = root / "shader/glsl" / name
            report["shaderIdentity"][name][label] = hashlib.sha256(raw(path)).hexdigest()
    for number in terrains:
        for level in (1, 2):
            for axis in (0, 1):
                side = 1 if number in ((0, 4) if axis == 0 else (0, 1)) else 0
                fine = {tuple(vertex["uv"]): vertex for vertex in boundary(number, axis, side, 0)}
                coarse = boundary(number, axis, side, level)
                report["lod"].append({"tile": number, "level": level, "axis": axis,
                    "heightDeltaCm": max(abs(vertex["position"][1] - fine[tuple(vertex["uv"])]["position"][1]) for vertex in coarse),
                    "normalDelta": max(maximum(vertex["normal"], fine[tuple(vertex["uv"])]["normal"]) for vertex in coarse),
                    "note": "all retained native boundary vertices; no runtime morph exists"})
    for first, second, axis in ((0, 1, 0), (4, 5, 0), (0, 4, 1), (1, 5, 1)):
        parameter = np.linspace(.975, 1, 1025) if first in (0, 1) and axis == 0 else np.linspace(0, .025, 1025) if axis == 0 else (
            np.linspace(.975, 1, 1025) if first == 0 else np.linspace(0, .025, 1025))
        left, right_sample = samples(first, axis, 1, parameter), samples(second, axis, 0, parameter)
        first_weights = coverage(first, left["uv1"] if candidate else left["uv"])
        second_weights = coverage(second, right_sample["uv1"] if candidate else right_sample["uv"])
        left_d, right_d = displacement(first, left["uv"], first_weights), displacement(second, right_sample["uv"], second_weights)
        left_p = left["position"] + left["normal"] * left_d[:, None]
        right_p = right_sample["position"] + right_sample["normal"] * right_d[:, None]
        pixels_left, pixels_right = project(left_p), project(right_p)
        visible = np.all((pixels_left >= [0, 0]) & (pixels_left < [1280, 720]), axis=1)
        delta = np.linalg.norm(pixels_left - pixels_right, axis=1)
        worst = int(np.argmax(np.where(visible, delta, -1)))
        desired_left = left["uv"] * surfaces[first]["sampling"]["coverageUvScale"] + surfaces[first]["sampling"]["coverageUvOffset"]
        desired_right = right_sample["uv"] * surfaces[second]["sampling"]["coverageUvScale"] + surfaces[second]["sampling"]["coverageUvOffset"]
        record = {"pair": [first, second], "axis": axis, "samples": len(parameter),
                  "worldDeltaCm": maximum(left["position"], right_sample["position"]),
                  "normalDelta": maximum(left["normal"], right_sample["normal"]),
                  "uv1Delta": maximum(left["uv1"], right_sample["uv1"]),
                  "tesWeightDelta": maximum(first_weights, second_weights),
                  "intendedCoverageUvDelta": maximum(desired_left, desired_right),
                  "tesDisplacementDeltaCm": maximum(left_d, right_d),
                  "visibleCount": int(visible.sum()), "maxVisiblePixelGap": float(delta[worst]),
                  "worst": {"parameter": float(parameter[worst]), "world": left["position"][worst].tolist(),
                            "uvLeft": left["uv"][worst].tolist(), "uvRight": right_sample["uv"][worst].tolist(),
                            "weightsLeft": first_weights[worst].tolist(), "weightsRight": second_weights[worst].tolist(),
                            "displacementCm": [float(left_d[worst]), float(right_d[worst])],
                            "pixelLeft": pixels_left[worst].tolist(), "pixelRight": pixels_right[worst].tolist()}}
        desired_wl, desired_wr = coverage(first, desired_left), coverage(second, desired_right)
        record["intendedWorldWeightDelta"] = maximum(desired_wl, desired_wr)
        record["intendedDisplacementDeltaCm"] = maximum(displacement(first, left["uv"], desired_wl),
                                                       displacement(second, right_sample["uv"], desired_wr))
        report["edges"].append(record)
        save(f"edge-{first:02}-{second:02}.json", {
            "parameter": parameter.tolist(), "worldLeft": left["position"].tolist(),
            "worldRight": right_sample["position"].tolist(), "normalLeft": left["normal"].tolist(),
            "normalRight": right_sample["normal"].tolist(), "tesWeightsLeft": first_weights.tolist(),
            "tesWeightsRight": second_weights.tolist(), "displacementLeftCm": left_d.tolist(),
            "displacementRightCm": right_d.tolist(), "pixelsLeft": pixels_left.tolist(), "pixelsRight": pixels_right.tolist()})
    screenshot = ROOT / "artifacts/terrain-rvt-production/p5/full-tour-tourr4-w2-smoke-r4/junction_0_0.bmp"
    raw(screenshot)
    pixels = np.asarray(Image.open(screenshot))[:, :, :3]
    black = np.max(pixels[340:380], axis=2) < 80
    rows, columns = np.where(black)
    report["screenshotDarkPixels"] = {"thresholdMaxRgb": 80, "count": len(rows),
        "bbox": [int(columns.min()), int(rows.min()+340), int(columns.max()), int(rows.max()+340)] if len(rows) else None}
    if candidate:
        mapping_checks = []
        bounded_queries = []
        all_placements = {item["name"]: item for item in scene["objects"] if item.get("type") == "terrain"}
        for number, surface in surfaces.items():
            placement = all_placements[f"Landscape_{number:02}"]
            terrain = read(resources / placement["terrainPath"])
            expected_offset = np.array(surface["sampling"]["coverageUvOffset"])
            expected_scale = np.array(surface["sampling"]["coverageUvScale"])
            assert maximum(np.array(terrain["pageOrigin"])/32, expected_offset) == 0
            assert maximum(np.repeat(terrain["pageTilesPerSide"]/32, 2), expected_scale) == 0
        for link in production["adjacency"]:
            first = int(link["sourceSurfaceId"].split("_")[-1])
            second = int(link["targetSurfaceId"].split("_")[-1])
            edge = link["sourceEdge"]
            along = np.linspace(0, 1, 513)
            uv = np.column_stack((np.repeat(edge, len(along)), along)) if edge < 2 else np.column_stack((along, np.repeat(edge-2, len(along))))
            transform = np.array(link["sourceUvToTargetUv"]).reshape(2, 3)
            target_uv = np.column_stack((uv, np.ones(len(uv)))) @ transform.T
            global_first = uv * surfaces[first]["sampling"]["coverageUvScale"] + surfaces[first]["sampling"]["coverageUvOffset"]
            global_second = target_uv * surfaces[second]["sampling"]["coverageUvScale"] + surfaces[second]["sampling"]["coverageUvOffset"]
            assert maximum(global_first, global_second) == 0
            mesh_first = global_first - np.maximum(uv * 512 - 511, 0) * (.5 / 4096)
            mesh_second = global_second - np.maximum(target_uv * 512 - 511, 0) * (.5 / 4096)
            first_weights = coverage(first, mesh_first)
            second_weights = coverage(second, mesh_second)
            mapping_checks.append({"source": first, "target": second, "edge": edge, "points": len(along),
                "affineUvDelta": 0, "actualMeshUv1Delta": maximum(mesh_first, mesh_second),
                "actualMeshCoverageWeightDelta": maximum(first_weights, second_weights)})
            bounded_queries.extend(np.column_stack((uv, mesh_first)).tolist())
            bounded_queries.extend(np.column_stack((target_uv, mesh_second)).tolist())
        junctions = []
        for row in range(3):
            for column in range(3):
                tiles = [row*4+column, row*4+column+1, (row+1)*4+column, (row+1)*4+column+1]
                corners = [[1, 1], [0, 1], [1, 0], [0, 0]]
                global_uv = [np.array(uv)*surfaces[number]["sampling"]["coverageUvScale"] + surfaces[number]["sampling"]["coverageUvOffset"]
                             for number, uv in zip(tiles, corners)]
                assert maximum(global_uv, np.repeat([global_uv[0]], 4, axis=0)) == 0
                junctions.append({"tiles": tiles, "affineGlobalUv": global_uv[0].tolist()})
        report["all48DirectedAdjacencyMappings"] = mapping_checks
        report["all9JunctionMappings"] = junctions
        assert len(mapping_checks) == 48 and len(junctions) == 9
        baseline = read(ROOT / "artifacts/terrain-rvt-production/geometry-seam-audit/audit.json")
        for before, after in zip(baseline["edges"], report["edges"]):
            assert before["pair"] == after["pair"]
            assert after["tesDisplacementDeltaCm"] < before["tesDisplacementDeltaCm"]
            assert after["maxVisiblePixelGap"] < before["maxVisiblePixelGap"]
        report["mappingScope"] = "Actual 16 runtime TRs and frozen production sampling; affine mapping proof, not all-island native geometry or GPU execution. Mesh max-edge inset retained and measured separately."
        np.asarray(bounded_queries, dtype="<f4").tofile(OUT / "bounded-queries.bin")
        report["boundedQueryCount"] = len(bounded_queries)
    save("audit.json", report)
    save("inputs.json", INPUTS)
    changed = [path for path, identity in INPUTS.items()
               if hashlib.sha256(Path(path).read_bytes()).hexdigest() != identity["sha256"]]
    save("verification.json", {"inputsUnchanged": not changed, "changed": changed, "inputCount": len(INPUTS)})
    print(json.dumps(report["edges"], indent=2))


if __name__ == "__main__":
    if "--candidate" in sys.argv:
        OUT = ROOT / "artifacts/terrain-rvt-production/geometry-seam-fix"
        OUT.mkdir(parents=True, exist_ok=True)
        import shutil
        shutil.copy2(ROOT / "artifacts/terrain-rvt-production/geometry-seam-audit/probe.exe", OUT / "probe.exe")
    main()
