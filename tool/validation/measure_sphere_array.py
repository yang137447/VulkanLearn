"""球阵基线 `SC_sphere_array` 的数值判定（plan `D-10`，共享层 §1.7.1 的标尺场景）。

球阵是**所有模型的参数标尺**：11×11 = 121 个 `M_pbr` 球，x 轴 = metallic（0..1，步长 0.1）、
y 轴 = roughness（0.05..1，步长 0.095）。本工具把"标尺现在长什么样"变成可复核的数字：
按相机模型投影出每个球的圆心，在圆盘内取线性亮度均值，然后检查三条**回归判据**。

## 判据来自实测，不是先验假设（这一条很重要）

| 判据 | 实测余量 | 为什么可以断言 |
| --- | --- | --- |
| 每个 roughness 上 `metallic=1` 比 `metallic=0` 暗 | 最小 **0.443** | 全金属没有漫反射项（`D-08` 已证明 diffuse 权重恒为 0），F0=baseColor 只反射环境 |
| 固定 roughness，沿 metallic 增大亮度**单调递减** | 最小步长 **0.016** | 固定的 0.5 灰 albedo + 该环境下漫反射比金属反射更亮 |
| `metallic=0` 时沿 roughness 增大亮度**单调递增** | 最小步长 **0.006** | 介电球以漫反射为主，粗糙度越大 Fresnel 阻尼越弱 |

**故意不断言的一条**：`metallic=1` 沿 roughness 并**不单调**（实测在 r≈0.43 处有峰值）——
这是"镜面瓣展宽"与"单次散射能量损失"在该 HDRI + 点光下的竞争结果，是**场景相关**的现象，
不是公式性质。把它记成观测值，免得以后有人照"能量随粗糙度单调下降"去断言而误判。

本工具不做目视判断，也不调用识图模型：判定全部是数值。

用法：

    py -3 tool/validation/measure_sphere_array.py <rn_sphere_array.bmp> [--min-step 0.002]
"""

from __future__ import annotations

import argparse
import math
import sys

from bmp_reader import BmpImage, srgb_to_linear
from measure_plot_curve import CameraModel

# 场景实参（Maps/SC_sphere_array）：球心间距与球半径来自 mesh scale，机位来自场景相机。
SPHERE_RADIUS = 0.35
SPHERE_SPACING = 0.9
GRID_HALF_EXTENT = 4.5
CENTER_OFFSET = -4.5
DISC_FILL = 0.62  # 圆盘采样半径 = 球投影半径的 62%，避开轮廓与相邻球的间隙

METALLIC_STEP = 0.1
ROUGHNESS_BASE = 0.05
ROUGHNESS_STEP = 0.095


def luminance(image: BmpImage, x: int, y: int) -> float:
    r, g, b = image.rgb(x, y)
    return (
        0.2126 * srgb_to_linear(r / 255.0)
        + 0.7152 * srgb_to_linear(g / 255.0)
        + 0.0722 * srgb_to_linear(b / 255.0)
    )


def disc_mean(image: BmpImage, center_x: float, center_y: float, radius: float) -> float:
    total = 0.0
    count = 0
    for y in range(int(center_y - radius), int(center_y + radius) + 1):
        for x in range(int(center_x - radius), int(center_x + radius) + 1):
            if (x - center_x) ** 2 + (y - center_y) ** 2 > radius * radius:
                continue
            if x < 0 or y < 0 or x >= image.width or y >= image.height:
                continue
            total += luminance(image, x, y)
            count += 1
    return total / count if count else float("nan")


