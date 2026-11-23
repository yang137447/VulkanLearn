"""Raw Vulkan S1 subset: production bake -> uploaded pages -> GPU source reference.

CPU generates bounded texture inputs and copies R16 mip2 texels into repeat-bordered
pages. Material evaluation stays on the GPU; height additionally has an independent
double-precision CPU bilinear sampling oracle.
"""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys

from validate_composited_bake_gpu import run
from height_bilinear_reference import bilinear, load_r16, validate_oracle_anchors, verify_storage


EXTENT = 136
TEXELS = EXTENT * EXTENT
CHANNEL_BYTES = TEXELS * 4


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def height_inputs(output):
    source = array("H")
    mip2 = []
    for mip, extent in enumerate((1024, 512, 256)):
        for layer in range(2):
            values = array("H", (
                (column * 173 + row * 311 + layer * 19001 + mip * 7001) % 65536
                for row in range(extent) for column in range(extent)
            ))
            source.extend(values)
            if mip == 2:
                mip2.append(values)
    (output / "height-source.bin").write_bytes(source.tobytes())
    atlas = array("H", [0]) * (408 * 408)
    for layer in range(2):
        for page_y in range(2):
            for page_x in range(2):
                slot = layer * 4 + page_y * 2 + page_x
                for row in range(136):
                    for column in range(136):
                        source_x = (page_x * 128 + column - 4) % 256
                        source_y = (page_y * 128 + row - 4) % 256
                        destination = (slot // 3 * 136 + row) * 408 + slot % 3 * 136 + column
                        atlas[destination] = mip2[layer][source_y * 256 + source_x]
    (output / "height-atlas.bin").write_bytes(atlas.tobytes())


def normal_angle(first, second):
    left = [value / 127.5 - 1 for value in first]
    right = [value / 127.5 - 1 for value in second]
    denominator = math.sqrt(sum(value * value for value in left) * sum(value * value for value in right))
    return math.degrees(math.acos(max(-1, min(1, sum(a * b for a, b in zip(left, right)) / denominator))))


def measure(path):
    data = path.read_bytes()
    if len(data) != 11 * CHANNEL_BYTES:
        raise ValueError("Incomplete eleven-plane GPU readback")
    maximum = [0, 0, 0]
    angle = 0
    sums = [0, 0, 0]
    invalid = 0
    for texel in range(TEXELS):
        offset = texel * 4
        invalid += data[offset + 3] != 255
        for channel in range(3):
            for component in range(3):
                error = abs(data[channel * CHANNEL_BYTES + offset + component] -
                            data[(channel + 3) * CHANNEL_BYTES + offset + component])
                maximum[channel] = max(maximum[channel], error)
                sums[channel] += error
        angle = max(angle, normal_angle(
            data[CHANNEL_BYTES + offset:CHANNEL_BYTES + offset + 3],
            data[4 * CHANNEL_BYTES + offset:4 * CHANNEL_BYTES + offset + 3]))
    actual_height = struct.unpack_from(f"<{TEXELS}f", data, 6 * CHANNEL_BYTES)
    manual_source = struct.unpack_from(f"<{TEXELS}f", data, 7 * CHANNEL_BYTES)
    hardware_source = struct.unpack_from(f"<{TEXELS}f", data, 8 * CHANNEL_BYTES)
    height_u = struct.unpack_from(f"<{TEXELS}f", data, 9 * CHANNEL_BYTES)
    height_v = struct.unpack_from(f"<{TEXELS}f", data, 10 * CHANNEL_BYTES)
    source_mip2 = load_r16(path.parent / "height-source.bin")[-2 * 256 * 256:]
    slices = [source_mip2[:256 * 256], source_mip2[256 * 256:]]
    source_oracle = [bilinear(slices[index % 136 % 2], 256, 256,
                              [height_u[index], height_v[index]])["exactR16"] for index in range(TEXELS)]
    if not all(math.isfinite(value) for plane in (actual_height, manual_source, hardware_source) for value in plane):
        raise ValueError("Nonfinite height readback")
    errors_r16 = [abs(actual_height[index] * 65535 - source_oracle[index]) for index in range(TEXELS)]
    height_error = max(errors_r16) / 65535
    manual_error = max(abs(actual_height[index] - manual_source[index]) * 65535 for index in range(TEXELS))
    hardware_bias = max(abs(actual_height[index] - hardware_source[index]) * 65535 for index in range(TEXELS))
    hardware_double = max(abs(hardware_source[index] * 65535 - source_oracle[index]) for index in range(TEXELS))
    storage_errors = verify_storage(source_mip2, load_r16(path.parent / "height-atlas.bin"), 408)
    height_regions = {
        label: max(errors_r16[index]
                   for index in range(TEXELS) if low <= index // 136 < high)
        for label, low, high in (("internalPageBoundary", 0, 16),
                                 ("repeatSeamSubtexel", 16, 32), ("regularGrid", 32, 136))
    }
    return {
        "texels": TEXELS, "invalidMappings": invalid,
        "maxAlbedoByteError": maximum[0], "maxNormalByteError": maximum[1],
        "maxOrmByteError": maximum[2],
        "meanAbsByteErrors": [value / (TEXELS * 3) for value in sums],
        "maxNormalAngleDegrees": angle,
        "maxHeightError": height_error,
        "maxHeightR16Units": height_error * 65535,
        "heightCriterion": "Actual production helper against independent double source bilinear, <=2 R16 units and identical atlas payload",
        "maxHeightVsManualSourceR16": manual_error,
        "legacyHardwareSourceDifferenceR16": hardware_bias,
        "legacyHardwareSourceVsDoubleR16": hardware_double,
        "legacyHardwareDifferenceIsGate": False,
        "heightAtlasInputPayloadMismatches": storage_errors,
        "synthetic1mMmPerR16Unit": 1000 / 65535,
        "synthetic1mMaxOracleErrorMm": max(errors_r16) / 65535 * 1000,
        "heightRegionsR16Units": height_regions,
        "materialPassed": invalid == 0 and max(maximum) <= 2 and angle <= 1.0,
        "heightPassed": height_error <= 2 / 65535 and manual_error <= 2 and storage_errors == 0,
        "passed": invalid == 0 and max(maximum) <= 2 and angle <= 1.0 and
                  height_error <= 2 / 65535 and manual_error <= 2 and storage_errors == 0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--validator", default=shutil.which("glslangValidator"))
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    if sys.byteorder != "little" or not options.compiler or not options.validator:
        parser.error("Little-endian host, MinGW g++ and glslangValidator required")
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
    executable = output / "bounded-check.exe"
    execute([options.compiler, "-std=gnu++17", "-O2", "-Iextern/VulkanSDK/include",
             here / "composited_bake_gpu_check.cpp", "-Lextern/VulkanSDK/lib/Win32",
             "-lvulkan-1", "-o", executable])
    bake = output / "bake.spv"
    compare = output / "source.spv"
    execute([options.validator, "-V", "--target-env", "vulkan1.2",
             root / "shader/glsl/terrain/terrainCompositedBake.comp", "-o", bake])
    production = (root / "shader/glsl/terrain/terrainCompositedBake.comp").read_text(encoding="utf-8")
    (output / "bounded-production.glsl").write_text(
        production.replace("#version 450", "", 1).replace(
            '"../materialFunction/terrainCompositedEvaluation.glsl"',
            '"materialFunction/terrainCompositedEvaluation.glsl"').replace(
            "outputPage.words[3u * TexelCount]", "outputPage.words[11u * TexelCount]"), encoding="utf-8")
    generated = output / "bounded-source.comp"
    generated.write_bytes((here / "composited_bounded_source.comp").read_bytes())
    execute([options.validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl", f"-I{here}",
             generated, "-o", compare])
    pages = {}
    for mode in ("bounded", "bounded-right", "bounded-coarse", "bounded-repeat"):
        target = output / f"{mode}.bin"
        execute([executable, bake, mode, target])
        pages[mode] = target.read_bytes()
    for channel, name in enumerate(("albedo", "normal", "orm")):
        start = channel * CHANNEL_BYTES
        (output / f"{name}.bin").write_bytes(b"".join(
            pages[mode][start:start + CHANNEL_BYTES]
            for mode in ("bounded-right", "bounded", "bounded-coarse")))
    height_inputs(output)
    readback = output / "source-comparison.bin"
    execute([executable, compare, "bounded-compare", readback, output])
    measurements = measure(readback)
    border_errors = 0
    for channel in range(3):
        for row in range(136):
            for column in range(8):
                left = channel * CHANNEL_BYTES + (row * 136 + 128 + column) * 4
                right = channel * CHANNEL_BYTES + (row * 136 + column) * 4
                border_errors += pages["bounded"][left:left + 4] != pages["bounded-right"][right:right + 4]
    repeat_changed = sum(a != b for a, b in zip(pages["bounded"], pages["bounded-repeat"]))
    negative = output / "negative-swapped-pages"
    negative.mkdir()
    for name in ("normal", "orm", "height-source", "height-atlas"):
        shutil.copyfile(output / f"{name}.bin", negative / f"{name}.bin")
    original = (output / "albedo.bin").read_bytes()
    (negative / "albedo.bin").write_bytes(
        original[CHANNEL_BYTES:2 * CHANNEL_BYTES] + original[:CHANNEL_BYTES] + original[2 * CHANNEL_BYTES:])
    negative_readback = output / "negative-swapped-pages.bin"
    execute([executable, compare, "bounded-compare", negative_readback, negative])
    negative_measurements = measure(negative_readback)
    sources = [
        here / "composited_bake_gpu_check.cpp", here / "composited_bounded_source.comp",
        Path(__file__), root / "shader/glsl/terrain/terrainCompositedBake.comp",
        root / "shader/glsl/materialFunction/terrainCompositedEvaluation.glsl",
        root / "shader/glsl/materialFunction/mf_terrainCompositedVT.glsl",
        root / "shader/glsl/materialFunction/terrainHeightAtlas.glsl",
        here / "height_bilinear_reference.py", here / "composited_height_source_reference.glsl",
    ]
    negative_rejected = negative_measurements["maxAlbedoByteError"] > 2
    head = execute(["git", "rev-parse", "HEAD"])
    helper_stable = helper_sha_before == digest(helper_path)
    report = {
        "status": "passed" if measurements["passed"] and border_errors == 0 and
                  repeat_changed > 0 and negative_rejected and helper_stable else "failed",
        "scope": "Bounded raw GPU S1 subset; not full S1 acceptance",
        "oracleAnchors": oracle_anchors,
        "productionHelperSha256": helper_sha_before,
        "productionHelperStableDuringRun": helper_stable,
        "heightFilteringSemantics": "Source mip2/tiling/amplitude unchanged; canonical full-float interpolation is more precise and not bit-equal to legacy hardware source",
        "thresholds": {"channelByteError": 2, "normalAngleDegrees": 1.0, "heightR16Units": 2},
        "measurements": measurements, "overlappingBorderTexelErrors": border_errors,
        "repeatVsClampChangedBytes": repeat_changed,
        "negativeSwappedAlbedoPages": negative_measurements,
        "negativeSwappedAlbedoRejectedByMaterialError": negative_rejected,
        "inputs": {
            "virtualMips": [[256, 128], [128, 64]], "areaSamplesPerTexel": 16,
            "sourceMaterial": "spatially varying two-layer 32x32 six authored mips; sRGB albedo, normal, ORM",
            "uvScale": 48, "layerScales": [1.3, 0.7], "rotationsRadians": [0.37, -0.81],
            "frame": "8x8 sloped varying non-unit interpolated N/T, negative tangent handedness",
            "weights": "spatial two-layer local weights, non-contiguous global IDs 13 and 207",
            "trilinear": [0, 0.25, 0.5, 0.75, 1],
            "height": "1024x1024 R16 source, fixed mip2=256x256, two layers, eight repeat-bordered pages",
            "heightAtlasExtent": [408, 408],
        },
        "sourceReference": "GPU EvaluateBase using production shared evaluator; direct area evaluation at the four texel neighbors of each virtual mip, then linear interpolation before encoding",
        "limits": [
            "Bounded authored texture inputs; no real Pegasus source pixels in raw GPU harness",
            "Shared evaluator correctness is not independently proven by comparing it with itself",
            "Only explicit AtMip path; fragment derivatives, feedback, anisotropy, BC compression untested",
            "R16 atlas input populated by CPU exact texel copies; production atlas uploader, Cook and Store not exercised",
            "Repeat bake dispatched and differs; repeat composite sampler parity not measured",
            "TES/detail spatial alignment and real renderer direct/composite images pending exclusive renderer window",
        ],
        "sourceSha256": {str(path.relative_to(root)).replace("\\", "/"): digest(path) for path in sources},
        "evidenceSha256": {path.name: digest(path) for path in output.iterdir() if path.is_file()},
        "gitHead": head,
    }
    (output / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report["measurements"], indent=2))
    print(f"Status: {report['status']}; report: {output / 'report.json'}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
