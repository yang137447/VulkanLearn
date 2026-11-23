"""Numerically check production TES macro weights against CPU triangle geometry.

Runs without a renderer or generated files. Covers TES evaluation points, not
quad tessellator primitive topology or displaced shadow equivalence.
"""

import ast
from pathlib import Path
import random
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]
SOURCE = (ROOT / "shader/glsl/engine/passTemplate/base.tese.glsl").read_text(
    encoding="utf-8"
)


def scalar(expression, surface_u, surface_v):
    tree = ast.parse(expression.strip(), mode="eval")
    allowed = (
        ast.Expression, ast.BinOp, ast.Add, ast.Sub, ast.Mult, ast.Name,
        ast.Load, ast.Constant, ast.Compare, ast.LtE,
    )
    for node in ast.walk(tree):
        if not isinstance(node, allowed):
            raise ValueError(f"Unsupported GLSL expression: {expression}")
        if isinstance(node, ast.Name) and node.id not in ("u", "v"):
            raise ValueError(f"Unexpected input: {node.id}")
    return eval(
        compile(tree, "<production TES>", "eval"),
        {"__builtins__": {}}, {"u": surface_u, "v": surface_v},
    )


def production_weights(surface_u, surface_v):
    match = re.search(
        r"const vec4 macroWeights\s*=\s*(.*?)\?\s*vec4\((.*?)\)"
        r"\s*:\s*vec4\((.*?)\)\s*;", SOURCE, re.S,
    )
    if not match:
        raise AssertionError("Missing production macroWeights")
    branch = 2 if scalar(match[1], surface_u, surface_v) else 3
    return tuple(
        scalar(component, surface_u, surface_v)
        for component in match[branch].split(",")
    )


def interpolate(points, weights):
    return tuple(
        sum(point[axis] * weight for point, weight in zip(points, weights))
        for axis in range(len(points[0]))
    )


def triangle_reference(points, surface_u, surface_v):
    coordinates = ((0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0))
    indices = (0, 1, 3) if surface_u + surface_v <= 1.0 else (1, 2, 3)
    first, second, third = (coordinates[index] for index in indices)
    edge_a = (second[0] - first[0], second[1] - first[1])
    edge_b = (third[0] - first[0], third[1] - first[1])
    offset = (surface_u - first[0], surface_v - first[1])
    determinant = edge_a[0] * edge_b[1] - edge_a[1] * edge_b[0]
    second_weight = (offset[0] * edge_b[1] - offset[1] * edge_b[0]) / determinant
    third_weight = (edge_a[0] * offset[1] - edge_a[1] * offset[0]) / determinant
    return interpolate(
        tuple(points[index] for index in indices),
        (1.0 - second_weight - third_weight, second_weight, third_weight),
    )


