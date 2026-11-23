"""Check actual GLSL feedback expressions against independent mip/wire oracles.

Uses only Python's standard library; does not write generated shader files.
"""

import ast
import math
from pathlib import Path
import re
from types import SimpleNamespace


class Vector(tuple):
    @property
    def x(self):
        return self[0]

    @property
    def y(self):
        return self[1]

    def __add__(self, other):
        return Vector(first + second for first, second in zip(self, other))

    def __sub__(self, other):
        return Vector(first - second for first, second in zip(self, other))

    def __mul__(self, scalar):
        if isinstance(scalar, tuple):
            return Vector(component * other for component, other in zip(self, scalar))
        return Vector(component * scalar for component in self)

    def __truediv__(self, scalar):
        if isinstance(scalar, tuple):
            return Vector(component / other for component, other in zip(self, scalar))
        return Vector(component / scalar for component in self)


def evaluate(expression, values):
    expression = re.sub(r"(\d+)u\b", r"\1", expression.strip())
    expression = " ".join(expression.split())
    if "?" in expression:
        condition, alternatives = expression.split("?", 1)
        first, second = alternatives.split(":", 1)
        expression = f"({first.strip()}) if ({condition.strip()}) else ({second.strip()})"
    tree = ast.parse(expression, mode="eval")
    allowed = (
        ast.Expression, ast.BinOp, ast.Mult, ast.Div, ast.Add, ast.Sub,
        ast.LShift, ast.RShift, ast.BitAnd, ast.BitOr,
        ast.Name, ast.Load, ast.Call, ast.Constant, ast.Attribute,
        ast.UnaryOp, ast.USub, ast.UAdd, ast.Compare, ast.Gt, ast.Lt,
        ast.GtE, ast.LtE, ast.Eq, ast.NotEq, ast.IfExp,
    )
    for node in ast.walk(tree):
        assert isinstance(node, allowed), f"Unsupported GLSL expression: {expression}"
        if isinstance(node, ast.Name):
            assert node.id in values, f"Unknown GLSL input: {node.id}"
        if isinstance(node, ast.Attribute):
            assert isinstance(node.value, ast.Name) and node.attr in (
                "x", "y", "majorUv", "targetMip", "tapCount")
        if isinstance(node, ast.Call):
            assert isinstance(node.func, ast.Name) and callable(values[node.func.id])
    return eval(compile(tree, "<feedback GLSL>", "eval"), {"__builtins__": {}}, values)


def rhs(source, name):
    match = re.search(rf"\b{re.escape(name)}\s*=\s*([^;]+);", source)
    assert match, f"Missing GLSL assignment: {name}"
    return match.group(1)


