"""Evaluate the actual fragment mip expressions and shared wire helper at float32 boundaries."""

import math
from pathlib import Path
import re
import struct
import unittest

from validate_composited_feedback import Vector as BaseVector, evaluate, function_body, rhs


class Vector(BaseVector):
    def __sub__(self, other):
        if not isinstance(other, tuple):
            other = (other,) * len(self)
        return Vector(first - second for first, second in zip(self, other))

ROOT = Path(__file__).resolve().parents[3]
FRAGMENT = ROOT / "shader/glsl/pass/terrainFeedback.frag"
HELPER = ROOT / "shader/glsl/materialFunction/terrainCompositedFeedback.glsl"


def expressions():
    source = FRAGMENT.read_text("utf-8")
    result = []
    for slot in (0, 2):
        match = re.search(
            rf"outLayerSlot{slot}\s*=\s*compositedValid\s*\?\s*"
            r"TerrainEncodeCompositedFeedbackAddress\(\s*inMaterialUv,\s*"
            r"compositedResolution,\s*compositedSurfaceIndex,\s*(.*?)\)\s*:\s*vec4",
            source, re.S)
        if not match:
            raise AssertionError("Production feedback address call changed")
        result.append(match.group(1).strip())
    return result


def neighbor(value, direction):
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    return struct.unpack("<f", struct.pack("<I", bits + direction))[0]


def cases():
    return [(0.0, 13, (0, 1)), (neighbor(0.0, 1), 13, (0, 1)),
            (neighbor(3.0, -1), 13, (2, 3)), (3.0, 13, (3, 4)),
            (neighbor(3.0, 1), 13, (3, 4)), (3.25, 13, (3, 4)),
            (neighbor(13.0, -1), 13, (12, 13)), (13.0, 13, (13, 13)),
            (0.0, 0, (0, 0))]


def production_pair(target, maximum):
    values = {"compositedMip": target, "maximum": maximum,
              "floor": math.floor, "int": int, "min": min}
    return tuple(evaluate(expression.replace("compositedSurface.a", "maximum"), values)
                 for expression in expressions())


def production_address(mip, uv, surface=3):
    source = HELPER.read_text("utf-8")
    def componentwise(operation, *values):
        return Vector(operation(*(value[index] if isinstance(value, tuple) else value
                                  for value in values)) for index in range(2))
    values = {"materialUv": Vector(uv), "resolution": Vector((8192.0, 8192.0)),
              "mip": mip, "surfaceIndex": surface, "float": float, "uint": int,
              "vec2": lambda value: Vector((value, value)) if not isinstance(value, tuple) else value,
              "uvec2": lambda value: Vector(int(item) for item in value),
              "vec4": lambda *value: Vector(value),
              "floor": lambda value: componentwise(math.floor, value) if isinstance(value, tuple) else math.floor(value),
              "exp2": lambda value: 2.0 ** value,
              "max": lambda left, right: componentwise(max, left, right),
              "clamp": lambda value, low, high: componentwise(
                  lambda item, first, last: min(max(item, first), last), value, low, high)}
    for name in ("extent", "texel", "page", "packedMip"):
        values[name] = evaluate(rhs(source, name), values)
    returned = re.search(r"return\s+([^;]+);", function_body(
        source, "TerrainEncodeCompositedFeedbackAddress")).group(1)
    return tuple(round(item * 255) for item in evaluate(returned, values))


class FeedbackMipPair(unittest.TestCase):
    def test_real_feedback_matches_consumer_pair(self):
        consumer = (ROOT / "shader/glsl/materialFunction/mf_terrainCompositedVT.glsl").read_text("utf-8")
        self.assertIn("int highMip = min(lowMip + 1, int(maxMip));", consumer)
        for target, maximum, expected in cases():
            with self.subTest(target=target, maximum=maximum):
                self.assertEqual(production_pair(target, maximum), expected)

    def test_actual_shared_address_bytes_for_both_requests(self):
        for target, maximum, expected in cases():
            for uv in ((0.0, 0.0), (0.5, 0.37), (1.0, 1.0)):
                for mip, oracle in zip(production_pair(target, maximum), expected):
                    extent = max(1, 8192 >> oracle)
                    page = tuple(math.floor(min(max(item * extent, 0.5), extent - 0.5) / 128)
                                 for item in uv)
                    self.assertEqual(production_address(mip, uv), (3, *page, oracle | 128))

    def test_fractional_requests_unchanged(self):
        for target in (0.25, 2.999, 3.001, 3.5, 12.999):
            self.assertEqual(production_pair(target, 13), (math.floor(target), math.ceil(target)))

    def test_old_ceil_regression_is_detected(self):
        self.assertNotEqual((0, math.ceil(0.0)), production_pair(0.0, 13))
        self.assertNotEqual((3, math.ceil(3.0)), production_pair(3.0, 13))


if __name__ == "__main__":
    unittest.main()
