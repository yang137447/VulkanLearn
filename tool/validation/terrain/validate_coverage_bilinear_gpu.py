import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys

from validate_composited_bake_gpu import material_stages, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--validator", default=shutil.which("glslangValidator"))
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    if not options.compiler or not options.validator:
        parser.error("g++ and glslangValidator required")
    root = Path(__file__).resolve().parents[3]
    here = Path(__file__).resolve().parent
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    mappings = [5, 0, 14, 9, 3, 12]
    table = bytearray()
    atlas_ids = bytearray(8 * 8 * 4)
    atlas_weights = [0.0] * (8 * 8 * 4)
    virtual = {}
    for page, physical in enumerate(mappings):
        valid = page != 5
        table.extend((physical % 4, physical // 4, 0, 255 if valid else 0))
        for local_y in range(2):
            for local_x in range(2):
                column = page % 3 * 2 + local_x
                row = page // 3 * 2 + local_y
                layer_ids = [13, 207, 31, 79] if column % 2 == 0 else [207, 13, 101, 153]
                weights = [0.55, 0.25, 0.15, 0.05]
                if row % 2:
                    layer_ids = [31, 13, 231, 79]
                    weights = [0.4, 0.3, 0.2, 0.1]
                if column in (2, 3):
                    layer_ids = [254, 255, 101, 153]
                if column == 0 and row == 0:
                    weights = [0.0] * 4
                if column == 3 and row == 2:
                    layer_ids = [13, 13, 207, 207]
                weights = [struct.unpack("<f", struct.pack("<f", value))[0] for value in weights]
                virtual[column, row] = (layer_ids, weights, valid)
                physical_x = physical % 4 * 2 + local_x
                physical_y = physical // 4 * 2 + local_y
                offset = (physical_y * 8 + physical_x) * 4
                atlas_ids[offset:offset + 4] = bytes(layer_ids)
                atlas_weights[offset:offset + 4] = weights
    queries = [(column / 12, row / 8) for row in range(9) for column in range(13)]
    queries += [
        (-0.1, -0.1), (1.1, 1.1), (0, 0), (1, 1), (0.5, 0.5),
        (1 / 3, 0.25), (2 / 3, 0.25), (0.25, 0.5),
        (0.5, 0.5 - 1 / 1024), (0.5, 0.5 + 1 / 1024),
        (1 / 3 - 1 / 1024, 0.375), (1 / 3 + 1 / 1024, 0.375),
        (2 / 3 - 1 / 1024, 0.75), (2 / 3 + 1 / 1024, 0.75),
        (1 / 12, 1 / 8), (7 / 12, 5 / 8), (11 / 12, 7 / 8),
        (1, 0), (0, 1),
    ]
    assert len(queries) == 136
    queries = [struct.unpack("<2f", struct.pack("<2f", *uv)) for uv in queries]

    def taps(uv, region=False):
        minimum = [0, 0]
        maximum = [1 if region else 5, 3]
        position = [min(1.0, max(0.0, uv[axis])) * extent - 0.5
                    for axis, extent in enumerate((6, 4))]
        base = [math.floor(value) for value in position]
        fractions = [position[axis] - base[axis] for axis in range(2)]
        for vertical in range(2):
            for horizontal in range(2):
                factor = (fractions[0] if horizontal else 1 - fractions[0]) * (
                    fractions[1] if vertical else 1 - fractions[1])
                if factor:
                    yield (min(maximum[0], max(minimum[0], base[0] + horizontal)),
                           min(maximum[1], max(minimum[1], base[1] + vertical))), factor

    def oracle(uv, region=False, selected_ids=None):
        mass = defaultdict(float)
        status = 0
        for location, factor in taps(uv, region):
            layer_ids, weights, valid = virtual[location]
            if not valid:
                status |= 1
                continue
            for layer, weight in zip(layer_ids, weights):
                mass[layer] += factor * weight
        if status:
            return [0] * 4, [0.0] * 4, status
        selected = sorted(((layer, weight) for layer, weight in mass.items() if weight > 0),
                          key=lambda entry: (-entry[1], entry[0]))[:4]
        if selected_ids is not None:
            selected_set = set(selected_ids)
            selected = [(layer, mass[layer]) for layer in selected_ids if mass[layer] > 0]
            omitted = [weight for layer, weight in mass.items()
                       if layer not in selected_set and weight > 0]
            if not selected or (omitted and max(omitted) - min(
                    weight for _, weight in selected) > 2e-7):
                raise AssertionError("GPU selection violates independent top4 cutoff")
        total = sum(weight for _, weight in selected)
        if total < 1e-4:
            return [0] * 4, [1.0, 0.0, 0.0, 0.0], status
        return ([layer for layer, _ in selected] + [0] * 4)[:4], (
            [weight / total for _, weight in selected] + [0.0] * 4)[:4], status

    (output / "table.bin").write_bytes(table)
    (output / "ids.bin").write_bytes(atlas_ids)
    (output / "weights.bin").write_bytes(struct.pack(f"<{len(atlas_weights)}f", *atlas_weights))
    region_queries = set(range(117, 127))
    (output / "queries.bin").write_bytes(b"".join(
        struct.pack("<4f", *uv, float(index in region_queries), 0)
        for index, uv in enumerate(queries)))
    executable = output / "coverage-check.exe"
    run([options.compiler, "-std=gnu++17", "-O2", "-Iextern/VulkanSDK/include",
         here / "composited_bake_gpu_check.cpp", "-Lextern/VulkanSDK/lib/Win32",
         "-lvulkan-1", "-o", executable], root)
    shader = output / "coverage.spv"
    run([options.validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl",
         here / "coverage_bilinear.comp", "-o", shader], root)
    stdout = run([executable, shader, "bounded-coverage-helper", output / "readback.bin", output], root)
    data = (output / "readback.bin").read_bytes()
    maximum = 0.0
    failures = []
    slot_negative_failures = 0
    physical_negative_failures = 0
    foreign_placement_weight = 0.0
    cutoff_rounding_queries = []
    status_queries = 0
    for index, uv in enumerate(queries):
        expected_ids, expected_weights, expected_status = oracle(
            uv, index in region_queries)
        status_queries += int(expected_status != 0)
        actual_ids = list(struct.unpack_from("<4I", data, index * 36))
        actual_weights = list(struct.unpack_from("<4f", data, index * 36 + 16))
        actual_status = struct.unpack_from("<I", data, index * 36 + 32)[0]
        if set(actual_ids) != set(expected_ids):
            selected_ids = [layer for layer, weight in zip(actual_ids, actual_weights) if weight > 0]
            expected_ids, expected_weights, expected_status = oracle(
                uv, index in region_queries, selected_ids)
            cutoff_rounding_queries.append(index)
        expected_mass = defaultdict(float)
        actual_mass = defaultdict(float)
        for layer, weight in zip(expected_ids, expected_weights):
            expected_mass[layer] += weight
        for layer, weight in zip(actual_ids, actual_weights):
            actual_mass[layer] += weight
        expected_mass = {key: value for key, value in expected_mass.items() if value}
        actual_mass = {key: value for key, value in actual_mass.items() if value}
        if index in region_queries:
            foreign_placement_weight = max(
                foreign_placement_weight, actual_mass.get(254, 0), actual_mass.get(255, 0))
        error = max((abs(actual_mass.get(layer, 0) - expected_mass.get(layer, 0))
                    for layer in actual_mass.keys() | expected_mass.keys()), default=0.0)
        maximum = max(maximum, error)
        if error > 2e-6 or actual_status != expected_status:
            failures.append({"query": index, "uv": uv, "error": error,
                             "expected": [expected_ids, expected_weights, expected_status],
                             "actual": [actual_ids, actual_weights, actual_status]})
        weighted_slots = [0.0] * 4
        for location, factor in taps(uv):
            for slot, weight in enumerate(virtual[location][1]):
                weighted_slots[slot] += weight * factor
        nearest = (min(5, max(0, math.floor(min(1, max(0, uv[0])) * 6))),
                   min(3, max(0, math.floor(min(1, max(0, uv[1])) * 4))))
        if index not in region_queries and expected_status == 0 and sum(weighted_slots):
            wrong = defaultdict(float)
            for layer, weight in zip(virtual[nearest][0], weighted_slots):
                wrong[layer] += weight / sum(weighted_slots)
            if any(abs(wrong.get(layer, 0) - expected_mass.get(layer, 0)) > 1e-3
                   for layer in wrong.keys() | expected_mass.keys()):
                slot_negative_failures += 1
        if index not in region_queries and expected_status == 0:
            virtual_x = min(1, max(0, uv[0])) * 6
            virtual_y = min(1, max(0, uv[1])) * 4
            page_x, page_y = min(2, int(virtual_x // 2)), min(1, int(virtual_y // 2))
            physical = mappings[page_y * 3 + page_x]
            atlas_position = (physical % 4 * 2 + virtual_x - page_x * 2 - 0.5,
                              physical // 4 * 2 + virtual_y - page_y * 2 - 0.5)
            wrong = defaultdict(float)
            for vertical in range(2):
                for horizontal in range(2):
                    column = min(7, max(0, math.floor(atlas_position[0]) + horizontal))
                    row = min(7, max(0, math.floor(atlas_position[1]) + vertical))
                    fraction_x = atlas_position[0] % 1
                    fraction_y = atlas_position[1] % 1
                    factor = (fraction_x if horizontal else 1 - fraction_x) * (
                        fraction_y if vertical else 1 - fraction_y)
                    offset = (row * 8 + column) * 4
                    for layer, weight in zip(atlas_ids[offset:offset + 4],
                                             atlas_weights[offset:offset + 4]):
                        wrong[layer] += weight * factor
            selected = sorted(((layer, weight) for layer, weight in wrong.items() if weight > 0),
                              key=lambda entry: (-entry[1], entry[0]))[:4]
            total = sum(weight for _, weight in selected)
            wrong = dict((layer, weight / total) for layer, weight in selected) if total else {0: 1}
            if any(abs(wrong.get(layer, 0) - expected_mass.get(layer, 0)) > 1e-3
                   for layer in wrong.keys() | expected_mass.keys()):
                physical_negative_failures += 1
    material_stages(root, output, options.validator)
    run([options.validator, "-V", "--target-env", "vulkan1.2",
         root / "shader/glsl/terrain/terrainCompositedBake.comp", "-o", output / "bake.spv"], root)
    regressions = {}
    for mode in ("coverage", "repeat", "local", "height", "tail", "missing", "lod", "area"):
        regressions[mode] = run([
            executable, output / "bake.spv", mode, output / f"bake-{mode}.bin"], root)
    report = {
        "passed": not failures and slot_negative_failures > 0 and
        physical_negative_failures > 0 and foreign_placement_weight == 0,
        "queries": len(queries), "independentOracle": "double per-layer virtual lattice",
        "placementRegionQueries": len(region_queries),
        "pureHelperQueries": len(queries) - len(region_queries),
        "missingFootprintNeutralQueries": status_queries,
        "foreignPlacementMaxWeight": foreign_placement_weight,
        "top4CutoffRoundingQueries": cutoff_rounding_queries,
        "top4CutoffRawWeightTolerance": 2e-7,
        "maxAbsoluteWeightError": maximum, "tolerance": 2e-6,
        "slotFilterNegativeFailures": slot_negative_failures,
        "physicalNeighborNegativeFailures": physical_negative_failures,
        "failures": failures, "gpu": stdout, "productionBakeRegression": regressions,
        "sourceSha256": {
            str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in [root / "shader/glsl/materialFunction/terrainCoverage.glsl",
                         root / "shader/glsl/materialFunction/mf_terrainLayerBlend.glsl",
                         root / "shader/glsl/terrain/terrainCompositedBake.comp",
                         here / "coverage_bilinear.comp",
                         here / "composited_bake_gpu_check.cpp", Path(__file__)]},
    }
    (output / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
