"""论文 case 的像素级测量工具（shading-model-alignment-plan §1.5.3 的共享 A 设施）。

三个子命令：

  uv-map  —— 读 mode 3 UV 探针截图，把"mesh texCoord ↔ 屏幕像素"的映射**实测**出来，
             并与相机模型（quad scale + fov + aspect 反推）逐点比对。
             它是 M-07 的判定工具：V 方向与 fov 约定必须由它给出，不能靠读代码猜。

  curve   —— 读 mode 0 / 2 的曲线面板截图，按列取曲线行位置，
             与"用同一份 shader 公式在脚本里独立算出的解析期望行位置"逐列比对，
             输出最大 / 均方误差（像素）。同时给出"当前 shader 约定"与"竖直翻转后"
             两种假设的误差，用来判定探针图是否被镜像。

  panel   —— 面板铺满 / 四角检查（§1.7.2.1）。给 run_paper_case.ps1 当产物校验用。

## 探针面板的成像约定（M-07 实测 + 2026-09-12 的坐标轴改造）

1. mesh texCoord.y 在屏幕上**向上增大**（uv.y = 1 在画面顶部）；
2. 相机 fov 是**水平**视场角：halfWidth = distance·tan(fov/2)，
   halfHeight = halfWidth / aspect；
3. **探针面板必须精确填满画面**：`plot_quad.obj` 的顶点是 ±1，所以探针场景的
   scale 必须取 (halfWidth, halfHeight)，此时 uv 0..1 正好覆盖整幅画面，
   于是 uv.x = (x+0.5)/W、uv.y = 1 - (y+0.5)/H。M_brdfPlot 的像素版式
   （绘图区留白、字号、刻度长度）就是在这条约定下用 fwidth(uv) 换算出来的，
   单位就是屏幕像素；
4. 曲线画在**绘图区**里，绘图区是面板内缩的一个矩形，留白给刻度数值与轴名。
   留白由材质的 `u_plotRectPixels` 给出（左 / 下 / 右 / 上，像素），
   本工具的 `--plot-rect` 必须与它一致；工具还会从图上**检出边框位置做交叉核对**；
5. 轴坐标 axis ∈ [0,1] 再映射成物理量：
   mode 0  x = θh = axisX·90°（deg），y = 归一化分布 D 的 log10 刻度（1e-3..1）
   mode 1  x = sinθl = -1 + 2·axisX，y = Mp 的 log10 刻度（1e-3..1）
   mode 2  x = cosθ = 1 - axisX，y = G1 线性 0..1
6. tonemap 0 + bloom 0 时，swapchain 写出的是 sRGB 编码值：
   pixel8 = 255·sRGB_encode(clamp(shaderLinear))，读数必须先 sRGB 解码；
7. 运行时 UI（RmlUi）即使 `--no-dev-ui` 也会在画面左上角画一条约 186x14 的控件
   （实测 bbox x[31..216] y[32..45]），测量必须避开它；
8. 坐标轴 / 刻度 / 文字 / 网格全部是**无彩色**（通道差 <= 0.08），曲线是强彩色，
   测量用彩度门限把标注排除在曲线判定之外。

本脚本**不做**目视判断，也不调用识图模型：判定全部是数值。
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path

from bmp_reader import BmpImage, linear_to_srgb, srgb_to_linear

# 面板底板色（M_brdfPlot.surface.glsl 的 color 初值）与网格色（mix 目标）。
PLOT_BASE_COLOR = (0.13, 0.14, 0.17)
PLOT_GRID_COLOR = (0.28, 0.30, 0.35)
PLOT_AXIS_COLOR = (0.45, 0.47, 0.52)
PLOT_TEXT_COLOR = (0.72, 0.74, 0.78)

# 曲线描边色（与 M_brdfPlot.surface.glsl 中的 mix() 目标色逐字对应）。
PLOT_CURVE_COLORS = {
    ("mode0", "ggx"): (0.28, 0.85, 0.36),
    ("mode0", "charlie"): (0.95, 0.32, 0.28),
    ("mode1", "r"): (0.95, 0.72, 0.25),
    ("mode1", "tt"): (0.30, 0.80, 0.92),
    ("mode1", "trt"): (0.90, 0.40, 0.85),
    ("mode2", "schlick"): (0.95, 0.32, 0.28),
    ("mode2", "smith"): (0.28, 0.85, 0.36),
}

# 运行时 UI 叠加层实测包围盒（1280x720）。测量时按这个盒子跳过像素。
# 外扩量取 12 px：描边本身有 9~16 px 的纵向厚度，叠加层的抗锯齿边缘会把
# 贴得最近的曲线亮核从一侧啃掉，实测表现为 3 px 量级的中心偏移（x≈132 那一列）。
UI_OVERLAY_BBOX = (31, 32, 216, 45)
UI_OVERLAY_MARGIN = 12

# 材质 u_plotRectPixels 的默认值（左、下、右、上，像素）；运行时优先从材质 JSON 读，
# 这里只是读不到 JSON 时的兜底。
DEFAULT_PLOT_RECT = (92.0, 52.0, 18.0, 20.0)

# 版式常量：必须与 M_brdfPlot.surface.glsl 里的同名常量一致。
#   网格线半宽 = GRID_STROKE_SCALE · u_plotTextPixels.z
#   曲线半宽   = CURVE_WIDTH_IN_GRID_LINES · 网格线半宽
GRID_STROKE_SCALE = 0.6
CURVE_WIDTH_IN_GRID_LINES = 2.0

# 材质定义：plot rect / 文字度量 / 描边宽度都从它读，避免脚本里再抄一份默认值。
MATERIAL_JSON_PATH = Path(__file__).resolve().parents[2] / "shader" / "glsl" / "M_brdfPlot.json"

PLOT_MODE_NAMES = {0: "mode0", 1: "mode1", 2: "mode2", 3: "mode3"}


def read_plot_material_defaults(
    path: Path = MATERIAL_JSON_PATH,
) -> tuple[tuple[float, float, float, float], tuple[float, float, float, float]] | None:
    """读材质 JSON 里 u_plotRectPixels / u_plotTextPixels 的默认值。

    为什么要读而不是再写一份常量：绘图区与描边宽度是材质与工具之间的接口，
    两边各写一个数就会漂移（漂移不会让判定失败，只会悄悄放宽剔除门限）。
    读不到时返回 None，调用方退回兜底值。
    """
    try:
        with path.open("r", encoding="utf-8") as stream:
            material = json.load(stream)
        parameters = material["parameters"]
        rect = parameters["u_plotRectPixels"]["default"]
        text = parameters["u_plotTextPixels"]["default"]
        return (
            (float(rect[0]), float(rect[1]), float(rect[2]), float(rect[3])),
            (float(text[0]), float(text[1]), float(text[2]), float(text[3])),
        )
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
        return None


# ---------------------------------------------------------------------------
# 版式：绘图区矩形与轴坐标
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class PlotLayout:
    """绘图区矩形（像素留白）与面板像素尺寸。

    在"面板精确填满画面"的探针约定下，面板像素就是屏幕像素：
    pixel_x = x + 0.5，pixel_y（y 向上）= height - y - 0.5。
    """

    width: int
    height: int
    left: float
    bottom: float
    right: float
    top: float

    @property
    def frame_left(self) -> float:
        return self.left

    @property
    def frame_right(self) -> float:
        return self.width - self.right

    @property
    def frame_bottom(self) -> float:
        return self.bottom

    @property
    def frame_top(self) -> float:
        return self.height - self.top

    @property
    def span_x(self) -> float:
        return self.frame_right - self.frame_left

    @property
    def span_y(self) -> float:
        return self.frame_top - self.frame_bottom

    def columns(self, step: int) -> range:
        return range(int(math.ceil(self.frame_left)), int(self.frame_right), step)

    def screen_to_axis(self, x: float, y: float) -> tuple[float, float]:
        """屏幕像素（左上原点、y 向下）-> 绘图区轴坐标 [0,1]²。"""
        pixel_x = x + 0.5
        pixel_y = self.height - y - 0.5
        return (
            (pixel_x - self.frame_left) / self.span_x,
            (pixel_y - self.frame_bottom) / self.span_y,
        )

    def axis_to_screen_y(self, axis_y: float) -> float:
        pixel_y = self.frame_bottom + axis_y * self.span_y
        return self.height - 0.5 - pixel_y

    def axis_to_screen_x(self, axis_x: float) -> float:
        pixel_x = self.frame_left + axis_x * self.span_x
        return pixel_x - 0.5

    def describe(self) -> str:
        return (
            f"frame x[{self.frame_left:.0f}..{self.frame_right:.0f}] "
            f"y_screen[{self.height - self.frame_top:.0f}..{self.height - self.frame_bottom:.0f}] "
            f"plot {self.span_x:.0f}x{self.span_y:.0f} px"
        )


@dataclass(frozen=True)
class CameraModel:
    """水平 fov 约定下的 uv ↔ 屏幕映射（M-07 实测确认的形态）。"""

    width: int
    height: int
    camera_distance: float
    fov_degrees: float
    scale_x: float
    scale_y: float

    @property
    def aspect(self) -> float:
        return self.width / self.height

    @property
    def half_width(self) -> float:
        return self.camera_distance * math.tan(math.radians(self.fov_degrees) * 0.5)

    @property
    def half_height(self) -> float:
        return self.half_width / self.aspect

    def screen_to_uv(self, x: float, y: float) -> tuple[float, float]:
        """像素坐标（左上原点、y 向下）-> mesh texCoord。"""
        ndc_x = 2.0 * (x + 0.5) / self.width - 1.0
        ndc_y = 2.0 * (y + 0.5) / self.height - 1.0
        world_x = ndc_x * self.half_width
        # Vulkan NDC 的 y 向下，世界 y 向上：这里显式取负，正是 V 方向的落点。
        world_y = -ndc_y * self.half_height
        return (
            0.5 + world_x / (2.0 * self.scale_x),
            0.5 + world_y / (2.0 * self.scale_y),
        )

    def fills_frame(self, tolerance: float = 5.0e-4) -> bool:
        """探针面板是否精确填满画面（uv 0..1 ↔ 整幅画面）。"""
        return (
            abs(self.scale_x - self.half_width) <= tolerance
            and abs(self.scale_y - self.half_height) <= tolerance
        )

    def describe(self) -> str:
        return (
            f"halfWidth={self.half_width:.5f} halfHeight={self.half_height:.5f} "
            f"aspect={self.aspect:.5f} scale=({self.scale_x:g}, {self.scale_y:g}) "
            f"fillsFrame={self.fills_frame()}"
        )


# ---------------------------------------------------------------------------
# shader 公式的 python 复刻（与 shader/glsl 中的实现逐字对应）
# ---------------------------------------------------------------------------


def distribution_ggx(cos_theta_h: float, roughness: float) -> float:
    """common/microfacetDistribution.glsl: DistributionGGX（入参是 perceptual roughness）。"""
    alpha = roughness * roughness
    alpha_squared = alpha * alpha
    cos_squared = cos_theta_h * cos_theta_h
    denominator = math.pi * (cos_squared * (alpha_squared - 1.0) + 1.0) ** 2
    return alpha_squared / denominator


def geometry_schlick_ggx(cos_theta: float, roughness: float) -> float:
    """common/microfacetDistribution.glsl: GeometrySchlickGGX（k=(r+1)^2/8 的 IBL 变体）。"""
    r = roughness + 1.0
    k = (r * r) / 8.0
    cos_theta = max(cos_theta, 0.0)
    return cos_theta / (cos_theta * (1.0 - k) + k)


def smith_g1_ggx(cos_theta: float, roughness: float) -> float:
    """common/microfacetDistribution.glsl: SmithG1Ggx（精确解，仅供参考曲线）。"""
    alpha = roughness * roughness
    alpha_squared = alpha * alpha
    clamped = min(max(cos_theta, 0.0), 1.0)
    root = math.sqrt(alpha_squared + (1.0 - alpha_squared) * clamped * clamped)
    return 2.0 * clamped / (clamped + root)


def cloth_sheen_roughness_to_alpha(sheen_roughness: float) -> float:
    """common/clothBrdf.glsl: ClothSheenRoughnessToAlpha（alpha = r^2）。"""
    return sheen_roughness * sheen_roughness


def cloth_charlie_distribution(alpha: float, cos_theta_h: float) -> float:
    """common/clothBrdf.glsl: ClothCharlieDistribution（Estévez & Kulla 2017 式(2)）。"""
    cos_theta_h = min(max(cos_theta_h, 0.0), 1.0)
    sin_theta_h = math.sqrt(max(1.0 - cos_theta_h * cos_theta_h, 0.0))
    if sin_theta_h <= 0.0:
        return 0.0
    return (2.0 + 1.0 / alpha) * (sin_theta_h ** (1.0 / alpha)) / (2.0 * math.pi)


def plot_log_y(normalized_value: float) -> float:
    """M_brdfPlot.surface.glsl: PlotLogY（log10 压缩 3 个数量级）。"""
    safe_value = max(normalized_value, 1.0e-4)
    return min(max((math.log2(safe_value) / math.log2(10.0) + 3.0) / 3.0, 0.0), 1.0)


PLOT_SAMPLE_COUNT = 256


def plot_curve_value(mode: str, curve: str, axis_x: float, roughness: float) -> float:
    """给定绘图区横轴坐标 axis_x ∈ [0,1]，算出该曲线被画出的纵轴值（axis 空间 0..1）。"""
    axis_x = min(max(axis_x, 0.0), 1.0)

    if mode == "mode0":
        theta_h = axis_x * (math.pi * 0.5)
        if curve == "ggx":
            value = distribution_ggx(math.cos(theta_h), roughness)
            peak = max(
                distribution_ggx(math.cos(i / PLOT_SAMPLE_COUNT * math.pi * 0.5), roughness)
                for i in range(PLOT_SAMPLE_COUNT + 1)
            )
        else:
            alpha = cloth_sheen_roughness_to_alpha(roughness)
            value = cloth_charlie_distribution(alpha, math.cos(theta_h))
            peak = max(
                cloth_charlie_distribution(alpha, math.cos(i / PLOT_SAMPLE_COUNT * math.pi * 0.5))
                for i in range(PLOT_SAMPLE_COUNT + 1)
            )
        return plot_log_y(value / max(peak, 1.0e-6))

    if mode == "mode2":
        cos_theta = 1.0 - axis_x
        if curve == "schlick":
            return min(max(geometry_schlick_ggx(cos_theta, roughness), 0.0), 1.0)
        return min(max(smith_g1_ggx(cos_theta, roughness), 0.0), 1.0)

    raise ValueError(f"mode {mode} curve prediction is not implemented yet")


# ---------------------------------------------------------------------------
# 图像侧：面板 / 曲线识别
# ---------------------------------------------------------------------------


def is_ui_overlay_pixel(x: int, y: int) -> bool:
    left, top, right, bottom = UI_OVERLAY_BBOX
    return (
        left - UI_OVERLAY_MARGIN <= x <= right + UI_OVERLAY_MARGIN
        and top - UI_OVERLAY_MARGIN <= y <= bottom + UI_OVERLAY_MARGIN
    )


def encode_expectation(linear_color: tuple[float, float, float]) -> tuple[float, float, float]:
    """线性色 -> 期望读到的 8-bit 值（含 swapchain 的 sRGB 编码）。"""
    return tuple(linear_to_srgb(component) * 255.0 for component in linear_color)


def classify_pixel(pixel_rgb: tuple[int, int, int], tolerance: float = 3.0) -> str:
    """像素是底板 / 网格 / 坐标轴 / 文字 / 其它（用于铺满与边框检查）。"""
    for name, linear_color in (
        ("底板色", PLOT_BASE_COLOR),
        ("网格色", PLOT_GRID_COLOR),
        ("轴色", PLOT_AXIS_COLOR),
        ("文字色", PLOT_TEXT_COLOR),
    ):
        expected = encode_expectation(linear_color)
        if all(abs(pixel_rgb[i] - expected[i]) <= tolerance for i in range(3)):
            return name
    return "其它（背景或 UI 叠加）"


def describe_corner_check(image: BmpImage, panel_mode: bool) -> tuple[list[str], bool]:
    """四角检查（§1.7.2.1）。panel_mode=True 时面板用 mode 3 的 UV 色，四角是 sRGB 编码的 uv。"""
    lines = []
    ok = True
    corners = {
        "左上": (0, 0),
        "右上": (image.width - 1, 0),
        "左下": (0, image.height - 1),
        "右下": (image.width - 1, image.height - 1),
    }
    for name, (x, y) in corners.items():
        pixel = image.rgb(x, y)
        if panel_mode:
            verdict = "UV 面板（蓝通道=0）" if pixel[2] == 0 else "非面板色"
            passed = pixel[2] == 0
        else:
            verdict = classify_pixel(pixel)
            passed = verdict == "底板色"
        ok = ok and passed
        lines.append(f"  {name} ({x},{y}) rgb={pixel} -> {verdict}")
    return lines, ok


def detect_frame(image: BmpImage, layout: PlotLayout, tolerance: int = 3) -> dict[str, float]:
    """从图上检出轴框四条边的位置，用于与声明的 --plot-rect 交叉核对。

    轴框是**无彩色**的轴色线：在画面中央行 / 中央列上，轴色像素应当出现在
    声明位置附近；这里返回实测的左右上下边（屏幕坐标）与它们和声明值的偏差。
    """
    expected_axis = encode_expectation(PLOT_AXIS_COLOR)

    def is_axis(pixel: tuple[int, int, int]) -> bool:
        return all(abs(pixel[i] - expected_axis[i]) <= tolerance for i in range(3))

    middle_y = image.height // 2
    middle_x = image.width // 2

    left = next((x for x in range(0, image.width) if is_axis(image.rgb(x, middle_y))), None)
    right = next(
        (x for x in range(image.width - 1, -1, -1) if is_axis(image.rgb(x, middle_y))), None
    )
    top = next((y for y in range(0, image.height) if is_axis(image.rgb(middle_x, y))), None)
    bottom = next(
        (y for y in range(image.height - 1, -1, -1) if is_axis(image.rgb(middle_x, y))), None
    )

    detected: dict[str, float] = {}
    if left is not None:
        detected["left"] = float(left)
    if right is not None:
        detected["right"] = float(right)
    if top is not None:
        detected["top"] = float(top)
    if bottom is not None:
        detected["bottom"] = float(bottom)
    return detected


def expected_band_thickness(
    mode: str, curve: str, axis_x: float, roughness: float,
    layout: PlotLayout, half_width_px: float,
) -> float:
    """该列上"等宽描边"应有的**垂直**厚度（像素）。

    描边是等宽屏幕宽度：垂直厚度 = 2·halfWidth·sqrt(1+slope²)，slope 为该列的屏幕斜率
    （px/px）。用解析公式算，不依赖图像，才能把"被遮挡"与"这段本来就陡"区分开。
    """
    step = 1.0 / layout.span_x
    left = plot_curve_value(mode, curve, max(axis_x - step, 0.0), roughness)
    right = plot_curve_value(mode, curve, min(axis_x + step, 1.0), roughness)
    slope = abs(
        layout.axis_to_screen_y(right) - layout.axis_to_screen_y(left)
    ) / (2.0 * step * layout.span_x)
    return 2.0 * half_width_px * math.sqrt(1.0 + slope * slope)


def curve_score(
    linear_pixel: tuple[float, float, float],
    curve_color: tuple[float, float, float],
) -> float:
    """像素在线段 base->curveColor 上的投影系数：1 = 正好是曲线描边色。"""
    direction = [curve_color[i] - PLOT_BASE_COLOR[i] for i in range(3)]
    squared_norm = sum(component * component for component in direction)
    if squared_norm <= 0.0:
        return 0.0
    return sum(
        (linear_pixel[i] - PLOT_BASE_COLOR[i]) * direction[i] for i in range(3)
    ) / squared_norm


def is_chromatic(linear_pixel: tuple[float, float, float], minimum_spread: float) -> bool:
    """曲线描边色都是强彩色的；用它把白/灰干扰（坐标标注、UI 叠加）挡在外面。

    实测：坐标轴 0.07、文字 0.06、网格 0.07、底板 0.04，而所有曲线描边色的通道差
    都在 0.50 以上，因此 0.15 的门限能干净地分开两者。
    """
    return max(linear_pixel) - min(linear_pixel) >= minimum_spread


def find_curve_band(
    image: BmpImage,
    x: int,
    curve_color: tuple[float, float, float],
    plateau_threshold: float,
    edge_margin: int,
    chromatic_threshold: float,
    layout: PlotLayout,
) -> tuple[float, int] | None:
    """在绘图区内的一列里找该颜色曲线的**亮核平台**，返回 (中心行, 平台厚度)。

    为什么取平台中心而不是 argmax 或加权重心：`PlotCurveLine` 在
    |value - axisY| <= width 处输出恒为 1，形成一条固定厚度的亮核平台，平台中心
    才是曲线值本身；靠近绘图区上下边界时平台会被截断，此时返回 None 让调用方丢列，
    避免把截断重心当成曲线位置（实测会带来 6 px 量级的假偏移）。
    """
    y_from = int(math.ceil(image.height - layout.frame_top)) + edge_margin
    y_to = int(math.floor(image.height - layout.frame_bottom)) - edge_margin
    rows = []
    for y in range(max(y_from, 0), min(y_to, image.height - 1) + 1):
        if is_ui_overlay_pixel(x, y):
            continue
        linear_pixel = image.linear_rgb(x, y)
        if not is_chromatic(linear_pixel, chromatic_threshold):
            continue
        if curve_score(linear_pixel, curve_color) >= plateau_threshold:
            rows.append(y)
    if not rows:
        return None

    runs: list[list[int]] = []
    for y in rows:
        if runs and y == runs[-1][-1] + 1:
            runs[-1].append(y)
        else:
            runs.append([y])
    run = max(runs, key=len)
    if run[0] <= y_from or run[-1] >= y_to:
        return None
    return (run[0] + run[-1]) / 2.0, len(run)


# ---------------------------------------------------------------------------
# 子命令
# ---------------------------------------------------------------------------


def cmd_uv_map(args: argparse.Namespace) -> int:
    image = BmpImage.load(args.bmp)
    model = CameraModel(
        width=image.width,
        height=image.height,
        camera_distance=args.distance,
        fov_degrees=args.fov,
        scale_x=args.scale[0],
        scale_y=args.scale[1],
    )

    print(f"图片: {args.bmp}  {image.width}x{image.height} top_down={image.top_down}")
    print(f"相机模型: {model.describe()}")
    print("四角检查（mode 3 面板 = 蓝通道 0）:")
    corner_lines, corner_ok = describe_corner_check(image, panel_mode=True)
    for line in corner_lines:
        print(line)

    # mode 3 的蓝通道恒为 0；用它判定面板覆盖范围（这是"面板是否铺满"的直接证据）。
    panel_pixels = sum(
        1
        for y in range(image.height)
        for x in range(image.width)
        if image.rgb(x, y)[2] == 0
    )
    coverage = panel_pixels / (image.width * image.height)
    print(f"面板覆盖: {panel_pixels}/{image.width * image.height} = {coverage * 100:.3f}%")

    # 逐列拟合 uv.x(x)，逐行拟合 uv.y(y)。像素先做 sRGB 解码，才等于 shader 的值。
    middle_y = image.height // 2
    middle_x = image.width // 2
    column_samples = [
        (float(x), image.linear_rgb(x, middle_y)[0])
        for x in range(0, image.width, args.sample_step)
        if not is_ui_overlay_pixel(x, middle_y)
    ]
    row_samples = [
        (float(y), image.linear_rgb(middle_x, y)[1])
        for y in range(0, image.height, args.sample_step)
        if not is_ui_overlay_pixel(middle_x, y)
    ]

    ux_slope, ux_intercept, ux_rms = linear_fit(column_samples)
    uy_slope, uy_intercept, uy_rms = linear_fit(row_samples)

    print("\n实测映射（sRGB 解码后线性拟合）:")
    print(f"  uv.x = {ux_slope:.8f}·x + {ux_intercept:.6f}   rms={ux_rms:.3e}")
    print(f"  uv.y = {uy_slope:.8f}·y + {uy_intercept:.6f}   rms={uy_rms:.3e}")
    if uy_slope < 0:
        print("  uv.y 斜率 < 0 -> 向下减小，即 **V 向上：uv.y = 1 在画面顶部**")
    else:
        print("  uv.y 斜率 > 0 -> 向下增大，即 V 向下：uv.y = 1 在画面底部")

    print("\n实测 vs 相机模型（逐点，单位: uv）:")
    probes = [
        ("画面左上", 0, 0),
        ("画面右上", image.width - 1, 0),
        ("画面左下", 0, image.height - 1),
        ("画面右下", image.width - 1, image.height - 1),
        ("画面中心", middle_x, middle_y),
    ]
    worst = 0.0
    for name, x, y in probes:
        actual = image.linear_rgb(x, y)
        measured_uv = (
            ux_intercept + ux_slope * x,
            uy_intercept + uy_slope * y,
        )
        model_uv = model.screen_to_uv(x, y)
        error_x = abs(measured_uv[0] - model_uv[0])
        error_y = abs(measured_uv[1] - model_uv[1])
        worst = max(worst, error_x, error_y)
        print(
            f"  {name}: 像素 uv=({actual[0]:.5f},{actual[1]:.5f}) "
            f"拟合=({measured_uv[0]:.5f},{measured_uv[1]:.5f}) "
            f"模型=({model_uv[0]:.5f},{model_uv[1]:.5f}) "
            f"误差=({error_x:.2e},{error_y:.2e})"
        )

    # 反推可见半宽 / 半高：uv.x = 0.5 + worldX/(2·scaleX)，worldX = ndc_x·halfWidth。
    half_width_from_fit = ux_slope * image.width * model.scale_x
    half_height_from_fit = -uy_slope * image.height * model.scale_y
    print("\n由拟合反推的可见范围:")
    print(f"  halfWidth  = {half_width_from_fit:.5f}   模型值 = {model.half_width:.5f}")
    print(f"  halfHeight = {half_height_from_fit:.5f}   模型值 = {model.half_height:.5f}")

    fits = model.fills_frame()
    print(
        "\n探针约定检查: 面板 scale 必须等于 (halfWidth, halfHeight) 才能让 uv 0..1 "
        f"覆盖整幅画面 -> {'满足' if fits else '不满足（坐标轴版式会整体偏移）'}"
    )

    passed = worst <= args.tolerance and corner_ok and coverage >= args.min_coverage and fits
    print(f"\n判定: 最大逐点误差 {worst:.3e} uv（阈值 {args.tolerance:g}）-> {'通过' if passed else '不通过'}")
    return 0 if passed else 1


def linear_fit(samples: list[tuple[float, float]]) -> tuple[float, float, float]:
    """最小二乘 y = a·x + b，返回 (a, b, rms)。"""
    count = len(samples)
    if count < 2:
        raise ValueError("linear_fit needs at least two samples")
    mean_x = sum(s[0] for s in samples) / count
    mean_y = sum(s[1] for s in samples) / count
    denominator = sum((s[0] - mean_x) ** 2 for s in samples)
    if denominator == 0.0:
        raise ValueError("linear_fit degenerate input")
    slope = sum((s[0] - mean_x) * (s[1] - mean_y) for s in samples) / denominator
    intercept = mean_y - slope * mean_x
    rms = math.sqrt(
        sum((s[1] - (slope * s[0] + intercept)) ** 2 for s in samples) / count
    )
    return slope, intercept, rms


def parse_plot_rect(text: str) -> tuple[float, float, float, float]:
    parts = [float(part) for part in text.replace(" ", "").split(",")]
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("--plot-rect needs four values: left,bottom,right,top")
    return parts[0], parts[1], parts[2], parts[3]


def cmd_curve(args: argparse.Namespace) -> int:
    image = BmpImage.load(args.bmp)

    # 绘图区与描边宽度优先取命令行的显式值，否则从材质 JSON 读；两者都说明来源，
    # 因为"脚本用的留白 / 宽度是不是材质那一套"直接决定误差表的可信度。
    material_defaults = read_plot_material_defaults(Path(args.material_json))
    if args.plot_rect is not None:
        plot_rect, plot_rect_source = args.plot_rect, "命令行 --plot-rect"
    elif material_defaults is not None:
        plot_rect, plot_rect_source = material_defaults[0], f"材质 JSON {args.material_json}"
    else:
        plot_rect, plot_rect_source = DEFAULT_PLOT_RECT, "内置兜底值"

    if args.curve_half_width_px is not None:
        curve_half_width, width_source = (
            args.curve_half_width_px, "命令行 --curve-half-width-px"
        )
    elif material_defaults is not None:
        curve_half_width = (
            CURVE_WIDTH_IN_GRID_LINES * GRID_STROKE_SCALE * material_defaults[1][2]
        )
        width_source = (
            f"材质 JSON u_plotTextPixels.z × {CURVE_WIDTH_IN_GRID_LINES:g} × "
            f"{GRID_STROKE_SCALE:g}（曲线宽 = {CURVE_WIDTH_IN_GRID_LINES:g} 倍网格线宽）"
        )
    else:
        curve_half_width = CURVE_WIDTH_IN_GRID_LINES * GRID_STROKE_SCALE
        width_source = "内置兜底值"


    model = CameraModel(
        width=image.width,
        height=image.height,
        camera_distance=args.distance,
        fov_degrees=args.fov,
        scale_x=args.scale[0],
        scale_y=args.scale[1],
    )
    layout = PlotLayout(
        width=image.width,
        height=image.height,
        left=plot_rect[0],
        bottom=plot_rect[1],
        right=plot_rect[2],
        top=plot_rect[3],
    )

    print(f"图片: {args.bmp}  mode={args.mode}  roughness={args.roughness}")
    print(f"相机模型: {model.describe()}")
    print(f"绘图区: {layout.describe()}   <- {plot_rect_source}")
    print(f"曲线描边半宽: {curve_half_width:.3f} px   <- {width_source}")
    if not model.fills_frame():
        print("  ⚠️ scale 与 (halfWidth, halfHeight) 不一致：坐标轴版式与测量映射都不可信")
    print("四角检查:")
    corner_lines, corner_ok = describe_corner_check(image, panel_mode=False)
    for line in corner_lines:
        print(line)

    # 边框交叉核对：声明值 vs 图上检出的轴色线位置。
    frame_ok = True
    if args.check_frame:
        detected = detect_frame(image, layout)
        expected = {
            "left": layout.frame_left,
            "right": layout.frame_right,
            "top": image.height - layout.frame_top,
            "bottom": image.height - layout.frame_bottom,
        }
        print("边框核对（声明 vs 检出，屏幕像素）:")
        for key in ("left", "right", "top", "bottom"):
            if key not in detected:
                print(f"  {key}: 未检出轴色线 -> 不通过")
                frame_ok = False
                continue
            error = abs(detected[key] - expected[key])
            ok = error <= args.frame_tolerance
            frame_ok = frame_ok and ok
            print(
                f"  {key:6s}: 声明 {expected[key]:7.2f}  检出 {detected[key]:7.2f}  "
                f"偏差 {error:5.2f} px -> {'ok' if ok else '超差'}"
            )

    # 逐列采集：先全部量出来，再按三类**模型驱动**的原因剔除不可靠的列：
    # 曲线交叉 / 曲线贴边 / 平台被遮挡。剔除规则必须显式、可复核，
    # 并且**把剔除的列数报出来**——否则就是在挑数据。
    #
    # 平台厚度不能拿一个全局中位数当标准：描边是**等宽屏幕宽度**（见材质里的
    # PlotCurveLine），它的**垂直**厚度按 2·halfWidth·sqrt(1+slope²) 随斜率变化，
    # 所以每列要按该列的解析斜率算出"应该有多厚"，只有明显更薄的列才是被遮挡的。

    per_column: list[
        tuple[int, float, dict[str, float], dict[str, tuple[float, int, float]]]
    ] = []
    for x in layout.columns(args.column_step):
        axis_x = layout.screen_to_axis(x, 0)[0]
        if not (args.axis_x_min <= axis_x <= args.axis_x_max):
            continue
        expected_rows = {
            name: layout.axis_to_screen_y(
                plot_curve_value(args.mode, name, axis_x, args.roughness)
            )
            for name in args.curves
        }
        column_rows: dict[str, tuple[float, int, float]] = {}
        for name in args.curves:
            color = PLOT_CURVE_COLORS[(args.mode, name)]
            found = find_curve_band(
                image, x, color, args.plateau_threshold, args.edge_margin,
                args.chromatic_threshold, layout,
            )
            if found is None:
                continue
            column_rows[name] = (
                found[0],
                found[1],
                expected_band_thickness(
                    args.mode, name, axis_x, args.roughness, layout,
                    curve_half_width,
                ),
            )
        if column_rows:
            per_column.append((x, axis_x, expected_rows, column_rows))

    results: dict[tuple[str, str], list[float]] = {}
    band_thickness: dict[str, list[int]] = {}
    expected_thickness: dict[str, list[float]] = {}
    crossing_columns = 0
    measured_columns = 0
    edge_skipped: dict[str, int] = {name: 0 for name in args.curves}
    thin_skipped: dict[str, int] = {name: 0 for name in args.curves}
    # 贴边判定用的上下限（屏幕坐标）：离绘图区边框太近时带子会被裁掉一半。
    top_limit = image.height - layout.frame_top + args.edge_margin + 3.0
    bottom_limit = image.height - layout.frame_bottom - args.edge_margin - 3.0

    for x, axis_x, expected_rows, column_rows in per_column:
        measured_columns += 1

        # ① 交叉列：两条曲线的描边互相覆盖时会互相啃掉对方的亮核，
        #    用**解析期望行**判断（不依赖"两条都恰好被测到"），靠得够近就整列丢弃。
        rows = sorted(expected_rows.values())
        if len(rows) >= 2 and any(
            rows[i + 1] - rows[i] < args.crossing_margin for i in range(len(rows) - 1)
        ):
            crossing_columns += 1
            continue

        # ②③ 贴边 / 平台过薄：按**每条曲线各自**判定——一条曲线贴着边界不代表
        #     另一条不可用（mode 0 的两条曲线长期各贴一边，整列丢弃会丢掉八成数据）。
        for name, (measured_y, thickness, expectation) in column_rows.items():
            if expected_rows[name] < top_limit or expected_rows[name] > bottom_limit:
                edge_skipped[name] += 1
                continue
            if thickness < max(3.0, args.min_thickness_ratio * expectation):
                thin_skipped[name] += 1
                continue

            band_thickness.setdefault(name, []).append(thickness)
            expected_thickness.setdefault(name, []).append(expectation)
            value = plot_curve_value(args.mode, name, axis_x, args.roughness)
            # 当前 shader（M-07 修正后）：曲线值直接作为轴坐标，值 v 画在 axisY = v 处。
            upright_y = layout.axis_to_screen_y(value)
            # 镜像假设（M-07 修正前的 `plotY = 1 - uv.y`）：值 v 画在 axisY = 1 - v 处。
            mirrored_y = layout.axis_to_screen_y(1.0 - value)
            results.setdefault((name, "upright"), []).append(abs(measured_y - upright_y))
            results.setdefault((name, "mirrored"), []).append(abs(measured_y - mirrored_y))

    skipped_summary = "；".join(
        f"{name}: 贴边 {edge_skipped[name]} / 过薄 {thin_skipped[name]}"
        for name in args.curves
    )
    print(
        f"\n测到曲线的列: {measured_columns}；丢弃的交叉列（整列）: {crossing_columns}；"
        f"按曲线丢弃的列 -> {skipped_summary}"
    )
    print("逐列误差（像素）:")
    verdict_ok = True
    for name in args.curves:
        for hypothesis in ("upright", "mirrored"):
            errors = results.get((name, hypothesis), [])
            label = (
                "当前 shader(值 = 轴坐标)"
                if hypothesis == "upright"
                else "镜像假设(值画在 1-轴坐标)"
            )
            if not errors:
                print(f"  {name:8s} {hypothesis:8s}: 没有测到可用的亮核平台")
                verdict_ok = False
                continue
            mean_error = statistics.fmean(errors)
            max_error = max(errors)
            print(
                f"  {name:8s} {hypothesis:8s}: 列数={len(errors):4d} "
                f"平均={mean_error:7.3f}px 最大={max_error:7.3f}px   <- {label}"
            )
            if hypothesis == "upright":
                thickness = band_thickness.get(name, [])
                expectation = expected_thickness.get(name, [])
                if thickness:
                    print(f"           亮核平台厚度: 中位 {statistics.median(thickness):.0f} px")
                if expectation:
                    print(
                        f"           该曲线应有厚度（等宽描边按斜率折算）: "
                        f"中位 {statistics.median(expectation):.0f} px"
                    )
                verdict_ok = verdict_ok and max_error <= args.tolerance

    passed = verdict_ok and corner_ok and frame_ok
    print(f"\n判定: 直立假设最大误差需 <= {args.tolerance:g} px，且四角 / 边框核对通过 -> "
          f"{'通过' if passed else '不通过'}")
    return 0 if passed else 1


def cmd_panel(args: argparse.Namespace) -> int:
    """面板铺满 / 四角检查，给 run_paper_case.ps1 当产物校验。"""
    image = BmpImage.load(args.bmp)
    panel_mode = args.panel_mode == "uv"
    corner_lines, corner_ok = describe_corner_check(image, panel_mode=panel_mode)
    print(f"图片: {args.bmp}  {image.width}x{image.height}")
    for line in corner_lines:
        print(line)

    if panel_mode:
        coverage = sum(
            1
            for y in range(image.height)
            for x in range(image.width)
            if image.rgb(x, y)[2] == 0
        ) / (image.width * image.height)
        passed = corner_ok and coverage >= args.min_coverage
        print(f"面板覆盖 = {coverage * 100:.3f}%（阈值 {args.min_coverage * 100:.1f}%）")
    else:
        passed = corner_ok
        print("mode 0/1/2 的面板中心是绘图区，四角必须是底板色（坐标轴留白之外）")

    print(f"判定: {'通过' if passed else '不通过'}")
    return 0 if passed else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    uv_parser = subparsers.add_parser("uv-map", help="mode 3 UV 探针的映射实测与模型比对")
    uv_parser.add_argument("bmp")
    uv_parser.add_argument("--scale", type=float, nargs=2, default=[2.485281, 1.397971])
    uv_parser.add_argument("--distance", type=float, default=6.0)
    uv_parser.add_argument("--fov", type=float, default=45.0)
    uv_parser.add_argument("--sample-step", type=int, default=8)
    uv_parser.add_argument("--tolerance", type=float, default=2.0e-3)
    uv_parser.add_argument("--min-coverage", type=float, default=0.995)
    uv_parser.set_defaults(func=cmd_uv_map)

    curve_parser = subparsers.add_parser("curve", help="曲线面板的逐列像素级比对")
    curve_parser.add_argument("bmp")
    curve_parser.add_argument("--mode", choices=["mode0", "mode2"], required=True)
    curve_parser.add_argument("--roughness", type=float, required=True)
    curve_parser.add_argument("--curves", nargs="+", required=True)
    curve_parser.add_argument("--scale", type=float, nargs=2, default=[2.485281, 1.397971])
    curve_parser.add_argument("--distance", type=float, default=6.0)
    curve_parser.add_argument("--fov", type=float, default=45.0)
    curve_parser.add_argument(
        "--plot-rect", type=parse_plot_rect, default=None,
        help="绘图区留白 left,bottom,right,top（像素）；缺省时从材质 JSON 的 u_plotRectPixels 读",
    )
    curve_parser.add_argument(
        "--curve-half-width-px", type=float, default=None,
        help="描边半宽（像素）；缺省时按材质 PLOT_CURVE_WIDTH_IN_GRID_LINES × 网格线半宽算",
    )
    curve_parser.add_argument(
        "--material-json", default=str(MATERIAL_JSON_PATH),
        help="材质定义路径，用于读绘图区 / 字号 / 描边宽度的默认值",
    )
    curve_parser.add_argument("--column-step", type=int, default=4)
    curve_parser.add_argument("--plateau-threshold", type=float, default=0.98)
    curve_parser.add_argument("--chromatic-threshold", type=float, default=0.15)
    curve_parser.add_argument("--edge-margin", type=int, default=3)
    curve_parser.add_argument("--crossing-margin", type=float, default=20.0)
    curve_parser.add_argument(
        "--min-thickness-ratio", type=float, default=0.6,
        help="亮核平台厚度低于该比例（相对中位数）的列视为被遮挡，丢弃",
    )
    curve_parser.add_argument("--tolerance", type=float, default=2.0)
    curve_parser.add_argument("--check-frame", action="store_true", default=True)
    curve_parser.add_argument("--frame-tolerance", type=float, default=2.0)
    curve_parser.add_argument("--axis-x-min", type=float, default=0.0)
    curve_parser.add_argument("--axis-x-max", type=float, default=1.0)
    curve_parser.set_defaults(func=cmd_curve)

    panel_parser = subparsers.add_parser("panel", help="面板铺满 / 四角检查")
    panel_parser.add_argument("bmp")
    panel_parser.add_argument("--panel-mode", choices=["uv", "curve"], default="curve")
    panel_parser.add_argument("--min-coverage", type=float, default=0.98)
    panel_parser.set_defaults(func=cmd_panel)

    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
