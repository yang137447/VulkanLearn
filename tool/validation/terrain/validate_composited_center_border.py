"""Real missing-neighbor page-table uploads; production border sampling vs CPU oracle."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct

from validate_composited_anisotropic import footprint, normalize, srgb, texel
from validate_composited_bake_gpu import material_stages, run


TEXELS = 136 * 136
PLANE_BYTES = TEXELS * 4
GRADIENTS = [
    ((8, 0), (0, 1)), ((1, 0), (0, 8)),
    ((5.65685425, 5.65685425), (-0.70710678, 0.70710678)), ((5, 0), (5, 1)),
    ((7.9, 0), (0, 1.99)), ((1.99, 0), (0, 7.9)),
    ((5.58614357, 5.58614357), (-1.40714249, 1.40714249)), ((0, 0), (0, 0)),
    ((2, 0), (0, 1)), ((5.5, 0), (0, 1.41421356)),
    ((128, 0), (0, 0.25)), ((128, 0), (0, 0.25)),
    ((0.25, 0), (0, 0.125)), ((-4, 0), (0, -1)),
    ((7.5, 0), (0, 1)), ((7.5, 0), (0, 1)),
]


def addressed(uv, extent, repeat):
    return [value % 1 * extent if repeat else min(extent - 0.5, max(0.5, value * extent))
            for value in uv]


def resident(page, mip, mask):
    return bool(mask & (16 if mip else 1 << (page[1] * 2 + page[0])))


def resolve(center, tap, requested, repeat, mask):
    for mip in range(requested, 2):
        extent = 256 >> mip
        center_texel = addressed(center, extent, repeat)
        center_page = [math.floor(value / 128) for value in center_texel]
        if repeat:
            delta = [tap[axis] - center[axis] for axis in range(2)]
            tap_texel = [center_texel[axis] + (delta[axis] - round(delta[axis])) * extent
                         for axis in range(2)]
        else:
            tap_texel = addressed(tap, extent, False)
        local = [tap_texel[axis] - center_page[axis] * 128 for axis in range(2)]
        if all(-3.5 <= value <= 131.5 for value in local) and resident(center_page, mip, mask):
            return mip, "center-border"
        tap_page = [math.floor(value / 128) for value in addressed(tap, extent, repeat)]
        if resident(tap_page, mip, mask):
            return mip, "independent"
    return None, "missing"


def sample(channel, mip, uv, repeat):
    extent = 256 >> mip
    position = [value - 0.5 for value in addressed(uv, extent, repeat)]
    base = [math.floor(value) for value in position]
    fraction = [value - base[axis] for axis, value in enumerate(position)]
    result = [0.0] * 4
    for row in range(2):
        for column in range(2):
            weight = (fraction[0] if column else 1 - fraction[0]) * (
                fraction[1] if row else 1 - fraction[1])
            values = texel(channel, mip, (base[0] + column) % extent, (base[1] + row) % extent)
            for component, value in enumerate(values):
                decoded = value / 255
                result[component] += (srgb(decoded) if channel == 0 and component < 3 else decoded) * weight
    return result


def reference(uv, major_uv, target, taps, repeat, mask):
    sums = [[0.0] * 4, [0.0] * 3, [0.0] * 4]
    actual_mips = [0, 0]
    valid = True
    routes = {"center-border": 0, "independent": 0, "missing": 0}
    for tap in range(taps):
        tap_uv = [uv[axis] + major_uv[axis] * ((tap + 0.5) / taps - 0.5) for axis in range(2)]
        levels = []
        for side, requested in enumerate((math.floor(target), min(math.floor(target) + 1, 1))):
            actual, route = resolve(uv, tap_uv, requested, repeat, mask)
            routes[route] += 1
            levels.append(actual)
            actual_mips[side] = max(actual_mips[side], 1 if actual is None else actual)
        if None in levels:
            valid = False
            continue
        for channel in range(3):
            first, second = [sample(channel, mip, tap_uv, repeat) for mip in levels]
            if channel == 1:
                first = normalize([value * 2 - 1 for value in first[:3]])
                second = normalize([value * 2 - 1 for value in second[:3]])
            blend = target - math.floor(target)
            mixed = [left * (1 - blend) + right * blend for left, right in zip(first, second)]
            if channel == 1:
                mixed = normalize(mixed)
            sums[channel] = [left + right / taps for left, right in zip(sums[channel], mixed)]
    sums[1] = [value * 0.5 + 0.5 for value in normalize(sums[1])] + [1]
    return sums, actual_mips, valid, routes


def measure(data, mask, baseline=None):
    maximum = [0, 0, 0]
    route_errors = invalid_errors = footprint_errors = 0
    maximum_angle = 0
    routes = {"center-border": 0, "independent": 0, "missing": 0}
    center_only_samples = center_only_difference = 0
    for index in range(TEXELS):
        row = index // 136
        scenario = row % 16
        major_uv, target, taps = footprint(*GRADIENTS[scenario])
        major_uv[1] *= 0.5
        if scenario == 11:
            target = 0
        metadata = [struct.unpack_from("<f", data, plane * PLANE_BYTES + index * 4)[0]
                    for plane in range(3, 10)]
        footprint_errors += abs(metadata[3] - target) > 1e-5 or metadata[4] != taps
        uv = metadata[5:7]
        expected, mips, valid, sample_routes = reference(uv, major_uv, target, taps, row >= 68, mask)
        for route, count in sample_routes.items():
            routes[route] += count
        route_errors += metadata[:2] != mips
        invalid_errors += bool(metadata[2]) != valid
        if not valid:
            continue
        for channel in range(3):
            for component in range(3):
                expected_byte = max(0, min(255, round(expected[channel][component] * 255)))
                actual_byte = data[channel * PLANE_BYTES + index * 4 + component]
                maximum[channel] = max(maximum[channel], abs(expected_byte - actual_byte))
        decoded = normalize([data[PLANE_BYTES + index * 4 + component] / 127.5 - 1 for component in range(3)])
        normal = normalize([value * 2 - 1 for value in expected[1][:3]])
        maximum_angle = max(maximum_angle, math.degrees(math.acos(max(-1, min(1,
            sum(left * right for left, right in zip(decoded, normal)))))))
        center_page = [math.floor(value / 128) for value in addressed(uv, 256, row >= 68)]
        if baseline is not None and resident(center_page, 0, mask) and scenario not in (10, 11):
            center_only_samples += 1
            for channel in range(3):
                for component in range(3):
                    offset = channel * PLANE_BYTES + index * 4 + component
                    center_only_difference = max(center_only_difference, abs(data[offset] - baseline[offset]))
            assert mips == [math.floor(target), min(math.floor(target) + 1, 1)]
    return {
        "passed": max(maximum) <= 2 and maximum_angle <= 1 and route_errors == invalid_errors == footprint_errors == 0
                  and center_only_difference <= 1,
        "maxChannelByteErrors": maximum, "maxNormalAngleDegrees": maximum_angle,
        "actualMipErrors": route_errors, "validityErrors": invalid_errors, "footprintErrors": footprint_errors,
        "oracleAddressRoutes": routes, "centerResidentSamples": center_only_samples,
        "centerResidentVsAllResidentMaxByteDifference": center_only_difference,
    }


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
        for mip, page_x, page_y in ((0, 1, 0), (0, 0, 0), (0, 1, 1), (0, 0, 1), (1, 0, 0)):
            extent = 256 >> mip
            for row in range(136):
                for column in range(136):
                    data.extend(texel(channel, mip, (page_x * 128 + column - 4) % extent,
                                      (page_y * 128 + row - 4) % extent))
        (output / f"{name}.bin").write_bytes(data)
    (output / "height-source.bin").write_bytes(bytes((1024 ** 2 + 512 ** 2 + 256 ** 2) * 4))
    (output / "height-atlas.bin").write_bytes(bytes(408 * 408 * 2))
    harness = (here / "composited_bake_gpu_check.cpp").read_text(encoding="utf-8")
    old = "const std::array<float,4> entries{2,1,3,0};"
    assert harness.count(old) == 1
    harness = harness.replace(old, """
        const uint32_t mask=argc>5?uint32_t(std::stoul(argv[5])):31;
        const std::array<float,8> entries{
            mask&1?2.f:0.f,mask&2?1.f:0.f,mask&4?4.f:0.f,mask&8?3.f:0.f,
            mask&16?5.f:0.f,0,0,0};""")
    for before, after in (
        ("makeImage(2,1,2,vk::Format::eR32Sfloat,entries", "makeImage(2,2,2,vk::Format::eR32Sfloat,entries"),
        ("data.size()!=3*channelBytes", "data.size()!=5*channelBytes"),
        ("makeImage(136,136,3,channel", "makeImage(136,136,5,channel"),
    ):
        assert harness.count(before) == 1
        harness = harness.replace(before, after)
    generated = output / "center-border-check.cpp"
    generated.write_text(harness, encoding="utf-8")
    executable = output / "center-border-check.exe"
    run([shutil.which("g++"), "-std=gnu++17", "-O2", "-Iextern/VulkanSDK/include",
         generated, "-Lextern/VulkanSDK/lib/Win32", "-lvulkan-1", "-o", executable], root)
    validator = shutil.which("glslangValidator")
    shader_source = (here / "composited_center_border.comp").read_text(encoding="utf-8")
    shader = output / "center-border.spv"
    run([validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl",
         here / "composited_center_border.comp", "-o", shader], root)
    measurements = {}
    baseline = None
    device = ""
    for mask in (31, 17, 18, 20, 24, 16, 1, 0):
        raw = output / f"mask-{mask}.bin"
        device = run([executable, shader, "bounded-compare", raw, output, mask], root)
        data = raw.read_bytes()
        measurements[str(mask)] = measure(data, mask, baseline)
        if mask == 31:
            baseline = data
    helper_path = root / "shader/glsl/materialFunction/mf_terrainCompositedVT.glsl"
    helper = helper_path.read_text(encoding="utf-8")
    old_lookup = helper.replace(
        "TerrainCompositedAddressAnchored(pageTable, centerUv, localUv, resolution,",
        "TerrainCompositedAddress(pageTable, localUv, resolution,")
    assert helper != old_lookup
    (output / "old-lookup.glsl").write_text(old_lookup.replace(
        '#include "terrainCompositedFootprint.glsl"', '#include "materialFunction/terrainCompositedFootprint.glsl"'),
        encoding="utf-8")
    negative_shader = output / "old-lookup.comp"
    negative_shader.write_text(shader_source.replace(
        '"materialFunction/mf_terrainCompositedVT.glsl"', '"old-lookup.glsl"'), encoding="utf-8")
    negative_spv = output / "old-lookup.spv"
    run([validator, "-V", "--target-env", "vulkan1.2", "-Ishader/glsl", negative_shader, "-o", negative_spv], root)
    negative_raw = output / "old-lookup-mask-17.bin"
    run([executable, negative_spv, "bounded-compare", negative_raw, output, 17], root)
    negative = measure(negative_raw.read_bytes(), 17)
    material_stages(root, output, validator)
    sources = [helper_path, root / "shader/glsl/materialFunction/terrainCompositedFootprint.glsl",
               here / "composited_center_border.comp", Path(__file__), here / "composited_bake_gpu_check.cpp"]
    report = {
        "status": "passed" if all(result["passed"] for result in measurements.values()) and not negative["passed"] else "failed",
        "device": device, "samplesPerDispatch": TEXELS, "dispatches": 9,
        "pageTable": "256x256 virtual; 2x2 fine entries and one 128x128 coarse entry; actual zero entries uploaded by generated private harness",
        "masks": "bits0..3 fine pages, bit4 coarse; 17/18/20/24 each retains only one center fine page plus coarse",
        "measurements": measurements, "negativeLegacyIndependentLookup": negative,
        "thresholds": {"channelBytes": 2, "normalAngleDegrees": 1, "residentVsAllResidentBytes": 1},
        "limits": "Raw synthetic GPU/independent CPU oracle; no renderer, Cook, capacity or producer changes",
        "sourceSha256": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
    }
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
