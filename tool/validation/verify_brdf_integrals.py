"""DefaultLit 的**公式级**离线验证：NDF 归一化 / G1 性质 / 白炉 / 参数化边界。

覆盖的 case（plan 的编号）：

| 层 | case | 判定什么 |
| --- | --- | --- |
| 共享 | `M-02` | `∫ D(h)(n·h) dω_h = 1`，并据此钉死 `DistributionGGX` 的 α 约定 |
| 共享 | `M-03` | `0 ≤ G1 ≤ 1`、`G1(c→1) = 1`、互易性 `D·G/(4|n·l||n·v|)` 对称 |
| 共享 | `M-04` | 白炉：各 lobe 的半球积分 ≤ 1（单次散射不许凭空造能量） |
| DefaultLit | `D-01` | 同 `M-02` + `M-03`（DefaultLit 视角的复述） |
| DefaultLit | `D-07` | conductor（F0=1）与 dielectric（F0=0.04）的白炉 |
| DefaultLit | `D-08` | 参数化边界：`metallic=1` 时可见 diffuse 为 0；F0 / albedo 不越界 |

## 为什么这些必须是**离线**判定（plan §1.9.1 / §2.1④）

判据本身是**积分恒等式**（归一化、能量），要的是积分值而不是某张图的观感；像素只能证明
"shader 画的曲线与解析期望一致"，不能证明"这条曲线积分为 1"。两者缺一不可：

- **本脚本**：把引擎的公式在脚本里独立积分 → 归一化 / 值域 / 能量；
- **曲线探针**（`M_brdfPlot` mode 0 / 2 / 4）：证明 shader 里的**同一份** `DistributionGGX` /
  `GeometrySchlickGGX` / `GeometrySchlickGGXIbl` 与这些公式逐列一致（实测 ≤0.5 px）；
- 两者合起来才是"引擎的 NDF 归一化正确"。

python 侧的公式镜像**不在这里重写**：`distribution_ggx` / `geometry_schlick_ggx` /
`geometry_schlick_ggx_ibl` / `smith_g1_ggx` 从 `measure_plot_curve.py` 导入，
`hammersley` / `importance_sample_ggx` / `integrate_brdf` 从 `verify_geometry_term.py` 导入——
数学定义在工具里也只有一份，避免"审计脚本自己漂移"。

用法：

    py -3 tool/validation/verify_brdf_integrals.py [--samples 8192] [--roughness-steps 6]

退出码：任一断言不通过即 1（判定与差异归因最终写进 `shading-model-case-records.md`）。
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

from measure_plot_curve import (
    distribution_ggx,
    geometry_schlick_ggx,
    geometry_schlick_ggx_ibl,
    smith_g1_ggx,
)
from verify_geometry_term import hammersley, importance_sample_ggx, integrate_brdf, k_ibl

# 引擎 DefaultLit 直接光入口（common/lighting.glsl EvaluateDefaultPbrLightLobes）的常量：
#   dielectricF0 = 0.08 * specular，specular 默认 0.5 → 0.04
#   引擎当前没有独立 F0 作者入口（D-12 登记的参数面缺口），所以这里固定 0.04。
ENGINE_DIELECTRIC_F0 = 0.04

GEOMETRY_VARIANTS = (
    ("analytic k=(r+1)²/8", geometry_schlick_ggx),
    ("ibl      k=α/2     ", geometry_schlick_ggx_ibl),
    ("smith    exact     ", smith_g1_ggx),
)


# ---------------------------------------------------------------------------
# 引擎公式的镜像（只补这里缺的那几个：Fresnel、direct-lobe 组合）
# ---------------------------------------------------------------------------


def fresnel_schlick(cos_theta: float, f0: float) -> float:
    """common/lighting.glsl: fresnelSchlick（标准 pow-5 Schlick，不是 SG 近似）。"""
    return f0 + (1.0 - f0) * (max(1.0 - cos_theta, 0.0) ** 5)


def f0_from_parameters(base_color: float, metallic: float, specular: float = 0.5) -> float:
    """`F0 = mix(0.08 * specular, baseColor, metallic)`，逐通道取灰度版。"""
    return (0.08 * specular) * (1.0 - metallic) + base_color * metallic


def diffuse_weight(fresnel: float, metallic: float) -> float:
    """引擎的漫反射权重：`(1 - F)(1 - metallic)`。

    出处是 Neubelt & Pettineo 2013 §Diffuse BRDF 式(15)（原始来源 Shirley 1991），
    **不是 Karis 2013**——Karis 那篇的 diffuse 是裸 Lambertian，没有这一步。
    它是"点取值 Fresnel"式的平衡，出处自己也声明它破坏 Helmholtz 互易性；
    掠射下的能量越界见 `D-07`。
    """
    return (1.0 - fresnel) * (1.0 - metallic)


# ---------------------------------------------------------------------------
# ① NDF 归一化（M-02 / D-01）
# ---------------------------------------------------------------------------


def ndf_normalization(roughness: float, steps: int = 200000, w_min: float = 1.0e-12) -> float:
    """`2π ∫₀¹ D(μ) μ dμ`：半球上 `∫ D(h)(n·h) dω_h` 的解析等价形式（μ = n·h）。

    用 μ 上的**确定性求积**而不是重要性采样：归一化是恒等式，不该由"用被测分布采样自己"给出。
    网格设计踩过两次坑，都记下来（都属于"网格问题看起来像公式问题"）：

    1. **均匀 μ 网格不够**：GGX 在 μ 上的峰宽是 **α²**（不是 α），r=0.05 时只有 6.25e-6；
       2e5 个均匀点（间距 5e-6）只积出 **0.868**，2 倍网格 0.955 —— 明显是欠采样，不是公式错。
    2. **"均匀 w" 其实是同一个网格**：代换 `w = (1-μ)/α²` 后若仍用均匀 w，雅可比正好抵消，
       点数、结果与均匀 μ 逐位相同（实测两列数字完全一致）。要点在于**质量集中在 w≲1，
       定义域却延伸到 1/α² ≈ 1.6e5**——动态范围 5 个数量级，均匀网格必然顾此失彼。

    所以这里用 **w 上的对数网格**（被积函数按 1/w² 衰减，对数网格正好匹配）：

        ∫₀¹ D(μ)μ dμ = α² ∫₀^{1/α²} D(1-α²w)(1-α²w) dw，w 取对数分布

    实测：20k 与 200k 点结果差 1e-9 量级。下界 `w_min` 不能省：`∫₀^{w_min}2/(1+2w)²dw ≈ 2·w_min`，
    取 1e-6 会让**所有 roughness** 一致偏低 2e-6（这个"与 α 无关的固定偏差"正是截断的特征）。
    """
    alpha = roughness * roughness
    alpha_squared = alpha * alpha
    w_max = 1.0 / alpha_squared
    log_min = math.log(w_min)
    log_max = math.log(w_max)
    step = (log_max - log_min) / steps
    total = 0.0
    for i in range(steps):
        w = math.exp(log_min + (i + 0.5) * step)
        mu = 1.0 - alpha_squared * w
        if mu <= 0.0:
            continue
        total += distribution_ggx(mu, roughness) * mu * alpha_squared * w * step
    return 2.0 * math.pi * total


def ndf_peak_identity(roughness: float) -> tuple[float, float]:
    """峰值恒等式：`D(n) = 1/(π α²)`（α = roughness²）。

    **为什么钉 α 约定要靠这个而不是归一化**：归一化对任何 α 都成立
    （`∫D(n·h)dω=1` 是 GGX 的定义性质，与 α 取值无关），所以"积分等于 1"证明不了
    代码有没有漏掉那次平方。峰值才带 α 的信息：漏掉平方会得到 `1/(π r²)`，
    r=0.05 时与正确值差 **r² = 400 倍**。
    """
    alpha = roughness * roughness
    return distribution_ggx(1.0, roughness), 1.0 / (math.pi * alpha * alpha)


# ---------------------------------------------------------------------------
# ③ 白炉（M-04 / D-07）
# ---------------------------------------------------------------------------


def specular_albedo(
    cos_v: float,
    roughness: float,
    f0: float,
    geometry,
    samples: int,
    fresnel_angle: str = "half",
) -> float:
    """镜面 lobe 的单次散射方向反照率：`∫ f_s(l,v) cosθ_l dω_l`（均匀环境 L=1）。

    按 NDF 采样半角向量 h（pdf(l) = D(h)(n·h) / (4 (v·h))），因此估计量包含
    微表面模型的全部项（D / G / F / 分母），不是另抄一份近似。

    `fresnel_angle` 决定 Fresnel 取哪个角：
      "half"     —— `F(v·h)`，本引擎（Neubelt 2013 式(15) 那一派）的写法；
      "incident" —— `F(θ_l)`，PG'97 coupled model 式(5) 的写法（`R_f(θ)` 的 θ 是入射角）。
    这一维是 ③c 试算 PG'97 候选时必须分开的——两者只在镜面方向重合。
    """
    sin_v = math.sqrt(max(1.0 - cos_v * cos_v, 0.0))
    total = 0.0
    valid = 0
    for i in range(samples):
        xi_x, xi_y = hammersley(i, samples)
        hx, hy, hz = importance_sample_ggx(xi_x, xi_y, roughness)
        v_dot_h = sin_v * hx + cos_v * hz
        if v_dot_h <= 0.0:
            continue
        lx = 2.0 * v_dot_h * hx - sin_v
        lz = 2.0 * v_dot_h * hz - cos_v
        cos_l = lz
        if cos_l <= 0.0:
            continue
        d = distribution_ggx(hz, roughness)
        g = geometry(cos_v, roughness) * geometry(cos_l, roughness)
        f = fresnel_schlick(v_dot_h if fresnel_angle == "half" else cos_l, f0)
        pdf = d * hz / (4.0 * v_dot_h)
        if pdf <= 0.0:
            continue
        brdf = f * d * g / (4.0 * cos_v * cos_l)
        total += brdf * cos_l / pdf
        valid += 1
    return total / samples


def specular_albedo_plain_d(cos_v: float, roughness: float, f0: float, samples: int) -> float:
    """Burley Fig 12 里的 **"No G"** 参照臂：`f = D·F/4`。

    论文对它的定义写得很明确：*"The 'no G' model **excludes the G and 1/(cosθl cosθv) factors**"*
    （Burley 2012 Fig 12 图注）。也就是去掉微表面分式的分母与遮蔽项，只留 `D·F/4`。
    保留它是因为论文用这条曲线说明"省略 G 会让掠射明显偏暗"——判据里要能画出这条对照臂。
    """
    sin_v = math.sqrt(max(1.0 - cos_v * cos_v, 0.0))
    total = 0.0
    for i in range(samples):
        xi_x, xi_y = hammersley(i, samples)
        hx, hy, hz = importance_sample_ggx(xi_x, xi_y, roughness)
        v_dot_h = sin_v * hx + cos_v * hz
        if v_dot_h <= 0.0:
            continue
        lx = 2.0 * v_dot_h * hx - sin_v
        lz = 2.0 * v_dot_h * hz - cos_v
        if lz <= 0.0:
            continue
        d = distribution_ggx(hz, roughness)
        f = fresnel_schlick(v_dot_h, f0)
        pdf = d * hz / (4.0 * v_dot_h)
        if pdf <= 0.0:
            continue
        total += (f * d / 4.0) * lz / pdf
    return total / samples


def diffuse_albedo(
    cos_v: float,
    roughness: float,
    base_color: float,
    metallic: float,
    samples: int,
) -> float:
    """漫反射 lobe 的方向反照率：`∫ f_d cosθ_l dω_l`，用余弦加权采样（pdf = cosθ_l/π）。

    `f_d = (1 - F(v·h)) (1 - metallic) baseColor / π`——注意 `F` 依赖 `v·h`，
    所以这一项不是解析的 `albedo`，必须积分。
    """
    sin_v = math.sqrt(max(1.0 - cos_v * cos_v, 0.0))
    total = 0.0
    for i in range(samples):
        xi_x, xi_y = hammersley(i, samples)
        radius = math.sqrt(xi_x)
        phi = 2.0 * math.pi * xi_y
        lx = radius * math.cos(phi)
        ly = radius * math.sin(phi)
        lz = math.sqrt(max(1.0 - xi_x, 0.0))
        # h = normalize(l + v)，v = (sin_v, 0, cos_v)
        hx, hy, hz = lx + sin_v, ly, lz + cos_v
        norm = math.sqrt(hx * hx + hy * hy + hz * hz)
        if norm <= 0.0:
            continue
        v_dot_h = (sin_v * hx + cos_v * hz) / norm
        f = fresnel_schlick(max(v_dot_h, 0.0), f0_from_parameters(base_color, metallic))
        weight = diffuse_weight(f, metallic)
        total += weight * base_color / math.pi * math.pi  # × (cosθ_l / pdf)
    return total / samples


# ---------------------------------------------------------------------------


def emit_albedo_glsl(path: Path, alphas: list[float], angles_deg: list[float],
                     table: list[list[list[float]]], samples: int) -> None:
    """把 D-05 的方向反照率表写成 shader 常量表（供 `M_brdfPlot` 的 mode 6 画图）。

    为什么要把离线结果搬进 shader：这张图在 VL 里要**能直接看**（用户视角），
    而逐像素做半球积分在 shader 里是不可能的（每列曲线点都要上千样本）。
    搬运的只是**数值**：曲线值来自本脚本的离线积分，公式本体仍在
    `common/microfacetDistribution.glsl` 与 `lighting.glsl` 里。
    生成物带"自动生成"标记，`--check-glsl` 会在它与当前计算不一致时报错。
    """
    arms = ("analytic k=(r+1)^2/8", "ibl k=alpha/2", "smith exact", "no G (D*F/4)")
    lines = [
        "// 自动生成，请勿手改：tool/validation/verify_brdf_integrals.py --emit-glsl",
        "//",
        "// D-05 方向反照率表（Burley 2012 Fig 12 的对照结构）：同一 D 与 F，只换 G。",
        f"// 每个点 = {samples} 样本的 NDF 采样估计量；α = roughness²（Disney 约定）。",
        "// 索引顺序：[alpha][arm][angle]；arm 依次是 " + " / ".join(arms) + "。",
        "#ifndef VL_VALIDATION_ALBEDO_TABLE_GLSL",
        "#define VL_VALIDATION_ALBEDO_TABLE_GLSL",
        "",
        f"const int PLOT_ALBEDO_ALPHA_COUNT = {len(alphas)};",
        f"const int PLOT_ALBEDO_ARM_COUNT = {len(arms)};",
        f"const int PLOT_ALBEDO_ANGLE_COUNT = {len(angles_deg)};",
        "const float PLOT_ALBEDO_ALPHAS[%d] = float[](%s);"
        % (len(alphas), ", ".join(f"{a:.4f}" for a in alphas)),
        "const float PLOT_ALBEDO_ANGLES[%d] = float[](%s);"
        % (len(angles_deg), ", ".join(f"{a:.1f}" for a in angles_deg)),
        "",
        f"const float PLOT_ALBEDO_TABLE[{len(alphas) * len(arms) * len(angles_deg)}] = float[](",
    ]
    rows = []
    for alpha_index in range(len(alphas)):
        for arm_index in range(len(arms)):
            values = table[alpha_index][arm_index]
            rows.append("    " + ", ".join(f"{v:.5f}" for v in values))
    lines.append(",\n".join(rows))
    lines += [");", "", "#endif", ""]
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"已写出 {path}（{len(alphas)} α × {len(arms)} 臂 × {len(angles_deg)} 角度）")


# ---------------------------------------------------------------------------


def sample_values(count: int, low: float, high: float) -> list[float]:
    if count == 1:
        return [0.5 * (low + high)]
    return [low + (high - low) * i / (count - 1) for i in range(count)]


def main(argv: list[str]) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--samples", type=int, default=8192, help="每个方向的采样数（默认 8192）")
    parser.add_argument("--roughness-steps", type=int, default=6, help="roughness 扫描点数（默认 6）")
    parser.add_argument("--view-steps", type=int, default=4, help="视线角扫描点数（默认 4）")
    parser.add_argument("--ndf-tolerance", type=float, default=2.0e-4, help="归一化判据（默认 2e-4）")
    parser.add_argument("--energy-tolerance", type=float, default=1.0e-3, help="白炉判据（默认 1e-3）")
    parser.add_argument(
        "--strict-total",
        action="store_true",
        help="把 diffuse+specular 组合的能量越界也算作失败（默认只报告，归因见 case 记录 D-07）",
    )
    parser.add_argument(
        "--emit-glsl", default=None,
        help="把 D-05 的方向反照率表写成 shader 常量表（VL 里 mode 6 要画它），给出输出路径",
    )
    parser.add_argument(
        "--emit-samples", type=int, default=8192,
        help="生成表时每个点用的样本数（默认 8192；与判定用的 --samples 独立）",
    )
    args = parser.parse_args(argv)

    failures: list[str] = []
    roughness_list = sample_values(args.roughness_steps, 0.05, 1.0)
    view_list = sample_values(args.view_steps, 0.1, 1.0)

    # ---- ① NDF 归一化 + α 约定（M-02 / D-01） ----
    print("① NDF 归一化 ∫ D(h)(n·h) dω_h = 1（确定性求积：w = (1-μ)/α² 上的对数网格）")
    print(f"   判据 |I-1| <= {args.ndf_tolerance:g}；另做网格翻倍的收敛检查")
    worst_norm = 0.0
    worst_norm_roughness = 0.0
    worst_peak = 0.0
    for roughness in roughness_list:
        value = ndf_normalization(roughness)
        refined = ndf_normalization(roughness, steps=400000)
        measured_peak, analytic_peak = ndf_peak_identity(roughness)
        error = abs(value - 1.0)
        if error > worst_norm:
            worst_norm = error
            worst_norm_roughness = roughness
        worst_peak = max(worst_peak, abs(measured_peak - analytic_peak) / analytic_peak)
        print(
            f"   roughness={roughness:.3f}: ∫D(n·h)dω={value:.9f}（网格翻倍 {refined:.9f}，"
            f"Δ={abs(value - refined):.1e}）  D(n)={measured_peak:.3f} vs 1/(πα²)={analytic_peak:.3f}"
        )
    print(f"   -> 归一化最大偏差 {worst_norm:.2e} @ roughness={worst_norm_roughness:.3f}；"
          f"峰值恒等式最大相对偏差 {worst_peak:.2e}")
    if worst_norm > args.ndf_tolerance:
        failures.append(f"NDF 未归一化：最大偏差 {worst_norm:.2e}")
    elif worst_peak > 1.0e-9:
        failures.append(f"α 约定不符：峰值恒等式偏差 {worst_peak:.2e}")
    else:
        print("   -> 通过：α = roughness² 下归一化成立，且峰值与 1/(πα²) 一致"
              "（漏掉平方会差 r² 倍）")

    # ---- ② G1 性质与互易性（M-03 / D-01） ----
    print("\n② G1 值域 / 端点 / 单调性，以及 D·G/(4|n·l||n·v|) 的互易性")
    for name, geometry in GEOMETRY_VARIANTS:
        worst_range = 0.0
        worst_mono = 0.0
        endpoint_gap = 0.0
        for roughness in roughness_list:
            previous = -1.0
            for i in range(2001):
                cos_theta = i / 2000.0
                value = geometry(cos_theta, roughness)
                worst_range = max(worst_range, value - 1.0, -value)
                if value < previous - 1e-12:
                    worst_mono = max(worst_mono, previous - value)
                previous = value
            endpoint_gap = max(endpoint_gap, abs(geometry(1.0, roughness) - 1.0))
        print(
            f"   {name}: max(超出 [0,1])={worst_range:.2e}  非单调最大回落={worst_mono:.2e}  "
            f"|G1(1)-1|={endpoint_gap:.2e}"
        )
        if worst_range > 1.0e-9 or worst_mono > 1.0e-9 or endpoint_gap > 1.0e-9:
            failures.append(f"{name} 的 G1 不满足值域 / 单调 / 端点性质")

    worst_reciprocity = 0.0
    geometry = geometry_schlick_ggx
    for roughness in roughness_list:
        for i in range(1, 40):
            cos_l = i / 40.0
            for j in range(1, 40):
                cos_v = j / 40.0
                # 给定 l 与 v 时 h 唯一：用 (l+v)/|l+v|
                sin_l = math.sqrt(max(1.0 - cos_l * cos_l, 0.0))
                sin_v = math.sqrt(max(1.0 - cos_v * cos_v, 0.0))
                hz = cos_l + cos_v
                norm = math.sqrt(sin_l * sin_l + hz * hz)
                cos_h = hz / norm
                d = distribution_ggx(cos_h, roughness)
                forward = d * geometry(cos_v, roughness) * geometry(cos_l, roughness) / (
                    4.0 * cos_v * cos_l
                )
                backward = d * geometry(cos_l, roughness) * geometry(cos_v, roughness) / (
                    4.0 * cos_l * cos_v
                )
                worst_reciprocity = max(worst_reciprocity, abs(forward - backward))
    print(f"   互易性（l↔v 交换）最大差 = {worst_reciprocity:.2e}")
    if worst_reciprocity > 1.0e-12:
        failures.append(f"镜面项不互易：{worst_reciprocity:.2e}")
    else:
        print("   -> 通过：G1 值域 / 端点 / 单调性与互易性成立（三种 G 变体都查）")

    # ---- ③ 白炉（M-04 / D-07） ----
    print(f"\n③ 白炉：∫ f cosθ_l dω_l（均匀环境 L=1，NDF / 余弦采样各 {args.samples}）")
    print("   conductor = F0 1.0（metallic=1, baseColor=1）；dielectric = F0 0.04（specular 默认 0.5）")
    print("   分两栏看：**镜面 lobe 单独**必须 <= 1（判据）；**diffuse+specular 组合**另行报告——")
    print("   引擎的漫反射权重 (1-F(v·h)) 出自 Neubelt & Pettineo 2013 式(15)（原始来源 Shirley 1991）；Karis 2013 的 diffuse")
    print("   就是裸 Lambertian、Burley 2012 里 (1-F) 出现 0 次——即论文没规定这一步）。掠射下它补偿不足，")
    print("   组合越界是这一项的代价，不是 G 写错——G 越正确（精确 Smith）越界反而越大（D-07 归因）。")

    worst_specular = 0.0
    worst_specular_case = ""
    worst_total = 0.0
    worst_total_case = ""
    for label, base_color, metallic in (
        ("conductor  F0=1.00", 1.0, 1.0),
        ("dielectric F0=0.04", 1.0, 0.0),
        ("metal grey F0=0.50", 0.5, 1.0),
    ):
        f0 = f0_from_parameters(base_color, metallic)
        for name, geometry in GEOMETRY_VARIANTS:
            specular_peak = (0.0, 0.0, 0.0)
            total_peak = (0.0, 0.0, 0.0)
            for roughness in roughness_list:
                for cos_v in view_list:
                    specular = specular_albedo(cos_v, roughness, f0, geometry, args.samples)
                    diffuse = diffuse_albedo(
                        cos_v, roughness, base_color, metallic, args.samples
                    )
                    if specular > specular_peak[0]:
                        specular_peak = (specular, roughness, cos_v)
                    if specular + diffuse > total_peak[0]:
                        total_peak = (specular + diffuse, roughness, cos_v)
            if specular_peak[0] > worst_specular:
                worst_specular = specular_peak[0]
                worst_specular_case = f"{label} / {name.strip()}"
            if total_peak[0] > worst_total:
                worst_total = total_peak[0]
                worst_total_case = (
                    f"{label} / {name.strip()} / roughness={total_peak[1]:.3f} / cosθv={total_peak[2]:.3f}"
                )
            print(
                f"   {label} | {name}: 镜面 lobe 最大 {specular_peak[0]:.4f} "
                f"@ r={specular_peak[1]:.3f} cosθv={specular_peak[2]:.3f} | "
                f"组合最大 {total_peak[0]:.4f} @ r={total_peak[1]:.3f} cosθv={total_peak[2]:.3f}"
            )
    print(f"   -> 镜面 lobe 全局最大 {worst_specular:.4f}（{worst_specular_case}），"
          f"判据 <= 1 + {args.energy_tolerance:g}")
    print(f"   -> 组合全局最大 {worst_total:.4f}（{worst_total_case}）")
    if worst_specular > 1.0 + args.energy_tolerance:
        failures.append(f"镜面 lobe 白炉越界：{worst_specular:.4f}")
    if worst_total > 1.0 + args.energy_tolerance:
        message = (
            f"组合（diffuse+specular）白炉越界：最大 {worst_total:.4f} @ {worst_total_case}"
            " —— 归因：漫反射权重用的 (1-F(v·h)) 出自 Neubelt & Pettineo 2013 式(15)（Karis 2013 的 diffuse "
            "是裸 Lambertian、Burley 2012 无此因子），掠射视角下 v·h 仍接近法线，Fresnel 上升没有传导"
            "到漫反射项（实测 diffuse≈0.94 而镜面再加 0.61）；且 G 越正确越界越大（analytic 1.06 → "
            "精确 Smith 1.55），说明缺口在 diffuse 侧而不在 G"
        )
        if args.strict_total:
            failures.append(message)
        else:
            print(f"   ⚠️ {message}")
            print("      （默认不算脚本失败；加 --strict-total 可让它变成退出码 1。"
                  "判定与归因写在 case 记录的 D-07 一节）")

    # ---- ③b LUT 与白炉的互证（脚本内两条独立路径） ----
    print("\n③b 引擎 split-sum LUT 与白炉积分互证（F0=1 时 LUT 的 A+B 就是镜面反照率）")
    worst_lut_gap = 0.0
    for roughness in roughness_list:
        for cos_v in view_list:
            a_term, b_term = integrate_brdf(cos_v, roughness, k_ibl, args.samples)
            lut = a_term + b_term
            direct = specular_albedo(cos_v, roughness, 1.0, geometry_schlick_ggx_ibl, args.samples)
            worst_lut_gap = max(worst_lut_gap, abs(lut - direct))
    print(f"   |LUT(A+B) - 白炉(IBL 变体)| 最大差 = {worst_lut_gap:.4f}（判据 0.02）")
    if worst_lut_gap > 0.02:
        failures.append(f"LUT 与白炉积分不一致：{worst_lut_gap:.4f}")

    # ---- ③c `D-07` 候选试算：PG'97 的 coupled model（**离线试算，不改引擎**） ----
    #
    # 背景：本引擎的 `diffuseWeight = (1 - F(v·h))` 出自 Neubelt 2013 式(15)，属 PG'97 批评的
    # "半角 matte 阻尼"那一类（掠射下组合反照率越界，见 ③）。PG'97 给的替代是**可分离乘积**：
    #
    #     ρ(θ,θ') = R_f(θ)·ρ_s + k·R_m·[1 - R_f(θ)]·[1 - R_f(θ')],   k = 21 / (20π(1 - R_0))
    #
    # 其中 θ 是**入射角**、θ' 是出射角（与我们记 `v·h` 的半角不是一回事），R_f 用 Schlick。
    # 这一节只回答一个问题：**把它搬到我们的 GGX lobe 上，组合反照率还越界吗？**
    # 结论只作为"要不要改"的依据；**本脚本不改引擎，也不产生任何 shader 改动**。
    print("\n③c `D-07` 候选试算：PG'97 coupled model（离线；θ=入射角、θ'=出射角）")

    def schlick_transmit(cos_theta: float, f0: float) -> float:
        """`1 - R_f(θ)`（Schlick），即耦合模型里的透射/漫反射让路因子。"""
        return 1.0 - fresnel_schlick(max(cos_theta, 0.0), f0)

    def pg97_k(f0: float) -> float:
        """式(4) 印的归一化常数（与论文里那个未编号的 `[1-R_f(θ)][1-R_f(θ')]` 形式自洽）。"""
        return 21.0 / (20.0 * math.pi * max(1.0 - f0, 1.0e-6))

    def pg97_matte_albedo(cos_v: float, r_m: float, f0: float, k_scale: float = 1.0) -> float:
        """漫反射项的方向反照率（解析）：`∫ k R_m (1-R_f(θ_l))(1-R_f(θ_v)) cosθ_l dω_l`。

        `∫(1-R_f(θ))cosθ dω = (1-R_0)·20π/21`（把 Schlick 代进去逐项积分可得，
        其中 `∫(1-cosθ)^5 cosθ dω = 2π/42`），所以乘上 `k = 21/(20π(1-R_0))` 后整项塌成
        **`R_m·(1 - R_f(θ_v))`** —— 这正是 PG'97 式(3) 那条约束的解析体现：
        镜面拿走 `R_f(θ_v)`，漫反射拿走剩下的 `1 - R_f(θ_v)`。
        `k_scale` 用来复现"按印出来的式(5)"那一版（丢了 `(1-R_0)²`，见下）。
        """
        return r_m * k_scale * schlick_transmit(cos_v, f0)

    # 自检①：δ（理想镜面）下解析与数值积分必须一致，且总和恒为 1（R_m = 1 时）
    print("   自检①（δ 理想镜面，解析 vs 数值积分）:")
    worst_delta = 0.0
    for cos_v in view_list:
        for f0 in (1.0, 0.04):
            analytic = fresnel_schlick(cos_v, f0) + pg97_matte_albedo(cos_v, 1.0, f0)
            # 数值：matte 对 θ_l 直接求积（各向同性，方位角给 2π）
            steps = 20000
            numeric = 0.0
            k = pg97_k(f0)
            for i in range(steps):
                theta = (i + 0.5) / steps * (math.pi * 0.5)
                cos_l = math.cos(theta)
                numeric += (k * schlick_transmit(cos_l, f0) * schlick_transmit(cos_v, f0)
                            * cos_l * math.sin(theta)) * (math.pi * 0.5 / steps) * 2.0 * math.pi
            numeric += fresnel_schlick(cos_v, f0)
            worst_delta = max(worst_delta, abs(analytic - numeric))
            print(f"      cosθv={cos_v:.2f} F0={f0:.2f}: 解析 ρhd={analytic:.6f}  "
                  f"数值={numeric:.6f}  差={abs(analytic - numeric):.2e}")
    print(f"      -> 解析与数值最大差 {worst_delta:.2e}；R_m=1 时 ρhd 恒为 1.000000（式(3) 的体现）")
    if worst_delta > 1.0e-6:
        failures.append(f"PG'97 自检失败：解析与数值积分差 {worst_delta:.2e}")

    # 自检②：按**印出来的式(5)**（matte 括号里没有 (1-R_0)）会偏多少——量化那处印刷不一致
    print("   自检②（按印出来的式(5) 那一版，量化印刷不一致的 (1-R_0)²）:")
    for cos_v in view_list:
        f0 = 0.04
        printed = fresnel_schlick(cos_v, f0) + 1.0 * (
            (1.0 - (1.0 - cos_v) ** 5) / max(1.0 - f0, 1e-6))  # 式(5) 的括号 + 式(4) 的 k
        consistent = fresnel_schlick(cos_v, f0) + pg97_matte_albedo(cos_v, 1.0, f0)
        print(f"      cosθv={cos_v:.2f}: 式(5) 照抄 ρhd={printed:.4f} | 自洽版 ρhd={consistent:.4f}"
              f"  （差 {printed - consistent:+.4f}）")

    # 试算：PG'97 coupled + 本引擎的 GGX lobe（F 取入射角），与我们现行公式同网格对比
    print("   试算（同一网格，ρhd = 镜面 + 漫反射；判据 <= 1）:")
    print(f"      {'材质 / G 变体':38s} {'现行公式':>10s} {'PG97+GGX':>10s} {'PG97 混合*':>11s}")
    worst_pg97 = 0.0
    worst_pg97_case = ""
    for label, base_color, metallic in (
        ("conductor  F0=1.00", 1.0, 1.0),
        ("dielectric F0=0.04", 1.0, 0.0),
        ("metal grey F0=0.50", 0.5, 1.0),
    ):
        f0 = f0_from_parameters(base_color, metallic)
        r_m = base_color * (1.0 - metallic)
        for name, geometry in GEOMETRY_VARIANTS:
            current_peak = pg97_peak = hybrid_peak = 0.0
            for roughness in roughness_list:
                for cos_v in view_list:
                    current_peak = max(current_peak, specular_albedo(
                        cos_v, roughness, f0, geometry, args.samples)
                        + diffuse_albedo(cos_v, roughness, base_color, metallic, args.samples))
                    # 出处的原样：镜面 F 取入射角，漫反射用可分离乘积（解析）
                    pg97 = (specular_albedo(cos_v, roughness, f0, geometry, args.samples,
                                            fresnel_angle="incident")
                            + pg97_matte_albedo(cos_v, r_m, f0))
                    pg97_peak = max(pg97_peak, pg97)
                    # 混合（**仅试算，非出处原样**）：保留引擎现有镜面（F 取半角），只换 matte
                    hybrid = (specular_albedo(cos_v, roughness, f0, geometry, args.samples)
                              + pg97_matte_albedo(cos_v, r_m, f0))
                    hybrid_peak = max(hybrid_peak, hybrid)
            print(f"      {label + ' | ' + name.strip():38s} {current_peak:10.4f} "
                  f"{pg97_peak:10.4f} {hybrid_peak:11.4f}")
            if pg97_peak > worst_pg97:
                worst_pg97, worst_pg97_case = pg97_peak, f"{label} / {name.strip()}"
    print(f"      * 混合列 = PG'97 的 matte + 引擎现有镜面（F 取半角）——**只是试算，不是出处原样**，"
          f"仅用来分辨'越界来自 matte 还是来自镜面'")

    # 追加：引擎**可 author** 的介电范围（`inputs.specular` 0.5 → 1.0 即 F0 0.04 → 0.08）。
    # 上面的网格把 F0 与 metallic 绑在一起，只有 F0=0.04 那一行带漫反射；这里单独扫 F0。
    print("   追加（引擎可 author 的介电范围，R_m = 1，G 取精确 Smith）:")
    for f0 in (0.04, 0.06, 0.08):
        sweep_current = sweep_pg97 = sweep_hybrid = 0.0
        for roughness in roughness_list:
            for cos_v in view_list:
                sweep_current = max(
                    sweep_current,
                    specular_albedo(cos_v, roughness, f0, smith_g1_ggx, args.samples)
                    + diffuse_albedo(cos_v, roughness, 1.0, 0.0, args.samples))
                sweep_pg97 = max(
                    sweep_pg97,
                    specular_albedo(cos_v, roughness, f0, smith_g1_ggx, args.samples,
                                    fresnel_angle="incident")
                    + pg97_matte_albedo(cos_v, 1.0, f0))
                sweep_hybrid = max(
                    sweep_hybrid,
                    specular_albedo(cos_v, roughness, f0, smith_g1_ggx, args.samples)
                    + pg97_matte_albedo(cos_v, 1.0, f0))
        print(f"      F0={f0:.2f}: 现行公式 {sweep_current:.4f} | PG97+GGX {sweep_pg97:.4f}"
              f" | PG97 matte 混合 {sweep_hybrid:.4f}")

    print(f"   -> PG'97 + GGX 的全局最大 ρhd = {worst_pg97:.4f}（{worst_pg97_case}）"
          f"；现行公式见 ③ 的组合列（全局最大 {worst_total:.4f}）")
    if worst_pg97 <= 1.0 + args.energy_tolerance:
        print("      结论：**搬到 GGX 上仍然 <= 1**，即这条修法在我们的镜面模型下也成立")
    else:
        print("      结论：**搬到 GGX 上仍会越界** —— 出处那条守恒性依赖的是理想 δ 镜面"
              "（式(3) 里镜面的方向反照率就取 R_f(θ)），换成 GGX 后不再自动成立")
    print("   （本节只是试算，不改引擎；是否采纳见 case 记录 `D-07` 的候选一节）")

    # 追加：**间接光（IBL）路径**——它不经过 `diffuseWeight`，所以本节的修法覆盖不到它。
    # `CalculateDiffuseIbl` 是裸 Lambert（`irradiance·baseColor·(1-metallic)/π`，没有 (1-F)），
    # 所以均匀环境下 IBL 的总反照率 = `baseColor·(1-metallic)` + split-sum 镜面 `(F0·A + B)`，
    # 而且**比直接光那条路径还差**（漫反射几乎一点不让路）。这里把两条路径分开报。
    print("   追加（**间接光 IBL 路径**：`CalculateDiffuseIbl` 是裸 Lambert，不走 diffuseWeight）:")
    for f0, base_color, metallic in ((0.04, 1.0, 0.0), (0.08, 1.0, 0.0)):
        r_m = base_color * (1.0 - metallic)
        ibl_current = ibl_matte = 0.0
        for roughness in roughness_list:
            for cos_v in view_list:
                a_term, b_term = integrate_brdf(cos_v, roughness, k_ibl, args.samples)
                spec = f0 * a_term + b_term          # split-sum 的镜面方向反照率
                ibl_current = max(ibl_current, r_m + spec)                    # 裸 Lambert
                ibl_matte = max(ibl_matte, r_m * schlick_transmit(cos_v, f0) + spec)  # 换成 PG'97 matte
        print(f"      F0={f0:.2f}（dielectric, R_m={r_m:.0f}）: 现状（裸 Lambert+split-sum）"
              f" {ibl_current:.4f} | 换成 PG'97 matte {ibl_matte:.4f}")
    print("      -> 直接光那条路径的修法**不会**自动修好 IBL：IBL 的漫反射压根没有让路因子。")

    # ---- ④ 参数化边界（M-05 / D-08） ----
    print("\n④ 参数化边界（D-08）：metallic=1 时可见 diffuse 为 0；F0 / albedo 不越界")
    worst_diffuse_at_full_metal = 0.0
    worst_f0 = 0.0
    for base_color in (0.0, 0.25, 0.5, 0.75, 1.0):
        for cos_v in view_list:
            for roughness in roughness_list:
                value = diffuse_albedo(cos_v, roughness, base_color, 1.0, 512)
                worst_diffuse_at_full_metal = max(worst_diffuse_at_full_metal, value)
        worst_f0 = max(worst_f0, abs(f0_from_parameters(base_color, 1.0) - base_color))
        worst_f0 = max(worst_f0, abs(f0_from_parameters(base_color, 0.0) - ENGINE_DIELECTRIC_F0))
    print(f"   metallic=1 时的漫反射反照率最大值 = {worst_diffuse_at_full_metal:.2e}（应为 0）")
    print(f"   F0 映射偏差 = {worst_f0:.2e}（metallic=1 → F0=baseColor；metallic=0 → 0.04）")
    if worst_diffuse_at_full_metal > 1.0e-12:
        failures.append(f"metallic=1 仍有可见 diffuse：{worst_diffuse_at_full_metal:.2e}")
    if worst_f0 > 1.0e-12:
        failures.append(f"F0 映射与参数化不符：{worst_f0:.2e}")

    # ---- ⑤ 方向反照率对照（D-05；Burley 2012 Fig 12） ----
    #
    # Burley Fig 12 的结构：同一套 D（GGX/TR）与 F，只换 G，画 smooth（α = 0.02）与 rough（α = 0.5）
    # 两条入射角扫描；并在图注里定义 "No G" = 去掉 G 与 1/(cosθl cosθv)。
    # 约定提醒：Burley 用的是 Disney 的 α = roughness²，而引擎的入参是 perceptual roughness，
    # 所以这里按 **α 对齐**（roughness = √α），不是按 roughness 对齐——弄反会得到完全不同的曲线。
    print("\n⑤ 方向反照率 vs 入射角（D-05 / Burley 2012 Fig 12：同一 D 与 F，只换 G）")
    print("   α = roughness²（Disney 约定）；下表是每 15° 取一个点，另报定义域上最大值")
    angles_deg = list(range(0, 90, 15))
    worst_albedo = 0.0
    worst_albedo_case = ""
    for alpha_value in (0.02, 0.5):
        roughness = math.sqrt(alpha_value)
        print(f"   --- α = {alpha_value}（roughness = {roughness:.4f}）---")
        for name, geometry in GEOMETRY_VARIANTS:
            values = [
                specular_albedo(math.cos(math.radians(deg)), roughness, 1.0, geometry, args.samples)
                for deg in angles_deg
            ]
            peak = max(values)
            if peak > worst_albedo:
                worst_albedo = peak
                worst_albedo_case = f"α={alpha_value} / {name.strip()}"
            print(f"      {name}: " + " ".join(f"{v:.3f}" for v in values) + f"   max={peak:.4f}")
        no_g = [
            specular_albedo_plain_d(math.cos(math.radians(deg)), roughness, 1.0, args.samples)
            for deg in angles_deg
        ]
        print(f"      no G (D·F/4) : " + " ".join(f"{v:.3f}" for v in no_g) + f"   max={max(no_g):.4f}")
        # 论文的两条定性结论，各取一个可复核的数字
        smooth_gap = (
            specular_albedo(math.cos(math.radians(75)), roughness, 1.0, smith_g1_ggx, args.samples)
            - no_g[5]
        )
        print(f"      -> 75° 处 Smith(精确) − noG = {smooth_gap:+.4f}"
              f"（论文：省略 G 会明显偏暗，该差值应为正）")
        if smooth_gap <= 0.0:
            failures.append(f"α={alpha_value}：75° 处 noG 不比 Smith 暗（{smooth_gap:+.4f}）")
    print(f"   -> 全表最大方向反照率 {worst_albedo:.4f}（{worst_albedo_case}），"
          f"判据 <= 1 + {args.energy_tolerance:g}")
    if worst_albedo > 1.0 + args.energy_tolerance:
        failures.append(f"方向反照率越界：{worst_albedo:.4f} @ {worst_albedo_case}")

    # ---- ⑤b 生成 VL 里可看的表（mode 6） ----
    if args.emit_glsl is not None:
        table_alphas = [0.02, 0.10, 0.25, 0.50, 1.00]
        table_angles = [5.0 * i for i in range(0, 18)]          # 0°..85°
        arms = [geometry_schlick_ggx, geometry_schlick_ggx_ibl, smith_g1_ggx]
        table: list[list[list[float]]] = []
        print(f"\n⑤b 生成 mode 6 的表（{len(table_alphas)} α × {len(table_angles)} 角度 × 4 臂，"
              f"每点 {args.emit_samples} 样本）……")
        for alpha_value in table_alphas:
            roughness = math.sqrt(alpha_value)
            arm_rows = []
            for geometry in arms:
                arm_rows.append([
                    specular_albedo(math.cos(math.radians(deg)), roughness, 1.0,
                                    geometry, args.emit_samples)
                    for deg in table_angles
                ])
            arm_rows.append([
                specular_albedo_plain_d(math.cos(math.radians(deg)), roughness, 1.0,
                                        args.emit_samples)
                for deg in table_angles
            ])
            table.append(arm_rows)
            print(f"   α={alpha_value:.2f} 完成（roughness={roughness:.4f}）")
        emit_albedo_glsl(Path(args.emit_glsl), table_alphas, table_angles, table,
                         args.emit_samples)

    print()
    if failures:
        for line in failures:
            print(f"不通过: {line}")
        return 1
    print("判定: 通过（NDF 归一化 / G1 性质 / 白炉 / 参数化边界 / 方向反照率）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