def function_body(source, name):
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", source)
    assert match, f"Missing GLSL function: {name}"
    begin = match.end()
    depth = 1
    for index in range(begin, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[begin:index]
    raise AssertionError(f"Unclosed GLSL function: {name}")


def vector_max(first, second):
    if isinstance(first, tuple):
        if not isinstance(second, tuple):
            second = (second,) * len(first)
        return Vector(max(left, right) for left, right in zip(first, second))
    return max(first, second)


def vec2(*values):
    if len(values) == 1:
        return Vector(values[0] if isinstance(values[0], tuple) else (values[0], values[0]))
    return Vector(values)


def environment():
    return {
        "vec2": vec2, "vec4": lambda *values: Vector(values),
        "max": vector_max, "min": min, "float": float, "int": int,
        "sqrt": math.sqrt, "abs": abs, "log2": math.log2, "ceil": math.ceil,
        "length": lambda vector: math.sqrt(sum(value * value for value in vector)),
        "dot": lambda first, second: sum(left * right for left, right in zip(first, second)),
        "normalize": lambda vector: vector / math.sqrt(sum(value * value for value in vector)),
        "clamp": lambda value, low, high: min(max(value, low), high),
    }


def execute(source, name, inputs):
    body = function_body(source, name)
    assert not re.search(r"\b(if|for|while|switch)\b", body), f"Review control flow in {name}"
    values = environment()
    values.update(inputs)
    for statement in body.split(";"):
        statement = statement.strip()
        if not statement:
            continue
        if statement.startswith("return "):
            return evaluate(statement[len("return "):], values)
        assignment = re.fullmatch(r"(?:(?:const\s+)?\w+\s+)?([\w.]+)\s*=\s*(.+)",
                                  statement, re.S)
        if assignment:
            target, expression = assignment.groups()
            value = evaluate(expression, values)
            if "." in target:
                owner, attribute = target.split(".")
                assert attribute in ("majorUv", "targetMip", "tapCount")
                setattr(values[owner], attribute, value)
            else:
                values[target] = value
        else:
            declaration = re.fullmatch(r"TerrainCompositedFootprint\s+(\w+)", statement)
            assert declaration, f"Unsupported GLSL statement: {statement}"
            values[declaration.group(1)] = SimpleNamespace()
    raise AssertionError(f"No return in {name}")


def independent_footprint(delta_x, delta_y, max_mip):
    axis_x = delta_x[0] ** 2 + delta_y[0] ** 2
    axis_y = delta_x[1] ** 2 + delta_y[1] ** 2
    cross = delta_x[0] * delta_x[1] + delta_y[0] * delta_y[1]
    angle = math.atan2(2 * cross, axis_x - axis_y) / 2
    direction = Vector((math.cos(angle), math.sin(angle)))
    projection = lambda vector, axis: sum(left * right for left, right in zip(vector, axis))
    major = math.hypot(projection(delta_x, direction), projection(delta_y, direction))
    perpendicular = Vector((-direction[1], direction[0]))
    minor = math.hypot(projection(delta_x, perpendicular), projection(delta_y, perpendicular))
    filtered_minor = max(minor, major / 8)
    target = min(max(math.log2(max(filtered_minor, 1e-6)), 0), max_mip)
    taps = min(max(math.ceil(major / max(filtered_minor, 1) - 1e-12), 1), 8)
    return direction * major, target, taps


def validate_shared_footprint(root):
    directory = root / "shader/glsl/materialFunction"
    strip = lambda path: re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)
    shared = strip(directory / "terrainCompositedFootprint.glsl")
    feedback = strip(directory / "terrainCompositedFeedback.glsl")
    sampling = strip(directory / "mf_terrainCompositedVT.glsl")
    maximum_taps = int(re.search(r"const int TerrainCompositedMaxTaps\s*=\s*(\d+);", shared).group(1))
    assert maximum_taps == 8

    def production(uv_dx, uv_dy, resolution, max_mip, shared_source=shared):
        return execute(shared_source, "TerrainCompositedFootprintFromGradients", {
            "uvDx": uv_dx, "uvDy": uv_dy, "resolution": resolution,
            "maxMip": max_mip, "TerrainCompositedMaxTaps": maximum_taps})

    cases = []
    for lod in (-8, 0, 0.25, 1, 3.75, 6, 8, 12.5, 13, 16):
        density = 2 ** lod
        for horizontal, vertical in ((1, 1), (1, 0.1), (0.1, 1)):
            cases.append((Vector((density * horizontal, 0)), Vector((0, density * vertical)),
                          "isotropic" if horizontal == vertical else "legacy-axis"))
    for major, minor in ((0, 0), (0.25, 0.125), (1, 1), (2, 1), (3, 1), (4, 1),
                         (5, 1), (6, 1), (7, 1), (8, 1), (7.5, 1),
                         (32, 0), (128, 0.25), (8, math.sqrt(2)), (3.7, 1.1)):
        for angle in (0, math.pi / 4, 1.123):
            direction = Vector((math.cos(angle), math.sin(angle)))
            perpendicular = Vector((-direction.y, direction.x))
            rotated_major = major + 0.125 if angle and major in range(2, 9) else major
            rotated_minor = minor * 0.87 if angle and major == minor else minor
            cases.append((direction * rotated_major, perpendicular * rotated_minor, "rotated"))
    for first, second in (((3, 0), (3, 1)), ((-3, 2), (1, 4)), ((1, -5), (2, -3)),
                          ((0, 0), (0, 4)), ((1e-8, 2e-8), (3e-8, 0))):
        cases.append((Vector(first), Vector(second), "shear"))
    checked = isotropic = tap_positions = tap_roundoff_boundaries = 0
    seen_taps = set()
    for width in (128, 1024, 8192):
        for height in (128, 1024, 8192):
            resolution = Vector((width, height))
            for max_mip in (0, 1, math.log2(max(width, height))):
                for delta_x, delta_y, kind in cases:
                    uv_dx = Vector(value / resolution[axis] for axis, value in enumerate(delta_x))
                    uv_dy = Vector(value / resolution[axis] for axis, value in enumerate(delta_y))
                    actual = production(uv_dx, uv_dy, resolution, max_mip)
                    major_axis, target, taps = independent_footprint(delta_x, delta_y, max_mip)
                    assert math.isclose(actual.targetMip, target, abs_tol=1e-9), (kind, actual, target)
                    assert actual.tapCount == taps, (kind, actual.tapCount, taps, delta_x, delta_y)
                    assert 1 <= actual.tapCount <= 8 and 0 <= actual.targetMip <= max_mip
                    seen_taps.add(actual.tapCount)
                    major_uv = Vector(value / resolution[axis] for axis, value in enumerate(major_axis))
                    axis_error = min(max(abs(left - sign * right) for left, right in zip(actual.majorUv, major_uv))
                                     for sign in (-1, 1))
                    assert axis_error <= max(1e-8, max(abs(value) for value in major_uv) * 1e-10)
                    if kind == "isotropic":
                        old_lod = min(max(math.log2(max(math.hypot(*delta_x), math.hypot(*delta_y))), 0), max_mip)
                        assert math.isclose(actual.targetMip, old_lod, abs_tol=1e-9)
                        assert actual.tapCount == 1
                        isotropic += 1
                    offsets = []
                    for tap in range(actual.tapCount):
                        local_uv = Vector((0.5, 0.37))
                        position = execute(shared, "TerrainCompositedTapUv", {
                            "localUv": local_uv, "footprint": actual, "tap": tap})
                        expected = local_uv + actual.majorUv * ((tap + 0.5) / actual.tapCount - 0.5)
                        assert max(abs(left - right) for left, right in zip(position, expected)) <= 1e-12
                        offsets.append(position - local_uv)
                        tap_positions += 1
                    assert all(abs(sum(offset[axis] for offset in offsets)) < 1e-10 for axis in (0, 1))
                    for scale_x, scale_y in ((1, 1), (10, 5.625), (15, 8.4375)):
                        inputs = {"uvDx": uv_dx * scale_x, "uvDy": uv_dy * scale_y,
                                  "resolution": resolution, "maxMip": max_mip,
                                  "feedbackScale": Vector((scale_x, scale_y)),
                                  "TerrainCompositedFootprintFromGradients": production}
                        scaled = execute(feedback, "TerrainCompositedFeedbackFootprintFromGradients", inputs)
                        assert math.isclose(scaled.targetMip, actual.targetMip, abs_tol=1e-9)
                        recovered_dx = inputs["uvDx"] / scale_x
                        recovered_dy = inputs["uvDy"] / scale_y
                        direct_recovered = production(recovered_dx, recovered_dy, resolution, max_mip)
                        assert vars(scaled) == vars(direct_recovered)
                        if scaled.tapCount != actual.tapCount:
                            assert kind == "isotropic" and actual.tapCount == 1 and scaled.tapCount == 2
                            recovered_x = recovered_dx * resolution
                            recovered_y = recovered_dy * resolution
                            assert abs(recovered_x.x - recovered_y.y) <= (
                                4 * math.ulp(max(abs(recovered_x.x), abs(recovered_y.y))))
                            assert recovered_x.y == recovered_y.x == 0
                            tap_roundoff_boundaries += 1
                        assert max(abs(left - right) for left, right in zip(scaled.majorUv, actual.majorUv)) < 1e-9
                        wrapper = execute(feedback, "TerrainCompositedFeedbackMip", {
                            "materialUv": None, "resolution": resolution, "maxMip": max_mip,
                            "feedbackScale": inputs["feedbackScale"],
                            "dFdx": lambda unused: inputs["uvDx"], "dFdy": lambda unused: inputs["uvDy"],
                            "TerrainCompositedFeedbackFootprintFromGradients": lambda *args: execute(
                                feedback, "TerrainCompositedFeedbackFootprintFromGradients",
                                dict(zip(("uvDx", "uvDy", "resolution", "maxMip", "feedbackScale"), args),
                                     TerrainCompositedFootprintFromGradients=production))})
                        assert math.isclose(wrapper, actual.targetMip, abs_tol=1e-9)
                        checked += 1
    assert seen_taps == set(range(1, 9))
    sampler = function_body(sampling, "MF_TerrainCompositedVTAtFootprint")
    for requested in (-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 64):
        clamped = evaluate(rhs(sampler, "footprint.tapCount"), {
            "footprint": SimpleNamespace(tapCount=requested), "TerrainCompositedMaxTaps": maximum_taps,
            "clamp": lambda value, low, high: min(max(value, low), high)})
        assert clamped == max(1, min(requested, 8))
    assert "tap < footprint.tapCount" in sampler and "MF_TerrainCompositedVTAtMip(" in sampler
    anisotropic = production(Vector((8 / 256, 0)), Vector((0, 1 / 128)), Vector((256, 128)), 8)
    wrong_formula = shared.replace("major / float(TerrainCompositedMaxTaps)", "major")
    wrong = production(Vector((8 / 256, 0)), Vector((0, 1 / 128)), Vector((256, 128)), 8, wrong_formula)
    assert anisotropic.targetMip == 0 and wrong.targetMip == 3 and wrong.tapCount == 1
    wrong_scale = feedback.replace("uvDx / max(feedbackScale.x", "uvDx / max(feedbackScale.y")
    inputs = {"uvDx": Vector((8 * 15 / 256, 0)), "uvDy": Vector((0, 8.4375 / 128)),
              "resolution": Vector((256, 128)), "maxMip": 8, "feedbackScale": Vector((15, 8.4375)),
              "TerrainCompositedFootprintFromGradients": production}
    assert execute(wrong_scale, "TerrainCompositedFeedbackFootprintFromGradients", inputs).targetMip > 0
    print(f"PASS: {checked} production shared-footprint/feedback cases; {isotropic} unchanged isotropic LOD cases")
    print(f"PASS: {tap_positions} tap positions; zero gradients, taps 1..8, requested tap 0/overflow clamps, LOD clamps")
    print("PASS: rotated/sheared/degenerate footprints and negative isotropic-formula/swapped-feedback-scale controls")
    print(f"INFO: {tap_roundoff_boundaries} isotropic ceil(1+roundoff) feedback tap-count boundaries; target LOD remains equal")
    return checked


