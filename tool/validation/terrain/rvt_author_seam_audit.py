"""Read-only CPU audit of the Full16 central author seams, never a GPU gate.

Requires numpy and Pillow (available in the desktop bundled Python).
Only writes under artifacts/terrain-rvt-production/seam-author-audit.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True

import numpy as np
from PIL import Image, ImageDraw

from check_composited_fixture_camera import png16


FAMILIES = {
    "legacy": "Full4x4_R8192_Default",
    "CoverageAtlas": "RVTv2_Full4x4_R8192_Default_CoverageAtlas",
    "LocalLayerArray": "RVTv2_Full4x4_R8192_Default_LocalLayerArray",
}
CENTRAL = (5, 6, 9, 10)
PAIRS = ((5, 6, "x"), (9, 10, "x"), (5, 9, "z"), (6, 10, "z"))
ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "artifacts/terrain-rvt-production/seam-author-audit"


def bilinear(values, positions, repeat=False):
    positions = np.asarray(positions, dtype=np.float64)
    base = np.floor(positions).astype(int)
    fraction = positions - base
    result = np.zeros((len(positions), values.shape[2]), dtype=np.float64)
    for horizontal, vertical in ((0, 0), (1, 0), (0, 1), (1, 1)):
        columns = base[:, 0] + horizontal
        rows = base[:, 1] + vertical
        if repeat:
            columns %= values.shape[1]
            rows %= values.shape[0]
        else:
            columns = np.clip(columns, 0, values.shape[1] - 1)
            rows = np.clip(rows, 0, values.shape[0] - 1)
        factor = (fraction[:, 0] if horizontal else 1 - fraction[:, 0]) * (
            fraction[:, 1] if vertical else 1 - fraction[:, 1])
        result += factor[:, None] * values[rows, columns]
    return result


def top4(values, quantize=False):
    indices = np.argsort(-values, axis=1, kind="stable")[:, :4]
    weights = np.take_along_axis(values, indices, axis=1)
    total = weights.sum(axis=1, keepdims=True)
    empty = total[:, 0] < 1.e-4
    weights = weights / np.maximum(total, 1.e-4)
    indices[empty] = 0
    weights[empty] = [1, 0, 0, 0]
    if quantize:
        weights = np.floor(weights * 255 + .5).astype(np.uint8)
    return indices, weights


def dense4(indices, weights, count=7):
    result = np.zeros((len(indices), count))
    for slot in range(4):
        np.add.at(result, (np.arange(len(indices)), indices[:, slot]), weights[:, slot])
    return result


def normalized(values):
    return values / np.linalg.norm(values, axis=-1, keepdims=True)


def differences(first, second):
    difference = np.abs(np.asarray(first) - np.asarray(second))
    per_sample = np.max(difference.reshape(len(difference), -1), axis=1)
    worst = int(np.argmax(per_sample))
    return {"maxAbs": float(per_sample[worst]), "meanAbs": float(np.mean(difference)),
            "worstIndex": worst, "firstAtWorst": np.asarray(first)[worst].tolist(),
            "secondAtWorst": np.asarray(second)[worst].tolist()}


class Audit:
    def __init__(self, resources):
        self.resources = resources.resolve()
        self.inputs = {}
        self.tiles = {}
        self.worlds = {}
        self.images = {}

    def bytes(self, path):
        path = path.resolve()
        data = path.read_bytes()
        self.inputs[str(path)] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        return data

    def document(self, path):
        return json.loads(self.bytes(path).decode("utf-8-sig"))

    def image(self, name):
        if name not in self.images:
            path = self.resources / name
            self.bytes(path)
            with Image.open(path) as image:
                values = np.asarray(image).copy()
            if values.ndim == 2:
                values = values[:, :, None]
            divisor = 65535 if values.max() > 255 or values.dtype.itemsize > 1 else 255
            self.images[name] = values.astype(float) / divisor
        return self.images[name]

    def load(self):
        schema = self.document(ROOT / "shader/glsl/M_terrain.json")
        defaults = {key: value["default"] for key, value in schema["parameters"].items()}
        for family, namespace in FAMILIES.items():
            base = self.resources / "Generated/Validation/TerrainCompositedVT/Variants" / namespace
            scene = self.document(base / "SC_composited_vt_4x4.json")
            paint_receipt = self.document(base / "paint_semantics.json") if (
                base / "paint_semantics.json").is_file() else None
            placements = [item for item in scene["objects"] if item.get("type") == "terrain"]
            catalog, mapping, terrains, materials = [], {}, {}, {}
            for placement in placements:
                number = int(placement["name"].split("_")[-1])
                terrain = self.document(self.resources / placement["terrainPath"])
                terrains[number] = terrain
                keys = [layer["key"] for layer in terrain["layers"]]
                local = []
                for layer in terrain["layers"]:
                    if layer["key"] not in [item["key"] for item in catalog]:
                        catalog.append(layer)
                    global_id = next(index for index, item in enumerate(catalog)
                                     if item["key"] == layer["key"])
                    if catalog[global_id] != layer:
                        raise ValueError("Conflicting same-key catalog definition")
                    local.append(global_id)
                mapping[number] = local
                if len(set(keys)) != len(keys):
                    raise ValueError("Duplicate layer keys")
                instances = {}
                for field in ("materialInstancePath", "detailMaterialInstancePath"):
                    instance = self.document(self.resources / terrain[field])
                    instances[field] = {
                        "effectiveParameters": defaults | instance.get("parameters", {}),
                        "authoredParameters": instance.get("parameters", {}),
                        "textures": instance.get("textures", {}),
                    }
                materials[number] = instances
                if number not in CENTRAL:
                    continue
                height_path = self.resources / terrain["heightmapPath"]
                self.bytes(height_path)
                height = np.asarray(png16(height_path), dtype=float)
                if height.shape != (515, 515) or terrain["gridResolution"] != 512 or (
                        terrain["heightmapMargin"] != 1):
                    raise ValueError("Audit requires the actual 512 grid / 1 margin fixture")
                height = height / 65535 * terrain["heightScale"] + terrain["heightOffset"]
                paint = self.bytes(self.resources / terrain["paintPath"])
                magic, width, rows, layers, flags, reserved = struct.unpack_from("<4s5I", paint)
                if magic != b"VLP1" or flags or reserved or layers != 7 or (
                        len(paint) != 24 + width * rows * layers * 5):
                    raise ValueError("Invalid complete VLP1")
                weights = np.frombuffer(paint, "<f4", width * rows * layers, 24).reshape(
                    layers, rows, width).transpose(1, 2, 0).copy()
                opacity = np.frombuffer(paint, "u1", width * rows * layers,
                                        24 + width * rows * layers * 4).reshape(
                                            layers, rows, width).transpose(1, 2, 0).copy()
                if not np.isfinite(weights).all() or weights.min() < 0 or weights.max() > 1:
                    raise ValueError("Invalid Paint values")
                if not (opacity == 255).all():
                    raise ValueError("Partial opacity requires procedural base evaluator; do not invent it")
                gradient = np.stack((-(height[1:-1, 2:] - height[1:-1, :-2]) / 393.75,
                                     np.ones((513, 513)),
                                     -(height[2:, 1:-1] - height[:-2, 1:-1]) / 393.75), axis=-1)
                self.tiles[family, number] = {
                    "terrain": terrain, "placement": placement, "height": height,
                    "paint": weights, "opacity": opacity, "normal": normalized(gradient),
                    "instances": instances, "mapping": local,
                    "paintShape": [width, rows, layers],
                    "paintSidecarExists": (self.resources / (terrain["paintPath"] + ".meta.json")).is_file(),
                }
                if placement["rotation"] != [0, 0, 0] or placement["scale"] != [1, 1, 1]:
                    raise ValueError("CPU pairing requires the actual unrotated unit placement")
                meta = self.resources / (terrain["paintPath"] + ".meta.json")
                if meta.exists():
                    self.document(meta)
            recipe_name = next(iter(terrains.values()))["compositedVT"].get("productionSourcePath")
            recipe = self.document(self.resources / recipe_name) if recipe_name else None
            manifest_name = next(iter(terrains.values()))["compositedVT"]["packagePath"]
            self.worlds[family] = {
                "scene": scene, "terrains": terrains, "materials": materials,
                "catalog": catalog, "localToGlobal": mapping,
                "sourceLayerMapping": [
                    {"local": index, "global": mapping[5][index], "sourceSet": layer["sourceSet"],
                     "slice": index, "key": layer["key"]}
                    for index, layer in enumerate(terrains[5]["layers"])],
                "mappingEvidence": "derived from registry order and terrainSourceSets.cpp local slice rule; not GPU readback",
                "authorRecipe": recipe, "manifestPath": manifest_name,
                "manifestExists": (self.resources / manifest_name).is_file(),
                "capturePlan": self.document(base / "capture_plan.json"),
                "paintSemanticsReceipt": paint_receipt,
            }
            for number in CENTRAL:
                tile = self.tiles[family, number]
                textures = tile["instances"]["materialInstancePath"]["textures"]
                tile["sourceDescriptors"] = {}
                for slot in ("layerAlbedo", "layerNormal", "layerPbr", "layerHeight",
                             "layerWeightMaps", "globalColour", "globalMasks"):
                    descriptor = self.document(self.resources / textures[slot])
                    tile["sourceDescriptors"][slot] = descriptor
                    for source in descriptor.get("sources", []):
                        self.bytes(self.resources / source)
                    for source in descriptor.get("displacementSources", []):
                        self.bytes(self.resources / source)
                    for field in ("source", "path"):
                        if field in descriptor and (self.resources / descriptor[field]).is_file():
                            self.bytes(self.resources / descriptor[field])
                imported = tile["sourceDescriptors"]["layerWeightMaps"]
                shape = self.image(imported["sources"][0]).shape
                if shape[0] != shape[1]:
                    raise ValueError("Non-square imported weight array")
                tile["importedWidth"] = shape[0]

    def sample(self, family, number, uv):
        tile = self.tiles[family, number]
        uv = np.asarray(uv, dtype=float)
        paint = tile["paint"]
        margin_uv = (1 + uv * 512) / 514
        coverage_paint = bilinear(paint, margin_uv * [paint.shape[1] - 1, paint.shape[0] - 1])
        direct_paint = bilinear(paint, np.clip(uv, 0, 1) * [paint.shape[1] - 1, paint.shape[0] - 1])
        width = tile["importedWidth"]
        weight_uv = (1 + uv * (width - 3)) / (width - 1)
        positions = weight_uv * width - .5
        base = np.floor(positions).astype(int)
        fraction = positions - base
        local = np.zeros_like(direct_paint)
        for horizontal, vertical in ((0, 0), (1, 0), (0, 1), (1, 1)):
            pixel = np.clip(base + [horizontal, vertical], 0, width - 1)
            surface = ((pixel + .5) / width * (width - 1) - 1) / (width - 3)
            factor = (fraction[:, 0] if horizontal else 1 - fraction[:, 0]) * (
                fraction[:, 1] if vertical else 1 - fraction[:, 1])
            local += factor[:, None] * bilinear(
                paint, np.clip(surface, 0, 1) * [paint.shape[1] - 1, paint.shape[0] - 1])
        coverage = np.zeros_like(local)
        coverage_positions = np.clip(uv * 1024 - .5, 0, 1023)
        coverage_base = np.floor(coverage_positions).astype(int)
        coverage_fraction = coverage_positions - coverage_base
        for horizontal, vertical in ((0, 0), (1, 0), (0, 1), (1, 1)):
            pixel = np.clip(coverage_base + [horizontal, vertical], 0, 1023)
            tap_uv = (1 + (pixel + .5) / 1024 * 512) / 514
            raw = bilinear(paint, tap_uv * [paint.shape[1] - 1, paint.shape[0] - 1])
            indices, weights = top4(raw, quantize=True)
            factor = (coverage_fraction[:, 0] if horizontal else 1 - coverage_fraction[:, 0]) * (
                coverage_fraction[:, 1] if vertical else 1 - coverage_fraction[:, 1])
            coverage += factor[:, None] * dense4(indices, weights.astype(float) / 255)
        coverage_mass = coverage.copy()
        indices, weights = top4(coverage)
        coverage = dense4(indices, weights)
        indices, weights = top4(local)
        local_top = dense4(indices, weights)
        height = bilinear(tile["height"][:, :, None], 1 + uv * 512)[:, 0]
        normal = bilinear(tile["normal"], uv * 512)
        parameters = tile["instances"]["materialInstancePath"]["effectiveParameters"]
        albedos, source_heights = [], []
        for index, layer in enumerate(tile["terrain"]["layers"]):
            rotation = math.radians(layer["uvRotation"])
            layer_uv = uv @ np.array([[math.cos(rotation), math.sin(rotation)],
                                      [-math.sin(rotation), math.cos(rotation)]])
            layer_uv *= parameters["u_uvScale"] * layer["uvScale"]
            source = tile["sourceDescriptors"]["layerAlbedo"]["sources"][index]
            image = self.image(source)
            linear = np.where(image[:, :, :3] <= .04045, image[:, :, :3] / 12.92,
                              ((image[:, :, :3] + .055) / 1.055) ** 2.4)
            albedos.append(bilinear(linear, layer_uv * [image.shape[1], image.shape[0]] - .5, True))
            source = tile["sourceDescriptors"]["layerHeight"]["sources"][index]
            image = self.image(source)
            source_heights.append(bilinear(image, layer_uv * [image.shape[1], image.shape[0]] - .5, True)[:, :1])
        return {"heightCm": height, "frameNormal": normal,
                "paintMeshMarginAll7": coverage_paint, "paintDirectSurfaceAll7": direct_paint,
                "localOverlayAll7": local, "localTop4NormalizedAll7": local_top,
                "coverageIsolatedClampCpuAll7": coverage,
                "coverageIsolatedUnnormalizedMassAll7": coverage_mass,
                "sourceLayerAlbedoLod0LinearAll21": np.concatenate(albedos, axis=1),
                "sourceLayerHeightLod0All7": np.concatenate(source_heights, axis=1),
                "surfaceUv": uv, "heightPaintUv": margin_uv, "localWeightUv": weight_uv,
                "coverageTexelPosition": coverage_positions}

    def cooked_legacy(self, number, uv):
        tile = self.tiles["legacy", number]
        manifest = self.document(self.resources / tile["terrain"]["compositedVT"]["packagePath"])
        surface = next(item for item in manifest["surfaces"]
                       if item["surfaceId"] == f"terrain|Landscape_{number:02}")
        parent = (self.resources / tile["terrain"]["compositedVT"]["packagePath"]).parent
        sidecar = self.document(parent / (surface["dependencyHash"] + ".coverage.json"))
        data = self.bytes(parent / sidecar["package"])
        texel = np.clip(np.floor(np.asarray(uv) * 1024).astype(int), 0, 1023)
        page = texel // 128
        table = list(data[(page[1] * 8 + page[0]) * 4:(page[1] * 8 + page[0]) * 4 + 4])
        physical = np.asarray(table[:2]) * 128 + texel % 128
        offset = int((physical[1] * 1024 + physical[0]) * 4)
        dependency = self.document(parent / (surface["dependencyHash"] + ".dependencies.json"))
        return {"surfaceId": surface["surfaceId"], "texel": texel.tolist(), "page": page.tolist(),
                "mapping": table, "ids": list(data[256 + offset:260 + offset]),
                "weights": list(data[256 + 1024 * 1024 * 4 + offset:260 + 1024 * 1024 * 4 + offset]),
                "worldLayerMapping": dependency["config"].get("buildConfig", {}).get("worldLayerMapping"),
                "scope": "actual legacy cooked texel; not new V2 production or bilinear fragment sample"}

    def ray(self, pixel):
        pose = next(item for item in self.worlds["legacy"]["capturePlan"]["shots"]
                    if item["name"] == "cross_tile_corner")
        eye = np.asarray(pose["position"])
        forward = normalized(np.asarray(pose["look_at"]) - eye)
        right = normalized(np.cross(forward, [0, 1, 0]))
        up = np.cross(right, forward)
        horizontal = ((pixel[0] + .5) / 640 - 1) * math.tan(math.radians(32.5))
        vertical = (1 - (pixel[1] + .5) / 360) * math.tan(math.radians(32.5)) * 720 / 1280
        direction = normalized(forward + horizontal * right + vertical * up)
        breaks = {100., 20000.}
        for origin, speed in ((eye[0], direction[0]), (eye[2], direction[2]),
                              (eye[0] + eye[2], direction[0] + direction[2])):
            if abs(speed) > 1.e-12:
                for line in range(-2048, 2049):
                    distance = (line * 196.875 - origin) / speed
                    if 100 < distance < 20000:
                        breaks.add(distance)

        def macro(distance):
            point = eye + direction * distance
            for number in CENTRAL:
                tile = self.tiles["legacy", number]
                uv = (point[[0, 2]] - np.asarray(tile["placement"]["position"])[[0, 2]]) / 100800 + .5
                if (uv >= 0).all() and (uv <= 1).all():
                    cell = np.minimum(np.floor(uv * 512).astype(int), 511)
                    horizontal, vertical = uv * 512 - cell
                    column, row = cell + 1
                    height = tile["height"]
                    first, second, third, fourth = (height[row, column], height[row, column + 1],
                                                    height[row + 1, column], height[row + 1, column + 1])
                    value = ((1 - horizontal - vertical) * first + horizontal * second + vertical * third
                             if horizontal + vertical <= 1 else
                             (1 - vertical) * second + (horizontal + vertical - 1) * fourth +
                             (1 - horizontal) * third)
                    return point[1] - value, number, uv
            raise ValueError("Ray escaped the central four tiles")

        previous = None
        for distance in sorted(breaks):
            difference, number, uv = macro(distance)
            if previous and previous[1] >= 0 and difference <= 0:
                old_distance, old_difference = previous
                hit_distance = old_distance + (distance - old_distance) * old_difference / (
                    old_difference - difference)
                _, number, uv = macro(hit_distance)
                return number, uv, eye + direction * hit_distance
            previous = distance, difference
        raise ValueError("No first macro triangle hit")

    def run(self):
        references = (
            "tool/validation/terrain/composited_page_reference.h",
            "source/terrain/terrainCompositedWeightOverlay.cpp", "source/terrain/terrainLayerPaint.cpp",
            "source/terrain/terrainWeightMap.cpp", "source/terrain/terrainHeightmap.cpp",
            "source/terrain/terrainMeshBuilder.cpp", "source/terrain/terrainLayerTable.cpp",
            "source/terrain/terrainLayerRegistry.cpp", "source/render/resource/terrain/terrainSourceSets.cpp",
            "source/render/resource/terrainCompositedService.cpp",
            "shader/glsl/terrain/terrainCompositedBakeWorldEvaluator.glsl",
            "shader/glsl/materialFunction/terrainCoverage.glsl",
            "tool/validation/terrain/rvt_author_seam_audit.py",
        )
        for relative in references:
            self.bytes(ROOT / relative)
        test_image = np.array([[[0.], [1.]], [[2.], [3.]]])
        assert abs(bilinear(test_image, [[.25, .75]])[0, 0] - 1.75) < 1.e-14
        assert bilinear(test_image, [[-.5, 0]], True)[0, 0] == .5
        indices, weights = top4(np.array([[.5, .5, 0, 0, 0, 0, 0]]), True)
        assert indices[0].tolist() == [0, 1, 2, 3] and weights[0].tolist() == [128, 128, 0, 0]
        self.load()
        report = {
            "kind": "Full16-central-author-seam-cpu-audit", "gpuStarted": False,
            "scope": "05/06/09/10 exact paired author inputs; not visual acceptance",
            "numericPolicy": "float64 scalar reference; not bit-exact C++ float32/GPU",
            "selfChecks": "bilinear affine field, repeat half texel, top4 tie order / C++ round-half-up PASS",
            "samplingPolicy": {
                "meshPaint": "(1+512*surfaceUV)/514 then VLP1 bilinear (width-1)",
                "localOverlay": "actual helper inverse pixel-center surface UV -> Paint.Blend(surfaceUV), opacity=1",
                "localReader": "current C++ page reference / GLSL textureLod normalized UV * width - 0.5",
                "coverage": "CPU rebuild of top4/round255 taps; isolated per-surface clamp, NOT world boundary routing",
                "frame": "current Frames() 513x513 signed central gradients + constant tangent [1,0,0,1]",
                "sourcePixels": "independent source layer mip0 bilinear in linear albedo / height; not height-blended or final rendered pixels",
                "ray": "exact fixed LOD0 triangle macro intersection, no TES/detail displacement",
            },
            "worlds": {}, "copyComparison": [], "edges": [], "corner": {},
            "screenPoints": [], "missingEvidence": [
                "New V2 manifest/production images and actual sourceLayerMap GPU bytes",
                "New Game normal LOD/TES pictures, quality/readiness/epoch/identity and paired frame/depth data",
                "Legacy image fragment local/global UV, effective mip, neighbour routing, base/detail material and lighting",
                "CPU isolated clamp does not validate world adjacency border/coarse footprint routing",
                "LOD transitions/TES interpolation may differ from fixed macro triangle CPU rays",
            ],
        }
        for family, world in self.worlds.items():
            report["worlds"][family] = {key: value for key, value in world.items()
                                       if key not in ("scene", "terrains", "materials")}
            report["worlds"][family]["centralPlacements"] = {
                number: self.tiles[family, number]["placement"] for number in CENTRAL}
            report["worlds"][family]["centralMaterials"] = {
                number: world["materials"][number] for number in CENTRAL}
            report["worlds"][family]["centralSourceDescriptors"] = {
                number: self.tiles[family, number]["sourceDescriptors"] for number in CENTRAL}
            report["worlds"][family]["centralPaintShapeAndSidecars"] = {
                number: {"shape": self.tiles[family, number]["paintShape"],
                         "importedWidth": self.tiles[family, number]["importedWidth"],
                         "sidecarExists": self.tiles[family, number]["paintSidecarExists"]}
                for number in CENTRAL}
        fractions = np.arange(2049) / 2048
        for first, second, axis in PAIRS:
            first_uv = np.column_stack((np.ones_like(fractions), fractions)) if axis == "x" else (
                np.column_stack((fractions, np.ones_like(fractions))))
            second_uv = first_uv - ([1, 0] if axis == "x" else [0, 1])
            for family in FAMILIES:
                first_sample = self.sample(family, first, first_uv)
                second_sample = self.sample(family, second, second_uv)
                metrics = {key: differences(first_sample[key], second_sample[key])
                           for key in first_sample if key not in ("surfaceUv", "heightPaintUv",
                                                                  "localWeightUv", "coverageTexelPosition")}
                first_world = np.asarray(self.tiles[family, first]["placement"]["position"])[[0, 2]] + (
                    first_uv - .5) * 100800
                second_world = np.asarray(self.tiles[family, second]["placement"]["position"])[[0, 2]] + (
                    second_uv - .5) * 100800
                metrics["worldXZCm"] = differences(first_world, second_world)
                first_page_uv = (np.asarray(self.tiles[family, first]["terrain"]["pageOrigin"]) +
                                 first_uv * 8) / 32
                second_page_uv = (np.asarray(self.tiles[family, second]["terrain"]["pageOrigin"]) +
                                  second_uv * 8) / 32
                metrics["coverageWorldUv"] = differences(first_page_uv, second_page_uv)
                first_mesh_page_uv = first_page_uv - np.clip(first_uv * 512 - 511, 0, 1) * (.5 / 4096)
                second_mesh_page_uv = second_page_uv - np.clip(second_uv * 512 - 511, 0, 1) * (.5 / 4096)
                metrics["meshTexCoord1WorldUv"] = differences(first_mesh_page_uv, second_mesh_page_uv)
                mass = (first_sample["coverageIsolatedUnnormalizedMassAll7"] +
                        second_sample["coverageIsolatedUnnormalizedMassAll7"]) * .5
                indices, weights = top4(mass)
                shared_coverage = dense4(indices, weights)
                angle = np.degrees(np.arccos(np.clip(np.sum(normalized(first_sample["frameNormal"]) *
                                                           normalized(second_sample["frameNormal"]), axis=1), -1, 1)))
                for metric in metrics.values():
                    metric["worstFraction"] = float(fractions[metric["worstIndex"]])
                report["edges"].append({
                    "family": family, "pair": [first, second], "axis": axis, "samples": len(fractions),
                    "metrics": metrics, "maxFrameAngleDegrees": float(angle.max()),
                    "constantTangent": [1, 0, 0, 1],
                    "paintOpacityAll255": True,
                    "pairedEffectiveMaterialParametersEqual": all(
                        self.tiles[family, first]["instances"][field]["effectiveParameters"] ==
                        self.tiles[family, second]["instances"][field]["effectiveParameters"]
                        for field in self.tiles[family, first]["instances"]),
                    "sourceDescriptorsEqual": self.tiles[family, first]["sourceDescriptors"] ==
                    self.tiles[family, second]["sourceDescriptors"],
                    "sourceAlbedoMaxDeltaPerLayer": np.max(np.abs(
                        first_sample["sourceLayerAlbedoLod0LinearAll21"] -
                        second_sample["sourceLayerAlbedoLod0LinearAll21"]).reshape(-1, 7, 3), axis=(0, 2)).tolist(),
                    "meshTexCoord1Scope": "current/legacy mesh max edge interpolated half coverage texel inset; producer uses its own domain->global UV, so do not substitute this for bake or actual Game evidence",
                    "meshFirstClampVsIdealSharedCoverage": differences(
                        first_sample["coverageIsolatedClampCpuAll7"][1:-1], shared_coverage[1:-1]),
                    "worldCoverageSharedTapScope": "interior edge only: ideal equal world UV merges half of each side's quantized boundary taps, identical for both placements; not actual mesh max-edge inset or GPU routing",
                    "centralAdjacencyLinks": [
                        link for link in (self.worlds[family]["authorRecipe"] or {}).get("adjacency", [])
                        if {link["sourceSurfaceId"], link["targetSurfaceId"]} ==
                        {f"terrain|Landscape_{first:02}", f"terrain|Landscape_{second:02}"}],
                    "samplesFile": f"edge-{family}-{first:02}-{second:02}.json",
                })
                samples = {"fraction": fractions.tolist(), "firstUv": first_uv.tolist(),
                           "secondUv": second_uv.tolist(),
                           "first": {key: value.tolist() for key, value in first_sample.items()},
                           "second": {key: value.tolist() for key, value in second_sample.items()},
                           "firstIdealWorldCoverageUv": first_page_uv.tolist(),
                           "secondIdealWorldCoverageUv": second_page_uv.tolist(),
                           "firstMeshTexCoord1": first_mesh_page_uv.tolist(),
                           "secondMeshTexCoord1": second_mesh_page_uv.tolist(),
                           "idealWorldCoverageSharedEdgeInteriorAll7": shared_coverage[1:-1].tolist()}
                (OUTPUT / report["edges"][-1]["samplesFile"]).write_text(
                    json.dumps(samples, separators=(",", ":")), encoding="utf-8")
        for number in CENTRAL:
            uv = [[1 if number in (5, 9) else 0, 1 if number in (5, 6) else 0]]
            report["corner"][number] = {
                family: {key: value[0].tolist() for key, value in self.sample(family, number, uv).items()}
                for family in FAMILIES}
            for family in ("CoverageAtlas", "LocalLayerArray"):
                old, new = self.tiles["legacy", number], self.tiles[family, number]
                report["copyComparison"].append({
                    "family": family, "tile": number,
                    "heightDecodedEqual": bool(np.array_equal(old["height"], new["height"])),
                    "paintWeightsEqual": bool(np.array_equal(old["paint"], new["paint"])),
                    "opacityEqual": bool(np.array_equal(old["opacity"], new["opacity"])),
                    "heightFileBytesEqual": self.inputs[str((self.resources / old["terrain"]["heightmapPath"]).resolve())]["sha256"] ==
                    self.inputs[str((self.resources / new["terrain"]["heightmapPath"]).resolve())]["sha256"],
                    "paintFileBytesEqual": self.inputs[str((self.resources / old["terrain"]["paintPath"]).resolve())]["sha256"] ==
                    self.inputs[str((self.resources / new["terrain"]["paintPath"]).resolve())]["sha256"],
                    "placementTrsEqual": all(old["placement"][key] == new["placement"][key]
                                             for key in ("position", "rotation", "scale")),
                    "layersEqual": old["terrain"]["layers"] == new["terrain"]["layers"],
                    "trChanges": {key: [old["terrain"].get(key), new["terrain"].get(key)]
                                  for key in set(old["terrain"]) | set(new["terrain"])
                                  if old["terrain"].get(key) != new["terrain"].get(key)},
                    "materialParameterChanges": {
                        field: {key: [old["instances"][field]["effectiveParameters"].get(key),
                                      new["instances"][field]["effectiveParameters"].get(key)]
                                for key in old["instances"][field]["effectiveParameters"]
                                if old["instances"][field]["effectiveParameters"].get(key) !=
                                new["instances"][field]["effectiveParameters"].get(key)}
                        for field in old["instances"]},
                })
        for pixel in ((499, 549), (500, 550), (501, 551), (450, 550), (550, 550),
                      (639, 550), (640, 550), (639, 549), (640, 549)):
            number, uv, world = self.ray(pixel)
            report["screenPoints"].append({
                "pixel": pixel, "tile": number, "macroWorldCm": world.tolist(),
                "distanceToSharedXandZCm": np.abs(world[[0, 2]]).tolist(),
                "samples": {family: {key: value[0].tolist() for key, value in
                                     self.sample(family, number, [uv]).items()}
                            for family in FAMILIES},
                "legacyCookTexel": self.cooked_legacy(number, uv),
            })
        layer_phases = []
        for layer in self.worlds["CoverageAtlas"]["catalog"]:
            rotation = math.radians(layer["uvRotation"])
            scale = self.tiles["CoverageAtlas", 5]["instances"]["materialInstancePath"][
                "effectiveParameters"]["u_uvScale"] * layer["uvScale"]
            shifts = np.array([[math.cos(rotation), math.sin(rotation)],
                               [-math.sin(rotation), math.cos(rotation)]]) * scale
            layer_phases.append({
                "key": layer["key"], "scaleProduct": scale,
                "pairedXandZLayerUvShift": shifts.tolist(),
                "repeatPhaseDistanceToInteger": np.abs(shifts - np.round(shifts)).tolist(),
                "scope": "authored per-tile UV phase; noninteger shift is not by itself observed pixel root cause"})
        report["layerUvPhase"] = layer_phases
        chart = Image.new("RGB", (1100, 900), "white")
        plot = ImageDraw.Draw(chart)
        plot.text((15, 10), "CPU author seam max channel delta: mesh-margin paint (blue), local overlay (red), isolated coverage (green)", fill="black")
        for row, (first, second, axis) in enumerate(PAIRS):
            edge = next(item for item in report["edges"]
                        if item["family"] == "LocalLayerArray" and item["pair"] == [first, second])
            data = json.loads((OUTPUT / edge["samplesFile"]).read_text(encoding="utf-8"))
            top = 45 + row * 210
            plot.text((15, top), f"{first:02}/{second:02} {axis} - paired same world position, t=0..1, delta=0..1", fill="black")
            plot.rectangle((60, top + 25, 1060, top + 185), outline="gray")
            for key, colour in (("paintMeshMarginAll7", "blue"), ("localOverlayAll7", "red"),
                                ("coverageIsolatedClampCpuAll7", "green")):
                delta = np.max(np.abs(np.asarray(data["first"][key]) -
                                      np.asarray(data["second"][key])), axis=1)
                points = [(60 + index / 2048 * 1000, top + 185 - float(value) * 160)
                          for index, value in enumerate(delta)]
                plot.line(points, fill=colour)
        chart.save(OUTPUT / "paired-weight-deltas.png")
        screenshot = ROOT / "artifacts/terrain-composited-vt/full16-standard/reader-final/runtime/cross_tile_corner.png"
        self.bytes(screenshot)
        with Image.open(screenshot) as image:
            annotated = image.convert("RGB")
        draw = ImageDraw.Draw(annotated)
        for point in report["screenPoints"]:
            horizontal, vertical = point["pixel"]
            draw.ellipse((horizontal - 3, vertical - 3, horizontal + 3, vertical + 3), outline="red")
        draw.text((15, 75), "LEGACY symptom only: x500 hits tile09 interior; x639/640 cross X=0", fill="red")
        annotated.save(OUTPUT / "legacy-points.png")
        report["inputSha256"] = self.inputs
        changed = []
        for name, record in list(self.inputs.items()):
            if hashlib.sha256(Path(name).read_bytes()).hexdigest() != record["sha256"]:
                changed.append(name)
        report["inputsChangedDuringAudit"] = changed
        report["validSnapshot"] = not changed
        report["sourceReferenceScope"] = "read source and ported sampling formulae, did not execute native/GPU helper"
        (OUTPUT / "audit.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps({"validSnapshot": report["validSnapshot"],
                          "worldManifestExists": {key: value["manifestExists"]
                                                  for key, value in self.worlds.items()},
                          "edges": [{"family": item["family"], "pair": item["pair"],
                                     **{key: value["maxAbs"] for key, value in item["metrics"].items()},
                                     "maxFrameAngleDegrees": item["maxFrameAngleDegrees"]}
                                    for item in report["edges"]]}, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", type=Path, default=ROOT.parent / "VukanLearnResources")
    options = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    Audit(options.resources).run()


if __name__ == "__main__":
    main()