class TessMacroSurfaceTest(unittest.TestCase):
    def assert_vector_close(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        for observed, reference in zip(actual, expected):
            self.assertAlmostEqual(observed, reference, delta=1.0e-9)

    def check_surface(self, points):
        for row in range(33):
            for column in range(33):
                surface_u, surface_v = column / 32, row / 32
                weights = production_weights(surface_u, surface_v)
                self.assertAlmostEqual(sum(weights), 1.0)
                self.assertGreaterEqual(min(weights), 0.0)
                self.assert_vector_close(
                    interpolate(points, weights),
                    triangle_reference(points, surface_u, surface_v),
                )

    def test_planar_and_nonplanar_match_cpu_triangles(self):
        self.check_surface(((0, 1, 0), (2, 5, 0), (2, 11, 3), (0, 7, 3)))
        self.check_surface(((0, -7, 0), (2, 13, 0), (2, -29, 3), (0, 41, 3)))
        generator = random.Random(73017)
        for _ in range(8):
            self.check_surface(tuple(
                (position[0], generator.uniform(-1000, 1000), position[1])
                for position in ((0, 0), (2, 0), (2, 3), (0, 3))
            ))

    def test_nonplanar_center_is_fixed_diagonal_not_bilinear(self):
        points = ((0, 0, 0), (1, 2, 0), (1, 20, 1), (0, 4, 1))
        actual = interpolate(points, production_weights(0.5, 0.5))
        self.assertEqual(actual, (0.5, 3.0, 0.5))
        self.assertNotEqual(actual, interpolate(points, (0.25,) * 4))

    def test_diagonal_and_both_sides_are_continuous(self):
        points = ((0, 7, 0), (1, -13, 0), (1, 47, 1), (0, 23, 1))
        for step in range(101):
            fraction = step / 100
            expected = interpolate((points[1], points[3]), (fraction, 1 - fraction))
            self.assert_vector_close(
                interpolate(points, production_weights(fraction, 1 - fraction)), expected,
            )
            for direction in (-1, 1):
                surface_v = 1 - fraction + direction * 1.0e-8
                if 0 <= surface_v <= 1:
                    self.assert_vector_close(
                        interpolate(points, production_weights(fraction, surface_v)),
                        triangle_reference(points, fraction, surface_v),
                    )

    def test_shared_edges_match_neighbor_and_linear_endpoints(self):
        current = ((0, 7, 0), (1, -13, 0), (1, 47, 1), (0, 23, 1))
        right = (current[1], (2, 101, 0), (2, -81, 1), current[2])
        above = (current[3], current[2], (1, 59, 2), (0, -17, 2))
        for step in range(101):
            fraction = step / 100
            self.assert_vector_close(
                interpolate(current, production_weights(1, fraction)),
                interpolate(right, production_weights(0, fraction)),
            )
            self.assert_vector_close(
                interpolate(current, production_weights(fraction, 1)),
                interpolate(above, production_weights(fraction, 0)),
            )
            for surface_u, surface_v, endpoints in (
                (fraction, 0, (0, 1)), (1, fraction, (1, 2)),
                (fraction, 1, (3, 2)), (0, fraction, (0, 3)),
            ):
                self.assert_vector_close(
                    interpolate(current, production_weights(surface_u, surface_v)),
                    interpolate(tuple(current[index] for index in endpoints),
                                (1 - fraction, fraction)),
                )

    def test_degenerate_transition_stays_in_original_triangle(self):
        self.check_surface(((0, 1, 0), (2, 17, 0), (0, -3, 4), (0, -3, 4)))

    def test_production_bindings_and_projection_linearity(self):
        for target, field in (
            ("worldPosition", "worldPosition"),
            ("previousClipPosition", "previousClipPosition"),
            ("ctx.clipPosition", "clipPosition"),
        ):
            match = re.search(rf"\b{re.escape(target)}\s*=\s*([^;]+);", SOURCE)
            self.assertIsNotNone(match)
            terms = re.findall(
                rf"v2fIn\[(\d)\]\.{field}\s*\*\s*macroWeights\.([xyzw])", match[1],
            )
            self.assertEqual(terms, list(zip("0123", "xyzw")))
        points = ((1, 2, 3), (4, -5, 6), (7, 19, 9), (10, 11, -12))
        matrix = ((2, 3, 5, 7), (11, 13, 17, 19), (23, 29, 31, 37), (1, 0, 2, 3))

        def project(point):
            return tuple(sum(component * value for component, value in zip(row, (*point, 1)))
                         for row in matrix)

        for surface_u, surface_v in ((0.2, 0.3), (0.7, 0.8), (0.5, 0.5)):
            self.assert_vector_close(
                interpolate(tuple(project(point) for point in points),
                            production_weights(surface_u, surface_v)),
                project(triangle_reference(points, surface_u, surface_v)),
            )
        for field in ("worldNormal", "texCoord", "texCoord1", "worldTangent", "vertexColor"):
            match = re.search(rf"\b{field}\s*=\s*([^;]+);", SOURCE)
            self.assertIsNotNone(match)
            self.assertNotIn("macroWeights", match[1])
        self.assertIn("TerrainBlendedLayerDisplacementMeters(ctx, ids, weights)", SOURCE)
        self.assertRegex(SOURCE, r"worldPosition\s*\+=\s*worldNormal \* layerDisplacementCm \* displacementFade")


if __name__ == "__main__":
    unittest.main(verbosity=2)