def main():
    root = Path(__file__).resolve().parents[3]
    source = (root / "shader/glsl/materialFunction/terrainCompositedFeedback.glsl").read_text(
        encoding="utf-8")
    source = re.sub(r"//[^\n]*", "", source)
    validate_shared_footprint(root)
    wire_cases = 0
    address_function = source.split("vec4 TerrainEncodeCompositedFeedbackAddress(", 1)[1].split("}", 1)[0]
    address_return = re.search(r"\breturn\s+([^;]+);", address_function).group(1)
    metadata_function = source.split("vec4 TerrainEncodeCompositedFeedbackMetadata(", 1)[1].split("}", 1)[0]
    metadata_return = re.search(r"\breturn\s+([^;]+);", metadata_function).group(1)
    for mip in range(14):
        for page_x in range(64):
            for page_y in range(64):
                values = {
                    "page": SimpleNamespace(x=page_x, y=page_y),
                    "mip": mip, "uint": int, "surfaceIndex": 255,
                    "float": float, "vec4": lambda *components: Vector(components),
                }
                values["packedMip"] = evaluate(rhs(source, "packedMip"), values)
                packed = evaluate(address_return, values)
                assert tuple(round(component * 255) for component in packed) == (
                    255, page_x, page_y, mip | 128)
                wire_cases += 1
    for tag in range(1, 256):
        packed = evaluate(metadata_return, {
            "frameTag": tag, "float": float, "vec4": lambda *components: Vector(components),
        })
        assert tuple(round(component * 255) for component in packed) == (tag, 1, 0, 0)
    fragment = (root / "shader/glsl/pass/terrainFeedback.frag").read_text(encoding="utf-8")
    assert "int(floor(compositedMip))" in fragment and (
        "min(int(floor(compositedMip)) + 1, int(compositedSurface.a))" in fragment)
    assert fragment.index("TerrainCompositedFeedbackFootprintFromGradients(") < fragment.index("if (miss)")
    assert "const float compositedMip = compositedFootprint.targetMip;" in fragment
    assert "u_compositedFeedbackEnabled" not in fragment
    assert "TerrainLayerMipForPageUv(" not in fragment
    branch = fragment.split("const bool compositedValid", 1)[1]
    assert "compositedSurface.r >= 1.0" in branch and "!miss" not in branch
    assert re.search(r"outLayerSlot0\s*=.*?TerrainEncodeCompositedFeedbackAddress\(.*?floor\(compositedMip\)",
                     branch, re.S)
    assert re.search(r"outLayerSlot2\s*=.*?TerrainEncodeCompositedFeedbackAddress\(.*?"
                     r"min\(int\(floor\(compositedMip\)\) \+ 1, int\(compositedSurface\.a\)\)",
                     branch, re.S)
    for slot in (1, 3):
        assert re.search(rf"outLayerSlot{slot}\s*=\s*compositedValid\s*\?\s*"
                         r"TerrainEncodeCompositedFeedbackMetadata\(frameTag\)\s*:\s*vec4\(0\.0\)", branch)
    print(f"PASS: {wire_cases} GLSL address cases, 255 metadata tags and paired floor/next-mip output guards")


if __name__ == "__main__":
    main()