def main(argv: list[str]) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("bmp")
    parser.add_argument("--distance", type=float, default=16.0, help="相机 z（场景实测 16）")
    parser.add_argument("--fov", type=float, default=80.0, help="水平视场角（场景实测 80）")
    parser.add_argument(
        "--min-step", type=float, default=0.002,
        help="单调判据的最小相邻步长（默认 0.002；实测最小 0.006 / 0.016）",
    )
    parser.add_argument(
        "--min-endpoint-gap", type=float, default=0.05,
        help="metallic 端点列的最小差值（默认 0.05；实测最小 0.443）",
    )
    args = parser.parse_args(argv)

    image = BmpImage.load(args.bmp)
    print(f"图片: {args.bmp}  {image.width}x{image.height}")
    print("四角检查（这条命令只判球阵，不要求面板铺满；四角仅供参考）:")
    for name, (x, y) in (
        ("左上", (0, 0)), ("右上", (image.width - 1, 0)),
        ("左下", (0, image.height - 1)), ("右下", (image.width - 1, image.height - 1)),
    ):
        print(f"  {name} rgb={image.rgb(x, y)}")

    # 相机模型：与曲线探针共用同一个实现，避免"投影公式两处各写一份"。
    camera = CameraModel(
        width=image.width,
        height=image.height,
        camera_distance=args.distance,
        fov_degrees=args.fov,
        scale_x=1.0,
        scale_y=1.0,
    )
    pixels_per_unit = (image.width * 0.5) / camera.half_width
    radius_px = SPHERE_RADIUS * pixels_per_unit
    sample_radius = radius_px * DISC_FILL
    print(f"相机模型: {camera.describe()}")
    print(f"投影尺度: {pixels_per_unit:.2f} px/单位  球投影半径 {radius_px:.1f} px  "
          f"圆盘采样半径 {sample_radius:.1f} px")

    def project(world_x: float, world_y: float) -> tuple[float, float]:
        ndc_x = world_x / camera.half_width
        ndc_y = world_y / camera.half_height
        return (ndc_x + 1.0) * 0.5 * image.width, (1.0 - ndc_y) * 0.5 * image.height

    table: dict[tuple[int, int], float] = {}
    for x in range(11):
        for y in range(11):
            world_x = CENTER_OFFSET + SPHERE_SPACING * x
            world_y = CENTER_OFFSET + SPHERE_SPACING * y
            center_x, center_y = project(world_x, world_y)
            table[(x, y)] = disc_mean(image, center_x, center_y, sample_radius)

    print("\n圆盘平均线性亮度（列 = metallic，行 = roughness）")
    header = "        " + "".join(f"m={METALLIC_STEP * x:4.1f} " for x in range(11))
    print(header)
    for y in range(11):
        row = "".join(f"{table[(x, y)]:8.4f} " for x in range(11))
        print(f"r={ROUGHNESS_BASE + ROUGHNESS_STEP * y:5.3f} {row}")

    failures: list[str] = []

    # ---- 判据 1：每个 roughness 上，全金属比纯介电暗 ----
    gaps = [table[(0, y)] - table[(10, y)] for y in range(11)]
    print(f"\n① metallic=1 vs metallic=0 的亮度差：最小 {min(gaps):.4f}（判据 >= {args.min_endpoint_gap:g}）")
    if min(gaps) < args.min_endpoint_gap:
        failures.append(f"全金属未比纯介电暗：最小差 {min(gaps):.4f}")

    # ---- 判据 2：固定 roughness，沿 metallic 单调递减 ----
    worst_metallic = float("inf")
    for y in range(11):
        steps = [table[(x, y)] - table[(x + 1, y)] for x in range(10)]
        worst_metallic = min(worst_metallic, min(steps))
    print(f"② 沿 metallic 递减的最小相邻步长：{worst_metallic:.4f}（判据 >= {args.min_step:g}）")
    if worst_metallic < args.min_step:
        failures.append(f"沿 metallic 非单调：最小步长 {worst_metallic:.4f}")

    # ---- 判据 3：metallic=0 沿 roughness 单调递增 ----
    steps_m0 = [table[(0, y + 1)] - table[(0, y)] for y in range(10)]
    print(f"③ metallic=0 沿 roughness 递增的最小相邻步长：{min(steps_m0):.4f}"
          f"（判据 >= {args.min_step:g}）")
    if min(steps_m0) < args.min_step:
        failures.append(f"metallic=0 沿 roughness 非单调：最小步长 {min(steps_m0):.4f}")

    # ---- 观测（不断言）：全金属列的峰值位置 ----
    conductor = [table[(10, y)] for y in range(11)]
    peak_index = max(range(11), key=lambda y: conductor[y])
    is_monotone = all(conductor[i] >= conductor[i + 1] for i in range(10))
    print(
        f"\n观测（不断言）：metallic=1 列峰值在 roughness={ROUGHNESS_BASE + ROUGHNESS_STEP * peak_index:.3f}"
        f"（{conductor[peak_index]:.4f}），单调递减={is_monotone}——"
        " 瓣展宽与单次散射损失的竞争，场景相关，不是公式性质"
    )

    print()
    if failures:
        for line in failures:
            print(f"不通过: {line}")
        return 1
    print("判定: 通过（球阵端点与两条单调性回归）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
