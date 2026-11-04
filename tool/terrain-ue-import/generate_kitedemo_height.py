"""生成 KiteDemo 演示用的确定性 2048² PNG16 高度图与九层权重图。

用法：
    py -3 tool/terrain-ue-import/generate_kitedemo_height.py
    py -3 tool/terrain-ue-import/generate_kitedemo_height.py --self-check

默认输出：
    <resources>/Generated/KiteDemo/Source/heightmap_kitedemo_2048.png
    <resources>/Generated/KiteDemo/Source/KiteDemoImport/Weights/w01..w09_*.png
    <resources>/Generated/KiteDemo/kitedemo_manifest.json

高度场由确定性随机网格的六层 value/fBm、非周期 Catmull-Rom 山谷路径、
旋转各向异性高斯山丘组成；每个 octave 用 Pillow BICUBIC 重采样，不使用
numpy，也不使用重复的 sin/|noise| 菱形栅格。权重图从同一高度/坡度场生成，
逐 texel 归一化到 255，并写回已有九个权重文件路径。

本脚本只更新 manifest 的高度图路径、gridResolution=2048、chunkQuads=256、
lodLevels=2 和 materialUvScale=2；其余层纹理、PBR 派生、法线翻转、距离数组
与场景设置保持不变。
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import random
import sys
import tempfile
from pathlib import Path
from typing import Sequence

from PIL import Image


DEFAULT_RESOURCE_ROOT = Path(
    r"D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources")
OUTPUT_RELATIVE = Path("Generated") / "KiteDemo"
HEIGHT_RELATIVE = Path("Source") / "heightmap_kitedemo_2048.png"
WEIGHT_RELATIVE = Path("Source") / "KiteDemoImport" / "Weights"
DEFAULT_SEED = 20261103
HEIGHT_RESOLUTION = 2048
WEIGHT_RESOLUTION = 512
HEIGHT_SIZE_CM = 20000.0
HEIGHT_SCALE_CM = 3000.0

WEIGHT_LAYERS = (
    ("Gravel A", "w01_gravel_a.png"),
    ("Gravel B", "w02_gravel_b.png"),
    ("Volcanic", "w03_volcanic.png"),
    ("Leaf Path", "w04_leaf_path.png"),
    ("Path Stones", "w05_path_stones.png"),
    ("Pebbly Riverbank", "w06_pebbly_riverbank.png"),
    ("River Pebbles", "w07_river_pebbles.png"),
    ("Rock Face", "w08_rock_face.png"),
    ("Rocky Path", "w09_rocky_path.png"),
)


class GeneratorError(RuntimeError):
    """用户可读的生成错误。"""


def _fail(message: str) -> None:
    raise GeneratorError(message)


def _atomic_save_png(path: Path, image: Image.Image) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".png", dir=path.parent)
    os.close(fd)
    temporary = Path(temporary_name)
    try:
        image.save(temporary, format="PNG", compress_level=9)
        payload = temporary.read_bytes()
        if path.is_file():
            try:
                if path.read_bytes() == payload:
                    return False
            except OSError:
                pass
        os.replace(temporary, path)
        return True
    finally:
        if temporary.exists():
            temporary.unlink()


def _atomic_write_json(path: Path, value: object) -> bool:
    payload = (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_file():
        try:
            if path.read_bytes() == payload:
                return False
        except OSError:
            pass
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        return True
    finally:
        if temporary.exists():
            temporary.unlink()


def _catmull_rom_path(
    rng: random.Random,
    knots: int,
    low: float,
    high: float,
) -> list[float]:
    control = [rng.uniform(low, high) for _ in range(knots)]
    sample_count = HEIGHT_RESOLUTION
    samples: list[float] = []
    for index in range(sample_count):
        position = index / (sample_count - 1) * (knots - 1)
        segment = min(knots - 2, int(math.floor(position)))
        t = position - segment
        p0 = control[max(0, segment - 1)]
        p1 = control[segment]
        p2 = control[segment + 1]
        p3 = control[min(knots - 1, segment + 2)]
        t2 = t * t
        t3 = t2 * t
        value = 0.5 * (
            2.0 * p1 +
            (-p0 + p2) * t +
            (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
            (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3)
        samples.append(max(low, min(high, value)))
    return samples


def _octave_field(
    seed: int,
    resolution: int,
    octaves: Sequence[tuple[int, float]],
) -> array.array:
    rng = random.Random(seed)
    total = resolution * resolution
    accumulator = array.array("f", [0.0]) * total
    weight_sum = 0.0
    for grid_size, weight in octaves:
        values = [rng.randrange(65536) for _ in range(grid_size * grid_size)]
        grid = Image.new("I;16", (grid_size, grid_size))
        grid.putdata(values)
        resampled = grid.resize(
            (resolution, resolution), Image.Resampling.BICUBIC)
        data = resampled.get_flattened_data()
        scale = weight / 65535.0
        for index, value in enumerate(data):
            accumulator[index] += int(value) * scale
        weight_sum += weight
    if weight_sum <= 0.0:
        _fail("fBm octave weights must sum to a positive value")
    inverse = 1.0 / weight_sum
    for index in range(total):
        accumulator[index] *= inverse
    return accumulator


def _generate_height(seed: int) -> tuple[array.array, dict[str, float]]:
    resolution = HEIGHT_RESOLUTION
    total = resolution * resolution
    base = _octave_field(
        seed,
        resolution,
        (
            (5, 0.34),
            (9, 0.25),
            (17, 0.17),
            (33, 0.12),
            (65, 0.08),
            (129, 0.04),
        ),
    )
    path_rng = random.Random(seed ^ 0x51A7C0DE)
    valley_center = _catmull_rom_path(path_rng, 8, 0.34, 0.68)

    hill_rng = random.Random(seed ^ 0x2F6E2B1)
    hills: list[tuple[float, float, float, float, float, float]] = []
    for _ in range(4):
        hills.append((
            hill_rng.uniform(0.10, 0.90),
            hill_rng.uniform(0.10, 0.90),
            hill_rng.uniform(0.10, 0.24),
            hill_rng.uniform(0.22, 0.46),
            hill_rng.uniform(-0.9, 0.9),
            hill_rng.uniform(0.08, 0.17),
        ))

    raw = array.array("f", [0.0]) * total
    minimum = float("inf")
    maximum = float("-inf")
    for y in range(resolution):
        v = y / (resolution - 1)
        row = y * resolution
        for x in range(resolution):
            u = x / (resolution - 1)
            base_value = max(0.0, min(1.0, base[row + x]))
            center = valley_center[x]
            valley_distance = abs(v - center)
            valley = math.exp(-((valley_distance / 0.085) ** 2))

            hill_value = 0.0
            for hx, hy, sx, sy, angle, amplitude in hills:
                dx = u - hx
                dy = v - hy
                cosine = math.cos(angle)
                sine = math.sin(angle)
                along = cosine * dx + sine * dy
                across = -sine * dx + cosine * dy
                hill_value += amplitude * math.exp(
                    -0.5 * ((along / sx) ** 2 + (across / sy) ** 2))

            asymmetric_slope = 0.055 * (u - 0.5) + 0.030 * (0.5 - v)
            height = (
                0.18 +
                0.64 * base_value +
                hill_value +
                asymmetric_slope -
                0.26 * valley
            )
            raw[row + x] = height
            minimum = min(minimum, height)
            maximum = max(maximum, height)

    if not math.isfinite(minimum) or not math.isfinite(maximum) or maximum - minimum < 1.0e-4:
        _fail("generated height field is degenerate")
    output = array.array("H", [0]) * total
    inverse_span = 1.0 / (maximum - minimum)
    for index in range(total):
        normalized = (raw[index] - minimum) * inverse_span
        normalized = 0.02 + 0.96 * max(0.0, min(1.0, normalized))
        output[index] = int(round(normalized * 65535.0))

    metrics = {
        "minimum": minimum,
        "maximum": maximum,
        "normalizedMin": min(output) / 65535.0,
        "normalizedMax": max(output) / 65535.0,
    }
    return output, metrics


def _smoothstep(edge0: float, edge1: float, value: float) -> float:
    if edge0 == edge1:
        return 0.0 if value < edge0 else 1.0
    t = max(0.0, min(1.0, (value - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def _sample_height(
    heights: array.array,
    size: int,
    u: float,
    v: float,
) -> float:
    u = max(0.0, min(1.0, u))
    v = max(0.0, min(1.0, v))
    fx = u * (size - 1)
    fy = v * (size - 1)
    x0 = int(math.floor(fx))
    y0 = int(math.floor(fy))
    x1 = min(size - 1, x0 + 1)
    y1 = min(size - 1, y0 + 1)
    tx = fx - x0
    ty = fy - y0
    h00 = heights[y0 * size + x0]
    h10 = heights[y0 * size + x1]
    h01 = heights[y1 * size + x0]
    h11 = heights[y1 * size + x1]
    top = h00 + (h10 - h00) * tx
    bottom = h01 + (h11 - h01) * tx
    return (top + (bottom - top) * ty) / 65535.0


def _generate_weights(
    heights: array.array,
    seed: int,
    size_x_cm: float,
    size_z_cm: float,
    height_scale_cm: float,
) -> tuple[list[bytes], list[dict[str, float]]]:
    resolution = WEIGHT_RESOLUTION
    total = resolution * resolution
    patch_a = _octave_field(seed ^ 0x1B56C4E9, resolution, ((6, 0.62), (13, 0.28), (29, 0.10)))
    patch_b = _octave_field(seed ^ 0x7A2D91C3, resolution, ((5, 0.60), (11, 0.29), (23, 0.11)))
    sparse = _octave_field(seed ^ 0x43F19E2D, resolution, ((7, 0.55), (15, 0.30), (31, 0.15)))

    path_rng = random.Random(seed ^ 0x51A7C0DE)
    valley_center_hd = _catmull_rom_path(path_rng, 8, 0.34, 0.68)
    valley_center = [
        valley_center_hd[min(HEIGHT_RESOLUTION - 1,
                             int(round(index / (resolution - 1) * (HEIGHT_RESOLUTION - 1))))]
        for index in range(resolution)
    ]
    path_a = _catmull_rom_path(
        random.Random(seed ^ 0x68D39A11), 7, 0.18, 0.48)
    path_b = _catmull_rom_path(
        random.Random(seed ^ 0x22B7F30D), 7, 0.52, 0.82)
    path_a_center = [
        path_a[min(HEIGHT_RESOLUTION - 1,
                   int(round(index / (resolution - 1) * (HEIGHT_RESOLUTION - 1))))]
        for index in range(resolution)
    ]
    path_b_center = [
        path_b[min(HEIGHT_RESOLUTION - 1,
                   int(round(index / (resolution - 1) * (HEIGHT_RESOLUTION - 1))))]
        for index in range(resolution)
    ]

    buffers = [bytearray(total) for _ in WEIGHT_LAYERS]
    stats = [
        {"minimum": 255, "maximum": 0, "nonzero": 0, "total": 0}
        for _ in WEIGHT_LAYERS
    ]
    eps = 1.0 / resolution
    for y in range(resolution):
        v = y / (resolution - 1)
        for x in range(resolution):
            u = x / (resolution - 1)
            height = _sample_height(heights, HEIGHT_RESOLUTION, u, v)
            left = _sample_height(heights, HEIGHT_RESOLUTION, u - eps, v)
            right = _sample_height(heights, HEIGHT_RESOLUTION, u + eps, v)
            down = _sample_height(heights, HEIGHT_RESOLUTION, u, v - eps)
            up = _sample_height(heights, HEIGHT_RESOLUTION, u, v + eps)
            world_dx = 2.0 * eps * size_x_cm
            world_dz = 2.0 * eps * size_z_cm
            slope = math.degrees(math.atan(math.hypot(
                (right - left) * height_scale_cm / world_dx,
                (up - down) * height_scale_cm / world_dz)))

            river_distance = abs(v - valley_center[x])
            valley = math.exp(-((river_distance / 0.085) ** 2))
            bank = math.exp(-(((river_distance - 0.085) / 0.060) ** 2))
            low = 1.0 - _smoothstep(0.16, 0.52, height)
            mid_low = math.exp(-(((height - 0.36) / 0.18) ** 2))
            mid = math.exp(-(((height - 0.48) / 0.18) ** 2))
            high = _smoothstep(0.52, 0.82, height)
            flat = 1.0 - _smoothstep(7.0, 28.0, slope)
            steep = _smoothstep(16.0, 52.0, slope)
            broad_mid = math.exp(-(((height - 0.40) / 0.27) ** 2))
            path_a_mask = math.exp(
                -(((v - path_a_center[x]) / 0.045) ** 2))
            path_b_mask = math.exp(
                -(((v - path_b_center[x]) / 0.038) ** 2))
            sparse_value = max(0.0, min(1.0, sparse[y * resolution + x]))
            sparse_mask = _smoothstep(0.62, 0.84, sparse_value) ** 2
            patch_value_a = max(0.0, min(1.0, patch_a[y * resolution + x]))
            patch_value_b = max(0.0, min(1.0, patch_b[y * resolution + x]))

            weights = [
                0.025 + 1.00 * broad_mid * (0.55 + 0.45 * patch_value_a),
                0.020 + 0.82 * broad_mid * (0.55 + 0.45 * patch_value_b),
                0.0005 + 2.00 * high * steep * sparse_mask,
                0.014 + 0.88 * mid_low * flat * path_a_mask,
                0.010 + 0.78 * mid_low * (0.35 + 0.65 * flat) * path_b_mask,
                0.018 + 0.95 * bank * (0.35 + 0.65 * flat),
                0.018 + 1.75 * valley * flat,
                0.010 + 1.15 * high * steep,
                0.018 + 0.88 * mid * path_b_mask * (0.35 + 0.65 * steep),
            ]
            weight_sum = sum(weights)
            if weight_sum <= 0.0:
                rounded = [0] * len(weights)
                rounded[0] = 255
            else:
                rounded = [
                    max(0, min(255, int(value / weight_sum * 255.0 + 0.5)))
                    for value in weights
                ]
                current = sum(rounded)
                if current == 0:
                    rounded[0] = 255
                else:
                    largest = max(range(len(rounded)), key=lambda index: rounded[index])
                    rounded[largest] = max(
                        0, min(255, rounded[largest] + 255 - current))

            offset = y * resolution + x
            for index, value in enumerate(rounded):
                buffers[index][offset] = value
                state = stats[index]
                state["minimum"] = min(state["minimum"], value)
                state["maximum"] = max(state["maximum"], value)
                state["nonzero"] += 1 if value else 0
                state["total"] += value

    summaries: list[dict[str, float]] = []
    for state in stats:
        summaries.append({
            "minimum": float(state["minimum"]),
            "maximum": float(state["maximum"]),
            "mean": state["total"] / total,
            "coverage": state["nonzero"] / total,
        })
    return [bytes(buffer) for buffer in buffers], summaries


def _height_metrics(heights: array.array) -> dict[str, float]:
    resolution = HEIGHT_RESOLUTION
    dx = dy = diagonal = 0.0
    count = 0
    for y in range(1, resolution - 1):
        row = y * resolution
        next_row = row + resolution
        for x in range(1, resolution - 1):
            center = heights[row + x]
            dx += abs(heights[row + x + 1] - center)
            dy += abs(heights[next_row + x] - center)
            diagonal += abs(heights[next_row + x + 1] - center)
            count += 1
    dx /= count
    dy /= count
    diagonal /= count
    smallest = max(1.0e-9, min(dx, dy, diagonal))
    return {
        "meanDx": dx,
        "meanDy": dy,
        "meanDiagonal": diagonal,
        "anisotropy": max(dx, dy, diagonal) / smallest,
    }


def _manifest_values(manifest_path: Path) -> tuple[float, float, float]:
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        _fail(f"cannot read manifest {manifest_path}: {error}")
    size = data.get("size", [HEIGHT_SIZE_CM, HEIGHT_SIZE_CM])
    if not isinstance(size, list) or len(size) != 2:
        _fail("manifest size must be a vec2")
    return float(size[0]), float(size[1]), float(
        data.get("heightScale", HEIGHT_SCALE_CM))


def _update_manifest(manifest_path: Path) -> bool:
    try:
        data = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        _fail(f"cannot read manifest {manifest_path}: {error}")
    data["heightmapPath"] = HEIGHT_RELATIVE.as_posix()
    data["gridResolution"] = 2048
    data["chunkQuads"] = 256
    data["lodLevels"] = 2
    data["materialUvScale"] = 2.0
    data["description"] = (
        "KiteDemo GroundTiles 真实地表纹理导入；200m x 200m 地形的高度图与九层权重"
        "由确定性程序生成，因为 KiteDemo Overview/Showcase 没有 ALandscape actor。"
        "2048 quads / 256-quad chunks / 2 LOD 为实时查看设置；不参与论文数值验证。"
    )
    data["sceneDescription"] = (
        "KiteDemo GroundTiles 地表材质与确定性 200m 地形查看场景；高度和权重不是 "
        "UE Landscape 导出，用于资产导入与性能验收，不参与论文验证。"
    )
    return _atomic_write_json(manifest_path, data)


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _run_self_check(seed: int, verbose: bool) -> None:
    first_height, first_stats = _generate_height(seed)
    second_height, second_stats = _generate_height(seed)
    if first_height.tobytes() != second_height.tobytes():
        _fail("height generation is not deterministic")
    if first_stats != second_stats:
        _fail("height metrics changed between identical runs")
    size_x, size_z, height_scale = HEIGHT_SIZE_CM, HEIGHT_SIZE_CM, HEIGHT_SCALE_CM
    first_weights, first_weight_stats = _generate_weights(
        first_height, seed, size_x, size_z, height_scale)
    second_weights, second_weight_stats = _generate_weights(
        second_height, seed, size_x, size_z, height_scale)
    if first_weights != second_weights or first_weight_stats != second_weight_stats:
        _fail("weight generation is not deterministic")
    for index in range(WEIGHT_RESOLUTION * WEIGHT_RESOLUTION):
        if sum(buffer[index] for buffer in first_weights) != 255:
            _fail("weight self-check found a texel whose normalization is not 255")
    metrics = _height_metrics(first_height)
    if metrics["anisotropy"] > 2.5:
        _fail(
            "height field has excessive directional anisotropy: "
            f"{metrics['anisotropy']:.3f}")
    print(
        "SELF-CHECK PASS: deterministic PNG16 height, "
        f"sha256={_sha256_bytes(first_height.tobytes())[:16]}, "
        f"anisotropy={metrics['anisotropy']:.3f}, "
        f"weights={len(first_weights)}x512x512 normalized.")
    if verbose:
        print(json.dumps({
            "height": first_stats,
            "directionalMetrics": metrics,
            "weights": [
                {"name": name, **stats}
                for (name, _), stats in zip(WEIGHT_LAYERS, first_weight_stats)
            ],
        }, ensure_ascii=False, indent=2))


def _run_generation(
    resource_root: Path,
    manifest_path: Path,
    seed: int,
    verbose: bool,
) -> None:
    output_root = resource_root.resolve() / OUTPUT_RELATIVE
    height_path = output_root / HEIGHT_RELATIVE
    weight_dir = output_root / WEIGHT_RELATIVE
    size_x, size_z, height_scale = _manifest_values(manifest_path)

    heights, height_stats = _generate_height(seed)
    height_image = Image.new("I;16", (HEIGHT_RESOLUTION, HEIGHT_RESOLUTION))
    height_image.putdata(heights)
    height_changed = _atomic_save_png(height_path, height_image)

    weight_payloads, weight_stats = _generate_weights(
        heights, seed, size_x, size_z, height_scale)
    weight_changed = 0
    for (_, filename), payload in zip(WEIGHT_LAYERS, weight_payloads):
        image = Image.frombytes(
            "L", (WEIGHT_RESOLUTION, WEIGHT_RESOLUTION), payload)
        if _atomic_save_png(weight_dir / filename, image):
            weight_changed += 1

    manifest_changed = _update_manifest(manifest_path)
    metrics = _height_metrics(heights)
    print(
        f"Generated {HEIGHT_RESOLUTION}x{HEIGHT_RESOLUTION} PNG16 {height_path}")
    print(
        f"Height range normalized [{height_stats['normalizedMin']:.4f}, "
        f"{height_stats['normalizedMax']:.4f}], "
        f"directional anisotropy {metrics['anisotropy']:.3f}")
    for (name, filename), stats in zip(WEIGHT_LAYERS, weight_stats):
        print(
            f"WEIGHT {filename}: mean={stats['mean']:.2f} "
            f"coverage={stats['coverage'] * 100.0:.1f}% "
            f"range=[{stats['minimum']:.0f}, {stats['maximum']:.0f}]")
    print(
        f"Changed height={int(height_changed)}, weights={weight_changed}/9, "
        f"manifest={int(manifest_changed)}")
    if verbose:
        print(json.dumps({
            "seed": seed,
            "height": height_stats,
            "directionalMetrics": metrics,
            "weights": [
                {"name": name, **stats}
                for (name, _), stats in zip(WEIGHT_LAYERS, weight_stats)
            ],
        }, ensure_ascii=False, indent=2))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate deterministic KiteDemo height and weight maps.")
    parser.add_argument(
        "--resource-root", type=Path, default=DEFAULT_RESOURCE_ROOT,
        help="VulkanLearn resource root")
    parser.add_argument(
        "--manifest", type=Path,
        help="manifest path; defaults to <resource-root>/Generated/KiteDemo/kitedemo_manifest.json")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--self-check", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.self_check:
            _run_self_check(args.seed, args.verbose)
            return 0
        manifest_path = args.manifest or (
            args.resource_root.resolve() / OUTPUT_RELATIVE / "kitedemo_manifest.json")
        if not manifest_path.is_file():
            _fail(f"manifest does not exist: {manifest_path}")
        _run_generation(
            args.resource_root, manifest_path, args.seed, args.verbose)
        return 0
    except GeneratorError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
