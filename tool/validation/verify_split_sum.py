"""split-sum 近似 vs 逐样本参考的环境预积分误差（plan `D-09`，Karis 2013 §4–§5）。

## 判据是什么

Karis §5 把环境预积分拆成两半（split-sum）：

    L_o ≈ PrefilteredEnvironment(R, roughness) · (F0 · A + B)

- `PrefilteredEnvironment` 假设 **N = V = R**，用 GGX 重要性采样把环境按 roughness 预模糊；
- `(A, B)` 是与环境无关的二维 LUT（`IntegrateBRDF`）。

本脚本把这条近似与**逐样本参考**对照：

    L_ref = (1/N) Σ L(l_k) · f(l_k, v) · cosθ_lk / pdf(l_k)      ← 真的对完整 BRDF 积分

两者用**同一个环境**、同一支 G（IBL 变体 `k=α/2`，即被测路径自己用的那支），
所以差值就是 **split-sum 近似本身的误差**，不含几何项近似的误差（后者在 `D-04` / `D-05` 里）。

## 镜像的是引擎自己的实现（不是重抄论文）

- `prefilter()` 逐行对应 `shader/glsl/generator/prefilterEnvMap.comp`：`V = N = R`、NDF 采样 h、
  `l = reflect(-V, h)`、按 `NdotL` 加权平均；
- `(A, B)` 直接调用 `verify_geometry_term.integrate_brdf(..., k_ibl, ...)`（对应 `brfdLut.comp`）；
- 环境采样沿用 `equirectToCubemap.comp` 的约定：`phi = atan(dir.z, dir.x)`、`theta = acos(dir.y)`、
  `uv = (phi/2π + 0.5, theta/π)`，x 方向环绕、y 方向钳制，双线性。

## 环境：为什么用 bistro 的 .hdr 而不是球阵场景的 sunset.exr

`sunset.exr` 是 **DWAB 压缩**的 EXR（oiiotool 生成，DCT 有损），离线脚本解不了；
仓库里的 `bistro_san_giuseppe_bridge_4k.hdr` 是标准 Radiance RGBE，可以纯 Python 解码。
所以 headline 数字用这**一张真实的 4k HDRI**，另外用四个解析环境把"误差随环境频率怎么变"框出来。
**限制写明**：误差量级依赖环境内容，sunset.exr 的具体数字本脚本给不出（见 case 记录的差异归因）。

用法：

    py -3 tool/validation/verify_split_sum.py [--samples 2048] [--prefilter-samples 1024] [--skip-hdr]
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from array import array
from pathlib import Path

from measure_plot_curve import distribution_ggx, geometry_schlick_ggx_ibl
from verify_geometry_term import hammersley, importance_sample_ggx, integrate_brdf, k_ibl

HDR_PATH = Path(
    r"D:\YYBWorkSpace\GitHub\VukanLearnResources"
    r"\Maps\SC_bistro_exterior_modular\Environments\bistro_san_giuseppe_bridge_4k.hdr"
)


# ---------------------------------------------------------------------------
# Radiance RGBE (.hdr) 读取
# ---------------------------------------------------------------------------


def load_radiance_hdr(path: Path) -> tuple[int, int, array]:
    """读 32-bit_rle_rgbe 的 Radiance 文件，返回 (width, height, 线性 RGB float 数组，行序 top-down)。

    只实现本仓库实际用到的情况：`-Y <h> +X <w>`（扫描行自上而下）、新式 RLE 扫描行
    （行首 `2 2 hi lo`，且 lo|hi<<8 == width）。遇到其它变体直接报错，不做"猜格式"。
    """
    data = path.read_bytes()
    if not data.startswith(b"#?"):
        raise ValueError(f"不是 Radiance HDR: {path}")
    # 头部：若干行 `KEY=VALUE`，空行结束
    position = data.index(b"\n") + 1
    while True:
        end = data.index(b"\n", position)
        line = data[position:end].strip()
        position = end + 1
        if line == b"":
            break
        if line.startswith(b"FORMAT=") and b"32-bit_rle_rgbe" not in line:
            raise ValueError(f"只支持 32-bit_rle_rgbe: {line!r}")
    end = data.index(b"\n", position)
    resolution = data[position:end].decode("ascii").strip()
    position = end + 1
    parts = resolution.split()
    if len(parts) != 4 or parts[0] != "-Y" or parts[2] != "+X":
        raise ValueError(f"只支持 '-Y h +X w' 的扫描行顺序: {resolution!r}")
    height, width = int(parts[1]), int(parts[3])

    pixels = array("f", bytes(width * height * 3 * 4))
    for row in range(height):
        header = data[position:position + 4]
        if len(header) < 4 or header[0] != 2 or header[1] != 2:
            raise ValueError("只支持新式 RLE 扫描行（行首 2 2 hi lo）")
        scan_width = (header[2] << 8) | header[3]
        if scan_width != width:
            raise ValueError(f"扫描行宽 {scan_width} != 文件宽 {width}")
        position += 4
        channels = []
        for _ in range(4):
            plane = bytearray()
            while len(plane) < width:
                count = data[position]
                position += 1
                if count > 128:                      # 重复游程
                    value = data[position]
                    position += 1
                    plane.extend(bytes([value]) * (count - 128))
                else:                                # 字面量游程
                    plane.extend(data[position:position + count])
                    position += count
            channels.append(plane)
        base = row * width * 3
        for x in range(width):
            exponent = channels[3][x]
            index = base + x * 3
            if exponent == 0:
                continue
            scale = math.ldexp(1.0, exponent - (128 + 8))
            pixels[index] = (channels[0][x] + 0.5) * scale
            pixels[index + 1] = (channels[1][x] + 0.5) * scale
            pixels[index + 2] = (channels[2][x] + 0.5) * scale
    return width, height, pixels


class EquirectEnvironment:
    """equirect 环境：采样约定与 `generator/equirectToCubemap.comp` 的 `DirectionToUv()` 一致。"""

    def __init__(self, width: int, height: int, pixels: array):
        self.width = width
        self.height = height
        self.pixels = pixels

    def _texel(self, x: int, y: int) -> tuple[float, float, float]:
        x %= self.width
        y = min(max(y, 0), self.height - 1)
        index = (y * self.width + x) * 3
        return self.pixels[index], self.pixels[index + 1], self.pixels[index + 2]

    def sample(self, dx: float, dy: float, dz: float) -> tuple[float, float, float]:
        phi = math.atan2(dz, dx)
        theta = math.acos(min(max(dy, -1.0), 1.0))
        u = math.fmod(phi / (2.0 * math.pi) + 0.5, 1.0)
        if u < 0.0:
            u += 1.0
        v = min(max(theta / math.pi, 0.0), 1.0)
        fx, fy = u * self.width - 0.5, v * self.height - 0.5
        x0, y0 = math.floor(fx), math.floor(fy)
        tx, ty = fx - x0, fy - y0
        out = [0.0, 0.0, 0.0]
        for corner_y, weight_y in ((y0, 1.0 - ty), (y0 + 1, ty)):
            for corner_x, weight_x in ((x0, 1.0 - tx), (x0 + 1, tx)):
                texel = self._texel(int(corner_x), int(corner_y))
                weight = weight_x * weight_y
                out[0] += texel[0] * weight
                out[1] += texel[1] * weight
                out[2] += texel[2] * weight
        return out[0], out[1], out[2]


class AnalyticEnvironment:
    """解析环境：用来把"误差随环境频率怎么变"框出来（不依赖任何文件）。"""

    def __init__(self, kind: str):
        self.kind = kind
        if kind == "sun":                            # 单个窄亮斑（最高频）
            length = math.sqrt(0.30 ** 2 + 0.55 ** 2 + 0.78 ** 2)
            self.sun = (0.30 / length, 0.55 / length, 0.78 / length)

    def sample(self, dx: float, dy: float, dz: float) -> tuple[float, float, float]:
        if self.kind == "constant":
            return 1.0, 1.0, 1.0
        if self.kind == "gradient":                  # 天空渐变（低频）
            t = 0.5 * (dy + 1.0)
            return 0.3 + 0.7 * t, 0.35 + 0.6 * t, 0.45 + 0.5 * t
        if self.kind == "cosine_lobe":               # 宽余弦瓣（中频）
            dot = 0.2 * dx + 0.9 * dy + 0.39 * dz
            value = max(dot, 0.0) ** 4
            return 4.0 * value, 3.6 * value, 3.2 * value
        if self.kind == "sun":                       # 窄亮斑：exp 峰
            dot = dx * self.sun[0] + dy * self.sun[1] + dz * self.sun[2]
            value = math.exp(-((1.0 - dot) / 0.002))
            return 120.0 * value + 0.05, 110.0 * value + 0.05, 95.0 * value + 0.05
        raise ValueError(self.kind)


# ---------------------------------------------------------------------------
# 镜像引擎的两半
# ---------------------------------------------------------------------------


def prefiltered_environment(env, r_direction, roughness: float, samples: int) -> float:
    """`generator/prefilterEnvMap.comp` 的镜像：V = N = R，NDF 采样，按 NdotL 加权。"""
    nx, ny, nz = r_direction
    total = 0.0
    weight_sum = 0.0
    for i in range(samples):
        xi_x, xi_y = hammersley(i, samples)
        hx, hy, hz = importance_sample_ggx(xi_x, xi_y, roughness)
        # 把局部坐标架的 h 转到以 n 为 z 的框架里（与 prefilterEnvMap.comp 的
        # up/tangent/bitangent 构造一致）
        up = (0.0, 0.0, 1.0) if abs(nz) < 0.999 else (1.0, 0.0, 0.0)
        tx = ny * up[2] - nz * up[1]
        ty = nz * up[0] - nx * up[2]
        tz = nx * up[1] - ny * up[0]
        tangent_length = math.sqrt(tx * tx + ty * ty + tz * tz)
        tx, ty, tz = tx / tangent_length, ty / tangent_length, tz / tangent_length
        bx = ny * tz - nz * ty
        by = nz * tx - nx * tz
        bz = nx * ty - ny * tx
        wx = tx * hx + bx * hy + nx * hz
        wy = ty * hx + by * hy + ny * hz
        wz = tz * hx + bz * hy + nz * hz
        length = math.sqrt(wx * wx + wy * wy + wz * wz)
        wx, wy, wz = wx / length, wy / length, wz / length
        # V = N = R：l = reflect(-V, h) = 2 (V·h) h - V
        v_dot_h = nx * wx + ny * wy + nz * wz
        lx = 2.0 * v_dot_h * wx - nx
        ly = 2.0 * v_dot_h * wy - ny
        lz = 2.0 * v_dot_h * wz - nz
        n_dot_l = nx * lx + ny * ly + nz * lz
        if n_dot_l <= 0.0:
            continue
        radiance = env.sample(lx, ly, lz)
        luminance = 0.2126 * radiance[0] + 0.7152 * radiance[1] + 0.0722 * radiance[2]
        total += luminance * n_dot_l
        weight_sum += n_dot_l
    return total / weight_sum if weight_sum > 0.0 else 0.0


def split_sum(env, normal, view, roughness: float, f0: float, lut_samples: int,
              prefilter_samples: int) -> tuple[float, float, float, float]:
    """返回 (split-sum 估计, 预过滤环境亮度, LUT 的 A, LUT 的 B)。"""
    nx, ny, nz = normal
    vx, vy, vz = view
    # R = reflect(-V, N)
    v_dot_n = vx * nx + vy * ny + vz * nz
    rx = 2.0 * v_dot_n * nx - vx
    ry = 2.0 * v_dot_n * ny - vy
    rz = 2.0 * v_dot_n * nz - vz
    prefiltered = prefiltered_environment(env, (rx, ry, rz), roughness, prefilter_samples)
    a_term, b_term = integrate_brdf(max(v_dot_n, 1.0e-4), roughness, k_ibl, lut_samples)
    return prefiltered * (f0 * a_term + b_term), prefiltered, a_term, b_term


def reference(env, normal, view, roughness: float, f0: float, samples: int) -> float:
    """逐样本参考：对完整 BRDF（引擎的 IBL 几何项 + Schlick Fresnel）积分。

    用与 LUT 相同的 NDF 采样（pdf(l) = D(h)(n·h)/(4(v·h))），所以两边的**采样策略一致**，
    差别只来自 split-sum 的那两步近似。
    """
    nx, ny, nz = normal
    vx, vy, vz = view
    cos_v = max(vx * nx + vy * ny + vz * nz, 1.0e-6)
    # 以 n 为 z 轴的局部坐标架
    up = (0.0, 0.0, 1.0) if abs(nz) < 0.999 else (1.0, 0.0, 0.0)
    tx = ny * up[2] - nz * up[1]
    ty = nz * up[0] - nx * up[2]
    tz = nx * up[1] - ny * up[0]
    length = math.sqrt(tx * tx + ty * ty + tz * tz)
    tx, ty, tz = tx / length, ty / length, tz / length
    bx = ny * tz - nz * ty
    by = nz * tx - nx * tz
    bz = nx * ty - ny * tx
    # 视线在局部坐标架里的分量
    local_vx = vx * tx + vy * ty + vz * tz
    local_vy = vx * bx + vy * by + vz * bz
    local_vz = cos_v
    total = 0.0
    for i in range(samples):
        xi_x, xi_y = hammersley(i, samples)
        hx, hy, hz = importance_sample_ggx(xi_x, xi_y, roughness)
        v_dot_h = local_vx * hx + local_vy * hy + local_vz * hz
        if v_dot_h <= 0.0:
            continue
        lx = 2.0 * v_dot_h * hx - local_vx
        ly = 2.0 * v_dot_h * hy - local_vy
        lz = 2.0 * v_dot_h * hz - local_vz
        if lz <= 0.0:
            continue
        d = distribution_ggx(hz, roughness)
        g = geometry_schlick_ggx_ibl(local_vz, roughness) * geometry_schlick_ggx_ibl(lz, roughness)
        f = f0 + (1.0 - f0) * (max(1.0 - v_dot_h, 0.0) ** 5)
        pdf = d * hz / (4.0 * v_dot_h)
        if pdf <= 0.0:
            continue
        world_lx = tx * lx + bx * ly + nx * lz
        world_ly = ty * lx + by * ly + ny * lz
        world_lz = tz * lx + bz * ly + nz * lz
        radiance = env.sample(world_lx, world_ly, world_lz)
        luminance = 0.2126 * radiance[0] + 0.7152 * radiance[1] + 0.0722 * radiance[2]
        total += (f * d * g / (4.0 * cos_v * lz)) * lz / pdf * luminance
    return total / samples


# ---------------------------------------------------------------------------


def environment_mean_luminance(env, samples: int = 4096) -> float:
    """环境的平均亮度（方向上的均匀采样），用作误差的归一化尺度。

    为什么需要它：有些配置下逐样本参考本身接近 0（例如窄瓣环境 + 视线背着亮斑），
    此时"相对误差"会爆炸成几百个百分点，但绝对量级小到没有意义。
    实测：cosine 环境 θv=60°、roughness 0.10 时参考值只有环境均值的 **0.06%**，
    1024 采样下相对误差 521%，32768 采样后掉到 1.7%——那是估计量噪声，不是近似误差。
    """
    total = 0.0
    golden_angle = math.pi * (3.0 - math.sqrt(5.0))
    for i in range(samples):
        y = 1.0 - 2.0 * (i + 0.5) / samples
        radius = math.sqrt(max(1.0 - y * y, 0.0))
        phi = i * golden_angle
        colour = env.sample(radius * math.cos(phi), y, radius * math.sin(phi))
        total += 0.2126 * colour[0] + 0.7152 * colour[1] + 0.0722 * colour[2]
    return total / samples


def view_direction(theta_deg: float) -> tuple[float, float, float]:
    """在 xz 平面里从法线（+z）转开 theta 的视线方向。"""
    theta = math.radians(theta_deg)
    return math.sin(theta), 0.0, math.cos(theta)


def main(argv: list[str]) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--samples", type=int, default=8192, help="逐样本参考的采样数（默认 8192）")
    parser.add_argument("--prefilter-samples", type=int, default=4096, help="预过滤的采样数（默认 4096）")
    parser.add_argument("--lut-samples", type=int, default=4096, help="LUT 的采样数（默认 4096）")
    parser.add_argument("--skip-hdr", action="store_true", help="跳过真实 HDRI，只跑解析环境")
    parser.add_argument("--hdr", default=str(HDR_PATH))
    parser.add_argument(
        "--meaningful-fraction", type=float, default=0.05,
        help="参考值达到本配置信号量级该比例以上，其相对误差才计入判定（默认 0.05）",
    )
    parser.add_argument(
        "--prefilter-floor-fraction", type=float, default=0.05,
        help="预过滤亮度不低于环境平均亮度的该比例，本配置才计入判定（默认 0.05）。"
             "**缺了这条绝对下限就会把噪声当成结论**：镜面瓣指向环境暗处时预过滤与参考"
             "都趋近 0，两个趋零的数相除照样能过 --meaningful-fraction（该门限的两边同时"
             "趋零），实测 cosine/α=0.0025 得到 2.85~5.59 倍的假误差，换采样数就变",
    )
    parser.add_argument(
        "--tolerance", type=float, default=0.25,
        help="真实 HDRI 上可接受的相对误差上限（默认 0.25；门限来自实测，见 case 记录）",
    )
    parser.add_argument(
        "--emit-glsl", default=None,
        help="额外把「相对误差 × roughness」写成 VL 里可看的表（M_brdfPlot mode 7 用），"
             "例：shader/glsl/validationSplitSumTable.glsl",
    )
    parser.add_argument(
        "--emit-samples", type=int, default=4096,
        help="--emit-glsl 时每个配置的采样数（默认 4096；表里的数字会跟着变，"
             "记录中的结论数字必须在同一采样数下复核）",
    )
    args = parser.parse_args(argv)

    # (短名, 显示名, 环境)。短名是 ASCII：表里按 arm 索引存，短名就是"arm N 是谁"的唯一依据，
    # 图上的图例必须用它（显示名带中文与全角括号，没法画进自带的 5x7 位图字体）。
    environments: list[tuple[str, str, object]] = [
        ("constant", "constant（对照组）", AnalyticEnvironment("constant")),
        ("gradient", "gradient  低频", AnalyticEnvironment("gradient")),
        ("cosine", "cosine    中频", AnalyticEnvironment("cosine_lobe")),
        ("sun", "sun       高频", AnalyticEnvironment("sun")),
    ]
    if not args.skip_hdr:
        width, height, pixels = load_radiance_hdr(Path(args.hdr))
        print(f"真实环境: {args.hdr}")
        print(f"  Radiance RGBE {width}x{height}（采样约定镜像 equirectToCubemap.comp）")
        environments.append(
            ("bistro4k", "bistro 4k HDRI（真实）", EquirectEnvironment(width, height, pixels))
        )

    normal = (0.0, 0.0, 1.0)
    roughness_list = [0.15, 0.4, 0.7]
    view_angles = [0.0, 60.0]
    f0_list = [("conductor F0=1.00", 1.0), ("dielectric F0=0.04", 0.04)]

    failures: list[str] = []
    print(f"\nsplit-sum vs 逐样本参考（参考 {args.samples} 样本 / 预过滤 {args.prefilter_samples}）")
    print("误差归一化：除以**本配置的信号量级** = 预过滤亮度 × (F0·A + B)")
    print("   （比除以环境均值更合理：电介质的镜面信号本来就只有环境均值的百分之几，"
          "用环境均值会把它的可判定配置全部滤掉）")
    print(f"相对误差仅在参考值 >= 信号量级的 {args.meaningful_fraction*100:.0f}% 时计入判定")
    print(f"且预过滤亮度 >= 环境平均亮度的 {args.prefilter_floor_fraction*100:.0f}%"
          f"（否则镜面信号本身趋零，比值没有意义）")
    for env_key, label, env in environments:
        env_mean = environment_mean_luminance(env)
        print(f"\n--- 环境：{label}（平均亮度 {env_mean:.4f}）---")
        for f0_name, f0 in f0_list:
            worst_scaled = (0.0, 0.0, 0.0)
            worst_relative = (0.0, 0.0, 0.0)
            measured = 0
            skipped = 0
            for roughness in roughness_list:
                for theta in view_angles:
                    view = view_direction(theta)
                    estimate, prefiltered, a_term, b_term = split_sum(
                        env, normal, view, roughness, f0, args.lut_samples, args.prefilter_samples)
                    truth = reference(env, normal, view, roughness, f0, args.samples)
                    if prefiltered < args.prefilter_floor_fraction * env_mean:
                        # 绝对下限：镜面瓣指向环境暗处时两个数都趋零，比值不是误差而是噪声。
                        skipped += 1
                        continue
                    scale = max(prefiltered * (f0 * a_term + b_term), 1.0e-9)
                    scaled = abs(estimate - truth) / scale
                    if scaled > worst_scaled[0]:
                        worst_scaled = (scaled, roughness, theta)
                    if truth >= args.meaningful_fraction * scale:
                        measured += 1
                        relative = abs(estimate - truth) / truth
                        if relative > worst_relative[0]:
                            worst_relative = (relative, roughness, theta)
                    else:
                        skipped += 1
            print(f"   {f0_name}: 误差/信号量级 最大 {worst_scaled[0]*100:6.2f}%"
                  f"  @ r={worst_scaled[1]:.2f} θv={worst_scaled[2]:.0f}°"
                  f" | 相对误差 最大 {worst_relative[0]*100:6.2f}%"
                  f"  @ r={worst_relative[1]:.2f} θv={worst_relative[2]:.0f}°"
                  f"（计入 {measured} 个配置，参考过小跳过 {skipped} 个）")
            if label.startswith("bistro") and measured and worst_relative[0] > args.tolerance:
                failures.append(
                    f"真实 HDRI 上 {f0_name} 的 split-sum 相对误差 {worst_relative[0]*100:.2f}% "
                    f"超过门限 {args.tolerance*100:.0f}%"
                )
        if label.startswith("constant"):
            # 对照组：环境恒为 1 时 split-sum 应当**精确**等于参考（两半各自就是 A/B）。
            # 实测噪声地板：1024 采样 1.21%、4096 采样 0.056%、65536 采样 0.001%，
            # 所以判据取 1% 只在采样数够大时才有意义——这里按 --samples 直接复用。
            observed = 0.0
            for roughness in roughness_list:
                for theta in view_angles:
                    view = view_direction(theta)
                    estimate, _, _, _ = split_sum(env, normal, view, roughness, 1.0,
                                                  args.lut_samples, args.prefilter_samples)
                    truth = reference(env, normal, view, roughness, 1.0, args.samples)
                    observed = max(observed, abs(estimate - truth) / max(truth, 1.0e-6))
            print(f"   -> 对照组自检：环境恒为 1 时最大相对误差 {observed*100:.3f}%"
                  f"（判据 <= 1%；这一项不为 0 说明镜像或采样数有问题，而不是近似误差）")
            if observed > 0.01:
                failures.append(f"恒定环境对照组误差 {observed*100:.3f}% 过大（镜像有问题）")

    print()
    if failures:
        for line in failures:
            print(f"不通过: {line}")
        return 1

    # ---- ⑤ 生成 VL 里可看的表（mode 7） ----
    if args.emit_glsl is not None:
        emit_path = Path(args.emit_glsl)
        # 图上扫的是 roughness（x 轴），所以表按 roughness 密集取样；θv 固定在 60°：
        # 掠射侧误差最大、也最能体现"环境频率越高、误差越大"，0° 那条基本贴着噪声地板。
        emit_roughness = [0.05, 0.15, 0.25, 0.40, 0.50, 0.60, 0.70, 0.85, 1.00]
        emit_view_deg = 60.0
        # 图上不画 gradient：它与 constant 的误差同量级（最大都是 0.05%~0.15%），
        # 在 6 个数量级的 log 轴上两条曲线相差不到 3 px——逐列像素测量无法把两条描边
        # 分开（会把**每一列**都判成"曲线交叉"而整体丢弃）。低频环境只留 constant 作代表，
        # gradient 的数字仍在上面 ① 的判定表里。
        emit_skip_envs = {"gradient"}
        emit_environments = [
            (key, label, env) for key, label, env in environments if key not in emit_skip_envs
        ]
        view = view_direction(emit_view_deg)
        print(f"\n⑤ 生成 mode 7 的表（{len(emit_environments)} 环境 × {len(emit_roughness)} roughness，"
              f"F0=1、θv={emit_view_deg:.0f}°，每配置 {args.emit_samples} 样本；"
              f"图上略过 {', '.join(sorted(emit_skip_envs))}）……")
        table: list[list[float]] = []
        env_keys: list[str] = []
        for env_key, label, env in emit_environments:
            env_keys.append(env_key)
            env_mean = environment_mean_luminance(env)
            row: list[float] = []
            for roughness in emit_roughness:
                estimate, prefiltered, a_term, b_term = split_sum(
                    env, normal, view, roughness, 1.0, args.lut_samples, args.prefilter_samples)
                truth = reference(env, normal, view, roughness, 1.0, args.emit_samples)
                scale = max(prefiltered * (a_term + b_term), 1.0e-9)
                # 与判定同一套门限（相对 + 绝对两条），任一不满足就记 -1，
                # shader 见到 -1 就断开曲线——**不画**比画一个假的小值诚实。
                if (prefiltered < args.prefilter_floor_fraction * env_mean
                        or truth < args.meaningful_fraction * scale):
                    row.append(-1.0)
                else:
                    row.append(abs(estimate - truth) / max(truth, 1.0e-6))
            table.append(row)
            print(f"      {label}: " + " ".join(
                "  n/a " if value < 0.0 else f"{value*100:5.2f}%" for value in row))

        lines = [
            "// 自动生成，请勿手改：tool/validation/verify_split_sum.py --emit-glsl",
            "//",
            "// D-09 split-sum 相对误差表（Karis 2013 §5）：同一环境、同一 G 支（IBL k=α/2）下，",
            "// split-sum 估计与逐样本参考积分之比的相对误差，扫 roughness、F0=1、θv=60°。",
            "// 索引顺序：[env][roughness]；-1 = 该配置参考值太小、相对误差无意义（图上断开）。",
            f"// 采样：参考 {args.emit_samples} / 预过滤 {args.prefilter_samples} / LUT {args.lut_samples}。",
            "#ifndef VL_VALIDATION_SPLIT_SUM_TABLE_GLSL",
            "#define VL_VALIDATION_SPLIT_SUM_TABLE_GLSL",
            "",
            f"const int PLOT_SPLITSUM_ENV_COUNT = {len(emit_environments)};",
            f"const int PLOT_SPLITSUM_ROUGHNESS_COUNT = {len(emit_roughness)};",
            "const float PLOT_SPLITSUM_ROUGHNESS[%d] = float[](%s);"
            % (len(emit_roughness), ", ".join(f"{value:.2f}" for value in emit_roughness)),
            f"const float PLOT_SPLITSUM_VIEW_DEG = {emit_view_deg:.1f};",
        ]
        for index, env_key in enumerate(env_keys):
            lines.append(f"// arm {index} = {env_key}")
        lines.append(f"const float PLOT_SPLITSUM_TABLE[{len(table) * len(emit_roughness)}] = float[](")
        # 注意：GLSL 的初始化列表**不允许尾随逗号**（C++ 允许），最后一行后面不能再有逗号，
        # 否则 glslang 报 "syntax error, unexpected RIGHT_PAREN"。这里显式拼逗号。
        for index, row in enumerate(table):
            suffix = "," if index + 1 < len(table) else ""
            lines.append("    " + ", ".join(f"{value:.5f}" for value in row) + suffix)
        lines.append(");")
        lines.append("")
        lines.append("#endif")
        lines.append("")
        emit_path.write_text("\n".join(lines), encoding="utf-8")
        print(f"      已写出 {emit_path}")
        print(f"      arm 顺序: {', '.join(env_keys)}")

    print("判定: 通过（split-sum 误差已量化；恒定环境对照组自洽）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
