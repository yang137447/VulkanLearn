"""Raw Vulkan fixed-eight-tap VT checks against an independent double CPU oracle."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct

from validate_composited_bake_gpu import run, material_stages


GRADIENTS = [
    ((0, 0), (0, 0)), ((2, 0), (0, 2)), ((8, 0), (0, 1)),
    ((1, 0), (0, 8)), ((5.65685425, 5.65685425), (-0.70710678, 0.70710678)),
    ((5, 0), (5, 1)), ((128, 0), (0, 0.25)), ((8, 0), (0, 1.41421356)),
    ((2, 0), (0, 1)), ((-4, 0), (0, -1)), ((0, 4), (-1, 0)),
    ((0.25, 0), (0, 0.125)),
]
EXTENT = 136
TEXELS = EXTENT * EXTENT
PLANE_BYTES = TEXELS * 4


def footprint(gradient_x, gradient_y):
    axis_x = gradient_x[0] ** 2 + gradient_y[0] ** 2
    axis_y = gradient_x[1] ** 2 + gradient_y[1] ** 2
    cross = gradient_x[0] * gradient_x[1] + gradient_y[0] * gradient_y[1]
    discriminant = math.hypot(axis_x - axis_y, 2 * cross)
    major = math.sqrt(max((axis_x + axis_y + discriminant) / 2, 1e-12))
    minor = math.sqrt(max((axis_x + axis_y - discriminant) / 2, 0))
    angle = math.atan2(2 * cross, axis_x - axis_y) / 2
    filtered = max(minor, major / 8)
    return (
        [major * math.cos(angle) / 256, major * math.sin(angle) / 128],
        min(1, max(0, math.log2(max(filtered, 1e-6)))),
        max(1, min(8, math.ceil(major / max(filtered, 1)))),
    )


def normalize(vector):
    length = math.sqrt(sum(value * value for value in vector))
    return [value / length for value in vector] if length > 1e-12 else [0, 1, 0]


def srgb(value):
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def texel(channel, mip, column, row):
    if channel == 0:
        if mip:
            return [round(sum(texel(0, 0, column * 2 + dx, row * 2 + dy)[component]
                              for dx in range(2) for dy in range(2)) / 4)
                    for component in range(3)] + [255]
        return [(column * 17 + row * 3) % 256, 255 if row % 2 else 0,
                (column * 7 + row * 11) % 256, 255]
    if channel == 1:
        return [112 + column % 32, 230 - mip * 10, 112 + row % 32, 255]
    return [160 + column % 32, 70 + row % 64, 10 + mip * 40, 255]


def sample(channel, mip, uv, repeat):
    width, height = 256 >> mip, 128 >> mip
    position = [(uv[0] % 1) * width - 0.5, (uv[1] % 1) * height - 0.5] if repeat else [
        min(width - 1, max(0, uv[0] * width - 0.5)),
        min(height - 1, max(0, uv[1] * height - 0.5))]
    base = [math.floor(value) for value in position]
    fraction = [position[axis] - base[axis] for axis in range(2)]
    result = [0.0] * 4
    for row in range(2):
        for column in range(2):
            weight = (fraction[0] if column else 1 - fraction[0]) * (
                fraction[1] if row else 1 - fraction[1])
            value = texel(channel, mip, (base[0] + column) % width, (base[1] + row) % height)
            for component in range(4):
                decoded = value[component] / 255
                if channel == 0 and component < 3:
                    decoded = srgb(decoded)
                result[component] += decoded * weight
    return result


def reference(uv, major_uv, target, taps, repeat):
    colours, normals, orms = [], [], []
    for tap in range(taps):
        tap_uv = [uv[axis] + major_uv[axis] * ((tap + 0.5) / taps - 0.5) for axis in range(2)]
        low, high = math.floor(target), math.ceil(target)
        blend = target - low
        channels = []
        for channel in range(3):
            first, second = sample(channel, low, tap_uv, repeat), sample(channel, high, tap_uv, repeat)
            if channel == 1:
                first = normalize([value * 2 - 1 for value in first[:3]])
                second = normalize([value * 2 - 1 for value in second[:3]])
            mixed = [left * (1 - blend) + right * blend for left, right in zip(first, second)]
            channels.append(normalize(mixed) if channel == 1 else mixed)
        colours.append(channels[0])
        normals.append(channels[1])
        orms.append(channels[2])
    mean = lambda values: [sum(value[axis] for value in values) / taps for axis in range(len(values[0]))]
    normal = normalize(mean(normals))
    return [mean(colours), [value * 0.5 + 0.5 for value in normal] + [1], mean(orms)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    here = Path(__file__).resolve().parent
    output = options.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    for channel, name in enumerate(("albedo", "normal", "orm")):
        data = bytearray()
        for mip, page_x in ((0, 1), (0, 0), (1, 0)):
            width, height = 256 >> mip, 128 >> mip
            for row in range(136):
                for column in range(136):
                    data.extend(texel(channel, mip, (page_x * 128 + column - 4) % width,
                                      (row - 4) % height))
        (output / f"{name}.bin").write_bytes(data)
    (output / "height-source.bin").write_bytes(bytes((1024 ** 2 + 512 ** 2 + 256 ** 2) * 2 * 2))
    (output / "height-atlas.bin").write_bytes(bytes(408 * 408 * 2))
    executable = output / "raw-check.exe"
    run([shutil.which("g++"), "-std=gnu++17", "-O2", "-Iextern/VulkanSDK/include",
         here / "composited_bake_gpu_check.cpp", "-Lextern/VulkanSDK/lib/Win32",
         "-lvulkan-1", "-o", executable], root)
    shader = output / "anisotropic.spv"
    validator = shutil.which("glslangValidator")
    run([validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl",
         here / "composited_anisotropic.comp", "-o", shader], root)
    raw = output / "anisotropic.bin"
    device = run([executable, shader, "bounded-compare", raw, output], root)
    data = raw.read_bytes()
    maximum = [0, 0, 0]
    footprint_errors = 0
    feedback_errors = 0
    invalid = 0
    changed = 0
    maximum_lod_error = 0
    maximum_axis_error = 0
    maximum_normal_angle = 0
    for index in range(TEXELS):
        column, row = index % 136, index // 136
        major_uv, target, taps = footprint(*GRADIENTS[row % 12])
        actual = [struct.unpack_from("<f", data, plane * PLANE_BYTES + index * 4)[0]
                  for plane in range(3, 9)]
        maximum_lod_error = max(maximum_lod_error, abs(actual[0] - target))
        axis_error = min(
            max(abs(actual[2 + axis] - sign * major_uv[axis]) for axis in range(2))
            for sign in (-1, 1))
        maximum_axis_error = max(maximum_axis_error, axis_error)
        footprint_errors += actual[1] != taps or abs(actual[0] - target) > 1e-5 or axis_error > 1e-5
        feedback_taps = struct.unpack_from("<f", data, 10 * PLANE_BYTES + index * 4)[0]
        feedback_errors += abs(actual[4] - target) > 1e-5 or feedback_taps != taps
        invalid += actual[5] != 1
        uv = [0.5 + (column - 67.5) / 32768, 0.37]
        if column % 3:
            uv[0] = (column - 67.5) / 32768 + (1 if column % 3 == 2 else 0)
        expected = reference(uv, major_uv, target, taps, row >= 68)
        for channel in range(3):
            for component in range(3):
                expected_byte = max(0, min(255, round(expected[channel][component] * 255)))
                actual_byte = data[channel * PLANE_BYTES + index * 4 + component]
                maximum[channel] = max(maximum[channel], abs(expected_byte - actual_byte))
        expected_normal = normalize([value * 2 - 1 for value in expected[1][:3]])
        actual_normal = normalize([data[PLANE_BYTES + index * 4 + component] / 127.5 - 1
                                   for component in range(3)])
        maximum_normal_angle = max(maximum_normal_angle, math.degrees(math.acos(
            max(-1, min(1, sum(left * right for left, right in zip(expected_normal, actual_normal)))))))
        changed += data[index * 4:index * 4 + 3] != data[9 * PLANE_BYTES + index * 4:9 * PLANE_BYTES + index * 4 + 3]
    material_stages(root, output, validator)
    feedback_source = (root / "shader/glsl/pass/terrainFeedback.frag").read_text(encoding="utf-8")
    feedback_schema = json.loads((root / "shader/glsl/pass/M_terrainFeedback.json").read_text(encoding="utf-8"))
    declarations = "layout(std140,set=1,binding=0) uniform MaterialParams {\n" + "\n".join(
        f'{descriptor["type"]} {name};' for name, descriptor in feedback_schema["parameters"].items()) + "\n};"
    feedback_source = feedback_source.replace('#version 450', '#version 450\n#extension GL_GOOGLE_include_directive : require')
    feedback_source = feedback_source.replace('#include "generate/M_terrainFeedbackParamter.glsl"', declarations)
    feedback_source = feedback_source.replace('"../common/', '"common/').replace('"../materialFunction/', '"materialFunction/')
    feedback_path = output / "terrainFeedback.frag"
    feedback_path.write_text(feedback_source, encoding="utf-8")
    run([validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl", feedback_path,
         "-o", output / "feedback.spv"], root)
    negative = output / "negative-page-swap"
    negative.mkdir()
    for name in ("normal", "orm", "height-source", "height-atlas"):
        shutil.copyfile(output / f"{name}.bin", negative / f"{name}.bin")
    original = (output / "albedo.bin").read_bytes()
    (negative / "albedo.bin").write_bytes(
        original[PLANE_BYTES:2 * PLANE_BYTES] + original[:PLANE_BYTES] + original[2 * PLANE_BYTES:])
    negative_raw = negative / "anisotropic.bin"
    run([executable, shader, "bounded-compare", negative_raw, negative], root)
    negative_bytes = negative_raw.read_bytes()
    negative_difference = max(abs(data[index] - negative_bytes[index]) for index in range(PLANE_BYTES))
    sources = [root / "shader/glsl/materialFunction" / name for name in (
        "terrainCompositedFootprint.glsl", "mf_terrainCompositedVT.glsl",
        "terrainCompositedFeedback.glsl", "mf_terrainLayerBlend.glsl")]
    sources += [root / "shader/glsl/pass/terrainFeedback.frag", Path(__file__),
                here / "composited_anisotropic.comp", here / "composited_bake_gpu_check.cpp"]
    report = {
        "status": "passed" if max(maximum) <= 2 and not footprint_errors and not feedback_errors
                  and not invalid and changed and maximum_normal_angle <= 1 and negative_difference > 2 else "failed",
        "device": device, "texels": TEXELS, "fixedMaximumTaps": 8,
        "maxChannelByteErrors": maximum, "footprintErrors": footprint_errors,
        "feedbackErrors": feedback_errors, "invalidSamples": invalid,
        "maxLodError": maximum_lod_error, "maxMajorUvError": maximum_axis_error,
        "maxNormalAngleDegrees": maximum_normal_angle,
        "negativePageSwapMaxByteDifference": negative_difference,
        "fullMaterialStagesAndFeedbackCompilation": "passed; declarations from current JSON schemas",
        "changedFromLegacyIsotropicTexels": changed, "channelByteThreshold": 2,
        "cases": "12 gradients: zero, isotropic, X/Y major, rotated, shear, high ratio, fractional LOD, signed axes, magnification; three UV seams; clamp/repeat; feedback 1920x1080/128x128",
        "limits": "Raw GPU synthetic atlas and independent double CPU oracle; no renderer/Cook, no screen-space S1 acceptance",
        "sourceSha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in sources},
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
