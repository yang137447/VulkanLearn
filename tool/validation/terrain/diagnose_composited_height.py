"""Validate the actual production height helper against an independent double oracle."""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import struct

from check_composited_fixture_camera import png16
from height_bilinear_reference import bilinear, interpolate, load_r16, validate_oracle_anchors, verify_storage
from validate_composited_bake_gpu import run
from validate_composited_bounded_source import height_inputs


TEXELS = 136 * 136
R16 = 65535


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def precision_bound(reference, bits):
    values = reference["neighborR16"]
    horizontal = max(abs(values[1] - values[0]), abs(values[3] - values[2]))
    vertical = max(abs(values[2] - values[0]), abs(values[3] - values[1]))
    return (horizontal + vertical) / (1 << bits)


def bracket_model_error(reference, actual, bits):
    grid = 1 << bits
    choices = [[math.floor(weight * grid) / grid, math.ceil(weight * grid) / grid]
               for weight in reference["axisWeights"]]
    candidates = [(abs(interpolate(reference["neighborR16"], [first, second]) - actual),
                   [first, second])
                  for first in choices[0] for second in choices[1]]
    error, weights = min(candidates)
    return {"errorR16": error, "closestQuantizedWeights": weights}


def measure(directory, atlas_extent, bits, source_records):
    atlas_width, atlas_height = atlas_extent
    layer_count = len(source_records)
    columns = atlas_width // 136
    raw = (directory / "samples.bin").read_bytes()
    if len(raw) != 9 * TEXELS * 4:
        raise ValueError("Incomplete diagnostic readback")
    planes = [struct.unpack_from(f"<{TEXELS}f", raw, channel * TEXELS * 4) for channel in range(9)]
    if not all(math.isfinite(value) for plane in planes for value in plane):
        raise ValueError("Nonfinite diagnostic readback")
    source = load_r16(directory / "gpu-source-mip2.bin")
    atlas = load_r16(directory / "gpu-height-atlas.bin")
    if len(source) != 256 * 256 * layer_count or len(atlas) != atlas_width * atlas_height:
        raise ValueError("Incomplete image readback")
    source_slices = [source[layer * 256 * 256:(layer + 1) * 256 * 256] for layer in range(layer_count)]
    names = ("productionHelperVsDouble", "productionVsManualSource", "productionVsHardwareSource",
             "hardwareSourceVsCpu", "cpuNormalizedAtlasVsCpuSource",
             "manualSourceVsCpu", "manualAtlasVsCpuSource", "manualPair", "sourceBracketModel")
    maxima = [{name: 0.0 for name in names} for _ in range(layer_count)]
    bound_violations = [0] * layer_count
    manual_bit_mismatches = 0
    neighbor_mismatches = 0
    canonical_neighbor_mismatches = 0
    neighbor_change_trace = None
    period_source_error = 0.0
    period_helper_error = 0.0
    bound_max = [0.0] * layer_count
    hardware_source_integer_residue = 0.0
    unmapped_neutral_errors = sum(value != 0.5 for value in planes[8])
    traces = []
    strict_failures = [0] * layer_count
    original_trace = None
    original_window_max = 0.0
    worst_oracle_trace = [None] * layer_count
    for index in range(TEXELS):
        slice_index = index % 136 % layer_count
        uv = [planes[4][index], planes[5][index]]
        atlas_uv = [planes[6][index], planes[7][index]]
        source_reference = bilinear(source_slices[slice_index], 256, 256, uv, True)
        atlas_reference = bilinear(atlas, atlas_width, atlas_height, atlas_uv, False)
        neighbor_mismatches += source_reference["neighborR16"] != atlas_reference["neighborR16"]
        page = [math.floor((coordinate - math.floor(coordinate)) * 2) for coordinate in uv]
        slot = slice_index * 4 + page[1] * 2 + page[0]
        canonical_base = [
            slot % columns * 136 + 4 + math.floor(source_reference["position"][0]) - page[0] * 128,
            slot // columns * 136 + 4 + math.floor(source_reference["position"][1]) - page[1] * 128,
        ]
        canonical_neighbors = [atlas[(canonical_base[1] + row) * atlas_width + canonical_base[0] + column]
                               for row, column in ((0, 0), (0, 1), (1, 0), (1, 1))]
        canonical_neighbor_mismatches += canonical_neighbors != source_reference["neighborR16"]
        production_helper, hardware_source, manual_atlas, manual_source = [
            planes[channel][index] * R16 for channel in range(4)]
        hardware_source_integer_residue = max(hardware_source_integer_residue,
                                             abs(hardware_source - round(hardware_source)))
        source_model = bracket_model_error(source_reference, hardware_source, bits)
        errors = {
            "productionHelperVsDouble": abs(production_helper - source_reference["exactR16"]),
            "productionVsManualSource": abs(production_helper - manual_source),
            "productionVsHardwareSource": abs(production_helper - hardware_source),
            "hardwareSourceVsCpu": abs(hardware_source - source_reference["exactR16"]),
            "cpuNormalizedAtlasVsCpuSource": abs(atlas_reference["exactR16"] - source_reference["exactR16"]),
            "manualSourceVsCpu": abs(manual_source - source_reference["exactR16"]),
            "manualAtlasVsCpuSource": abs(manual_atlas - source_reference["exactR16"]),
            "manualPair": abs(manual_atlas - manual_source),
            "sourceBracketModel": source_model["errorR16"],
        }
        for name, error in errors.items():
            maxima[slice_index][name] = max(maxima[slice_index][name], error)
        manual_bit_mismatches += struct.pack("<f", planes[2][index]) != struct.pack("<f", planes[3][index])
        source_bound = precision_bound(source_reference, bits) + 0.5 + 0.05
        bound_max[slice_index] = max(bound_max[slice_index], source_bound)
        bound_violations[slice_index] += errors["hardwareSourceVsCpu"] > source_bound
        strict_failures[slice_index] += errors["productionHelperVsDouble"] > 2
        trace = {
            "pixel": [index % 136, index // 136], "slice": slice_index,
            "uv": uv, "legacyNormalizedAtlasUv": atlas_uv,
            "source": source_reference, "legacyNormalizedAtlasCpu": atlas_reference,
            "hardwareSourceR16": hardware_source, "productionHelperR16": production_helper,
            "manualSourceR16": manual_source, "manualAtlasR16": manual_atlas,
            "errorsR16": errors,
            "hardwareSourceGridModel": source_model,
            "hardwareSourceDiagnosticPrecisionBoundR16": source_bound,
            "canonicalAtlasBase": canonical_base, "canonicalAtlasNeighborR16": canonical_neighbors,
        }
        if worst_oracle_trace[slice_index] is None or errors["productionHelperVsDouble"] >= worst_oracle_trace[slice_index]["errorsR16"]["productionHelperVsDouble"]:
            worst_oracle_trace[slice_index] = trace
        if source_reference["neighborR16"] != atlas_reference["neighborR16"] and neighbor_change_trace is None:
            neighbor_change_trace = trace
        if 16 <= index // 136 < 32:
            original_window_max = max(original_window_max, errors["productionHelperVsDouble"])
            paired_index = index + 64 * 136
            period_source_error = max(period_source_error, abs(planes[1][index] - planes[1][paired_index]) * R16)
            period_helper_error = max(period_helper_error, abs(planes[0][index] - planes[0][paired_index]) * R16)
        if index == 16 * 136 + 4:
            original_trace = trace
        if errors["productionHelperVsDouble"] > 2 or index % 4096 == 0:
            traces.append(trace)
    traces.extend(worst_oracle_trace)
    traces.sort(key=lambda item: item["errorsR16"]["productionHelperVsDouble"], reverse=True)
    retained = traces[:8]
    storage_errors = verify_storage(source, atlas, atlas_width)
    slices = []
    for slice_index, record in enumerate(source_records):
        amplitude = record["displacementM"]
        slices.append({
            **record, "samples": (136 // layer_count + int(slice_index < 136 % layer_count)) * 136,
            "mip2RangeR16": [
                min(source_slices[slice_index]), max(source_slices[slice_index])],
            "maxErrorsR16": maxima[slice_index],
            "productionDoubleOracleTwoR16Failures": strict_failures[slice_index],
            "mmPerR16UnitAtUnitScaleAndFullFade": amplitude * 1000 / R16,
            "twoR16UnitsInMmAtUnitScaleAndFullFade": 2 * amplitude * 1000 / R16,
            "maxProductionDoubleOracleErrorMmAtUnitScaleAndFullFade":
                maxima[slice_index]["productionHelperVsDouble"] / R16 * amplitude * 1000,
            "maxProductionHardwareSourceDifferenceMmAtUnitScaleAndFullFade":
                maxima[slice_index]["productionVsHardwareSource"] / R16 * amplitude * 1000,
            "maxHardwareSourceDoubleOracleErrorMmAtUnitScaleAndFullFade":
                maxima[slice_index]["hardwareSourceVsCpu"] / R16 * amplitude * 1000,
            "hardwareSourceDiagnosticMaxBoundR16": bound_max[slice_index],
            "hardwareSourceDiagnosticBoundViolations": bound_violations[slice_index],
        })
    oracle_passed = (storage_errors == 0 and canonical_neighbor_mismatches == 0 and
                     sum(strict_failures) == 0 and unmapped_neutral_errors == 0 and
                     all(item["productionVsManualSource"] <= 2 and item["manualSourceVsCpu"] <= 0.05
                         for item in maxima))
    report = {
        "criterion": "production-helper-vs-independent-double-source-bilinear-and-stored-data-v1",
        "heightOracleAcceptancePassed": oracle_passed,
        "atlasExtent": list(atlas_extent), "subTexelPrecisionBits": bits,
        "storageTexelsChecked": layer_count * 4 * 136 * 136, "storageMismatches": storage_errors,
        "hardwareSourceR16DistanceToIntegerMax": hardware_source_integer_residue,
        "manualSourceAtlasBitMismatches": manual_bit_mismatches,
        "sourceAtlasFourNeighborValueMismatches": neighbor_mismatches,
        "canonicalAtlasFourNeighborValueMismatches": canonical_neighbor_mismatches,
        "normalizedCoordinateNeighborChangeSample": neighbor_change_trace,
        "repeatBoundaryMaxProductionDoubleErrorR16": original_window_max,
        "original173HistoryUvRetestedWithActualProductionHelper": original_trace,
        "repeatVZeroVsOneHardwareSourceR16": period_source_error,
        "repeatVZeroVsOneProductionHelperR16": period_helper_error,
        "unmappedSourceNeutralHalfErrors": unmapped_neutral_errors,
        "manualCpuToleranceR16": 0.05,
        "productionDoubleOracleThresholdR16": 2,
        "hardwareSourceDifferenceIsAcceptanceGate": False,
        "slices": slices, "worstSamples": retained,
        "proof": "Independent double CPU source oracle on actual GPU mip2 readback; GPU source/atlas self-comparison is supplementary",
    }
    (directory / "analysis.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for trace in retained[:2]:
        print(json.dumps({"failedSample": trace}, separators=(",", ":")))
    print(json.dumps({"original173HistoryUvRetest": original_trace}, separators=(",", ":")))
    print(json.dumps({
        "atlasExtent": report["atlasExtent"], "storageMismatches": storage_errors,
        "manualBitMismatches": manual_bit_mismatches,
        "heightOracleAcceptancePassed": oracle_passed,
        "sliceSummaries": [{"name": item["name"], "maxHelperDoubleR16": item["maxErrorsR16"]["productionHelperVsDouble"],
                           "maxHelperDoubleMm": item["maxProductionDoubleOracleErrorMmAtUnitScaleAndFullFade"],
                           "legacyHardwareSourceDifferenceR16": item["maxErrorsR16"]["productionVsHardwareSource"]}
                          for item in slices],
    }))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--validator", default=shutil.which("glslangValidator"))
    parser.add_argument("--synthetic-only", action="store_true")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    here = Path(__file__).resolve().parent
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    oracle_anchors = validate_oracle_anchors()
    helper_path = root / "shader/glsl/materialFunction/terrainHeightAtlas.glsl"
    helper_sha_before = digest(helper_path)
    log = []
    def execute(arguments):
        text = run(arguments, root)
        log.append({"argv": [str(value) for value in arguments], "stdout": text, "exitCode": 0})
        (output / "commands.json").write_text(json.dumps(log, indent=2) + "\n", encoding="utf-8")
        return text
    executable = output / "height-check.exe"
    shader = output / "height.spv"
    execute([options.compiler, "-std=gnu++17", "-O2", "-Iextern/VulkanSDK/include",
             here / "composited_bake_gpu_check.cpp", "-Lextern/VulkanSDK/lib/Win32",
             "-lvulkan-1", "-o", executable])
    execute([options.validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl",
             here / "composited_height_diagnostic.comp", "-o", shader])
    cases = []
    stress = output / "stress-input"
    stress.mkdir()
    height_inputs(stress)
    synthetic_records = [
        {"name": f"extreme-discontinuity-{index}", "displacementM": 1.0,
         "amplitudeNote": "1m sensitivity example; not a real asset amplitude"}
        for index in range(2)
    ]
    inputs = [("stress", stress / "height-source.bin", False, synthetic_records)]
    if not options.synthetic_only:
        resource = root.parent / "VukanLearnResources"
        descriptor = resource / "Generated/HoudiniPegasusIsland/tile_00/Textures/T_terrain_kitedemo_height_array.json"
        texture = json.loads(descriptor.read_text(encoding="utf-8"))
        terrain_path = resource / "Generated/HoudiniPegasusIsland/tile_00/TR_terrain_houdini_island_tile_00.json"
        terrain = json.loads(terrain_path.read_text(encoding="utf-8"))
        decoded = []
        records = []
        for index, name in enumerate(texture["sources"]):
            path = resource / name
            rows = png16(path)
            if len(rows) != 1024 or len(rows[0]) != 1024:
                raise ValueError("Real height source dimensions changed")
            if texture.get("flipY", True):
                rows.reverse()
            decoded.append(array("H", (value for row in rows for value in row)))
            records.append({
                "name": terrain["layers"][index]["name"], "source": name, "sourceSha256": digest(path),
                "displacementM": terrain["layers"][index]["displacementM"],
                "flipY": texture.get("flipY", True), "sourceLayer": index,
                "mipGeneration": "GPU R16 linear blit 1024->512->256; actual mip2 read back",
                "textureDescriptorSha256": digest(descriptor), "terrainDescriptorSha256": digest(terrain_path),
            })
        for first in range(0, len(decoded), 2):
            second = min(first + 1, len(decoded) - 1)
            path = output / f"real-{first}-{second}-mip0.bin"
            path.write_bytes(decoded[first].tobytes() + decoded[second].tobytes())
            inputs.append((f"real-{first}-{second}", path, True, [records[first], records[second]]))
        path = output / "real-seven-layer-mip0.bin"
        path.write_bytes(b"".join(plane.tobytes() for plane in decoded))
        inputs.append(("real-seven-layer", path, True, records))
    for label, source_path, real, records in inputs:
        for padded in (False, True):
            directory = output / (label + ("-padded" if padded else "-native"))
            directory.mkdir()
            shutil.copyfile(source_path, directory / "height-source.bin")
            mode = "bounded-height" + ("-real" if real else "") + ("-pad" if padded else "")
            text = execute([executable, shader, mode, directory / "samples.bin", directory, len(records)])
            match = re.search(r"subTexelPrecisionBits=(\d+)", text)
            if not match:
                raise RuntimeError("Missing actual device subtexel precision")
            width = re.search(r"heightAtlasWidth=(\d+)", text)
            height = re.search(r"heightAtlasHeight=(\d+)", text)
            if not width or not height:
                raise RuntimeError("Missing actual atlas extent")
            report = measure(directory, (int(width.group(1)), int(height.group(1))), int(match.group(1)), records)
            old_directory = root / "artifacts/terrain-composited-vt/independent-gpu-bake/height-diagnosis-seven-layer-final" / directory.name
            if (old_directory / "analysis.json").is_file():
                old_samples = (old_directory / "samples.bin").read_bytes()
                current_samples = (directory / "samples.bin").read_bytes()
                old_analysis = json.loads((old_directory / "analysis.json").read_text(encoding="utf-8"))
                report["preChangeHistory"] = {
                    "directory": str(old_directory),
                    "sourceMip2BytesUnchanged": digest(directory / "gpu-source-mip2.bin") == digest(old_directory / "gpu-source-mip2.bin"),
                    "atlasBytesUnchanged": digest(directory / "gpu-height-atlas.bin") == digest(old_directory / "gpu-height-atlas.bin"),
                    "tilingUvSamplesUnchanged": current_samples[4*TEXELS*4:6*TEXELS*4] == old_samples[4*TEXELS*4:6*TEXELS*4],
                    "hardwareSourceReadbackUnchanged": current_samples[TEXELS*4:2*TEXELS*4] == old_samples[TEXELS*4:2*TEXELS*4],
                    "sourceImagesAndAmplitudeUnchanged": all(
                        current.get("sourceSha256") == old.get("sourceSha256") and
                        current["displacementM"] == old["displacementM"] and
                        current.get("flipY") == old.get("flipY")
                        for current, old in zip(records, old_analysis["slices"])),
                }
                (directory / "analysis.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            cases.append({"name": directory.name, "realSource": real, "testOnlyPadding": padded,
                          "analysis": report})
    history = root / "artifacts/terrain-composited-vt/independent-gpu-bake/bounded-final/report.json"
    helper_stable = helper_sha_before == digest(helper_path)
    accepted = all(case["analysis"]["heightOracleAcceptancePassed"] for case in cases) and helper_stable
    report = {
        "status": "passed" if accepted else "failed",
        "criterion": "actual-production-helper-vs-independent-double-source-bilinear; <=2 R16 units and exact atlas storage",
        "heightOracleAcceptancePassed": accepted,
        "s1Acceptance": "not evaluated in full by this bounded height suite",
        "oracleAnchors": oracle_anchors,
        "productionHelperSha256": helper_sha_before,
        "productionHelperStableDuringRun": helper_stable,
        "sourceMip2AtlasTilingAmplitudeAndHardwareBaselineUnchanged": all(
            all(value for key, value in case["analysis"].get("preChangeHistory", {}).items() if key != "directory")
            and "preChangeHistory" in case["analysis"] for case in cases),
        "originalFailureHistory": {"path": str(history), "sha256": digest(history)},
        "cpuReference": "Exact double bilinear, source p=fract(uv)*256-0.5, repeat four neighbors; atlas CPU uses emitted float UV and actual GPU-readback pixels",
        "hardwareBaseline": "Original textureLod(source,mip2) remains diagnostic only; improved mathematical filtering is not required to be bit-equal to hardware source",
        "boundInterpretation": "Hardware source precision envelope is diagnostic only and cannot make the production double-oracle gate pass",
        "gridModelInterpretation": "Lower/upper weight-grid model is a diagnostic only; nonzero residual must not be claimed as an exact reconstruction of fixed-function R16 filtering arithmetic",
        "physicalError": "Abs height difference * displacementM * 1000 at u_layerDisplacementScale=1 and distance fade=1; multiply by actual scale/fade for deployment",
        "cases": cases,
        "sourceSha256": {str(path.relative_to(root)).replace("\\", "/"): digest(path) for path in (
            Path(__file__), here / "composited_height_diagnostic.comp", here / "composited_bake_gpu_check.cpp",
            helper_path, here / "height_bilinear_reference.py",
            here / "composited_height_source_reference.glsl",
            root / "source/terrain/terrainHeightAtlasLayout.h",
        )},
        "evidenceSha256": {path.relative_to(output).as_posix(): digest(path)
                           for path in output.rglob("*") if path.is_file()},
    }
    (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Production double-oracle gate {report['status']}; original hardware history retained: {output / 'report.json'}")
    return 0 if accepted else 1


if __name__ == "__main__":
    raise SystemExit(main())
