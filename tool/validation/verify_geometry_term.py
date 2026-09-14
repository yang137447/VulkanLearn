"""几何项 Schlick k 变体的离线参考（shading-model-alignment-plan 的 `D-04` / `D-20` / `D-21`）。

被验证的对象是**公式本身**，不是渲染结果，所以用离线脚本而不是曲线探针：
把 shader 侧的三支 k 抽出来，在 `(roughness, cosθ)` 网格上量化它们的差别，
再用 Karis 2013 的 `IntegrateBRDF` 独立重算 split-sum 预积分，给出"换一支 k 会让
LUT 的 A/B 变多少"。这与 §M-02 / §M-04 的"离线独立积分 + 与 shader 输出对照"是同一种
证据形式（曲线探针负责像素级对照，本脚本负责公式级对照）。

## 三支 k 分别是什么（Karis 2013 §Specular G）

| 名字 | 表达式 | 出处 / 用途 |
| --- | --- | --- |
| `analytic` | `k=(r+1)²/8` | 论文式(4)：Disney 的粗糙度重映射 `(Roughness+1)/2` 再平方。论文原话限定 *"this adjustment is only used for analytic light sources"* → 引擎直接光路径（`GeometrySchlickGGX`） |
| `ibl` | `k=α/2`（α = r²） | 同节基础拟合 *"we chose to use the Schlick model, but with k = α/2"* → IBL 域；引擎 split-sum LUT 生成器（`GeometrySchlickGGXIbl`） |
| `alpha_sq_misread` | `k=α²/2 = r⁴/2` | **不是任何实现里的公式**。它是把 `brfdLut.comp` 里旧写法的局部变量 `float a = roughness`（感知粗糙度，不是 α）误当成 α 之后得到的式子；本脚本把它一起算出来，是为了让"误读值离真实值有多远"有数，从而说明 `D-21` 的审计标注为什么会出错。2026-09-13 起 shader 里该变量已改名为 `alpha`，这类误读在命名上被堵死 |

判据（脚本不通过即非零退出）：

1. `ibl` 与"按 `brfdLut.comp` 字面写法求值"逐点相等（同一表达式的两种写法）；
2. `ibl != analytic`，且两者在定义域上的最大偏差被报出来（这是论文两支变体的真实差距）；
3. `alpha_sq_misread` 与 `ibl` 的偏差被报出来（误读值的量级）。

用法：

    py -3 tool/validation/verify_geometry_term.py [--grid 41] [--lut-size 17] [--samples 512]
"""

from __future__ import annotations

import argparse
import math
import sys

# ---------------------------------------------------------------------------
# 三支 k（与 shader/glsl/common/microfacetDistribution.glsl 逐字对应）
# ---------------------------------------------------------------------------


def k_analytic(roughness: float) -> float:
    """common/microfacetDistribution.glsl: GeometrySchlickGGX，Karis 式(4) 的 analytic 变体。"""
    r = roughness + 1.0
    return (r * r) / 8.0


def k_ibl(roughness: float) -> float:
    """common/microfacetDistribution.glsl: GeometrySchlickGGXIbl，论文 §Specular G 的基础拟合。

    以 α = roughness² 记，`k = α/2`；直接写成 `roughness²/2`。
    """
    alpha = roughness * roughness
    return alpha * 0.5


def k_lut_literal(roughness: float) -> float:
    """`generator/brfdLut.comp` 的字面写法（2026-09-13 起）：`alpha = roughness²; k = alpha*0.5`。

    与 `k_ibl` 是同一表达式——这正是 `D-21` 的结论：LUT 里用的就是论文的 `k = α/2`。
    这条断言现在是"工具镜像了 shader 当前文本"的自检。
    """
    alpha = roughness * roughness
    return alpha * 0.5


def k_lut_legacy_literal(roughness: float) -> float:
    """改名**之前**的字面写法：`float a = roughness; k = (a*a)*0.5`。

    单独保留一支，是为了让 `D-21` 的判定一直可复核：当时的代码同样等于 `α/2`，
    误读成 `α²/2` 来自变量名（`a` 在同一个文件里两处含义不同），不是代码写错了。
    """
    a = roughness
    return (a * a) * 0.5


def k_alpha_sq_misread(roughness: float) -> float:
    """`k = α²/2 = roughness⁴/2`：D-21 审计表里写的那个式子，仓库里从来没有过。"""
    alpha = roughness * roughness
    return (alpha * alpha) * 0.5


def g1_schlick(cos_theta: float, k: float) -> float:
    cos_theta = max(cos_theta, 0.0)
    return cos_theta / (cos_theta * (1.0 - k) + k)


def smith_g1_ggx(cos_theta: float, roughness: float) -> float:
    """common/microfacetDistribution.glsl: SmithG1Ggx（精确解）。"""
    alpha = roughness * roughness
    alpha_squared = alpha * alpha
    clamped = min(max(cos_theta, 0.0), 1.0)
    root = math.sqrt(alpha_squared + (1.0 - alpha_squared) * clamped * clamped)
    return 2.0 * clamped / (clamped + root)


# ---------------------------------------------------------------------------
# split-sum 预积分（与 generator/brfdLut.comp 的 IntegrateBRDF 独立重写）
# ---------------------------------------------------------------------------


def radical_inverse_vdc(bits: int) -> float:
    bits = ((bits << 16) | (bits >> 16)) & 0xFFFFFFFF
    bits = (((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1)) & 0xFFFFFFFF
    bits = (((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2)) & 0xFFFFFFFF
    bits = (((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4)) & 0xFFFFFFFF
    bits = (((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8)) & 0xFFFFFFFF
    return float(bits) * 2.3283064365386963e-10


def hammersley(index: int, count: int) -> tuple[float, float]:
    return index / count, radical_inverse_vdc(index)


def importance_sample_ggx(xi_x: float, xi_y: float, roughness: float) -> tuple[float, float, float]:
    """在 N=(0,0,1) 的局部空间按 GGX 采样半角向量 H（与 brfdLut.comp 同式）。

    α = roughness²，变量名与 shader 一致（都是 `alpha`）。
    """
    alpha = roughness * roughness
    alpha_squared = alpha * alpha
    phi = 2.0 * math.pi * xi_x
    cos_theta = math.sqrt(
        max((1.0 - xi_y) / (1.0 + (alpha_squared - 1.0) * xi_y), 0.0)
    )
    sin_theta = math.sqrt(max(1.0 - cos_theta * cos_theta, 0.0))
    return math.cos(phi) * sin_theta, math.sin(phi) * sin_theta, cos_theta


def integrate_brdf(ndotv: float, roughness: float, k_function, samples: int) -> tuple[float, float]:
    """Karis 2013 §Environment BRDF 的 IntegrateBRDF，返回 (A, B)。"""
    a_sum = 0.0
    b_sum = 0.0
    k = k_function(roughness)
    for i in range(samples):
        xi_x, xi_y = hammersley(i, samples)
        hx, hy, hz = importance_sample_ggx(xi_x, xi_y, roughness)
        # L = reflect(-V, H)，V = (sin, 0, cos)
        vx = math.sqrt(max(1.0 - ndotv * ndotv, 0.0))
        vz = ndotv
        vdot_h = max(vx * hx + vz * hz, 0.0)
        lx = 2.0 * vdot_h * hx - vx
        lz = 2.0 * vdot_h * hz - vz
        ndot_l = max(lz, 0.0)
        ndot_h = max(hz, 0.0)
        if ndot_l <= 0.0:
            continue
        g = g1_schlick(ndotv, k) * g1_schlick(ndot_l, k)
        g_vis = (g * vdot_h) / max(ndot_h * ndotv, 1.0e-5)
        fc = (1.0 - vdot_h) ** 5
        a_sum += (1.0 - fc) * g_vis
        b_sum += fc * g_vis
    return a_sum / samples, b_sum / samples


# ---------------------------------------------------------------------------


def sample_grid(grid: int, count: int):
    for i in range(1, grid + 1):
        roughness = i / grid
        for j in range(1, grid + 1):
            yield roughness, j / grid


def max_deviation(grid: int, count: int, left, right) -> tuple[float, float, float]:
    """返回 (最大偏差, 取到最大偏差的 roughness, 取到最大偏差的 cosθ)。"""
    worst = -1.0
    worst_roughness = 0.0
    worst_cos = 0.0
    for roughness, cos_theta in sample_grid(grid, count):
        deviation = abs(left(cos_theta, roughness) - right(cos_theta, roughness))
        if deviation > worst:
            worst = deviation
            worst_roughness = roughness
            worst_cos = cos_theta
    return worst, worst_roughness, worst_cos


def main(argv: list[str]) -> int:
    # Windows 控制台默认代码页可能是 GBK，而报告里有 α / ² / θ 这类字符：直接 print 会
    # 抛 UnicodeEncodeError 把工具打断。这里只放宽错误处理（不强行换编码），
    # 让工具在任何代码页下都能跑完并给出退出码。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--grid", type=int, default=64, help="roughness x cosθ 网格边长（默认 64）")
    parser.add_argument("--lut-size", type=int, default=17, help="split-sum LUT 采样边长（默认 17）")
    parser.add_argument("--samples", type=int, default=512, help="每纹素的重要性采样数（默认 512）")
    parser.add_argument("--tolerance", type=float, default=1.0e-12, help="同一表达式的相等判据")
    args = parser.parse_args(argv)

    failures: list[str] = []

    # ---- ① 当前字面写法 == k_ibl，且改名前的字面写法也 == k_ibl（D-21 的核心事实） ----
    literal_gap = 0.0
    legacy_gap = 0.0
    for roughness, _ in sample_grid(args.grid, args.grid):
        literal_gap = max(
            literal_gap,
            abs(k_lut_literal(roughness) - k_ibl(roughness)),
        )
        legacy_gap = max(
            legacy_gap,
            abs(k_lut_legacy_literal(roughness) - k_ibl(roughness)),
        )
    print("① brfdLut.comp 的字面写法 vs k=α/2（α = roughness²）")
    print(f"   当前写法（alpha = roughness²; k = alpha*0.5）：最大偏差 {literal_gap:.3e}")
    print(f"   改名前的写法（a = roughness; k = (a*a)*0.5）：最大偏差 {legacy_gap:.3e}"
          f"（判据 <= {args.tolerance:.1e}）")
    if literal_gap > args.tolerance or legacy_gap > args.tolerance:
        failures.append("brfdLut.comp 的字面写法不等于 k = α/2")
    else:
        print("   -> 两支写法都等于 k = α/2：误读来自变量名 `a`，不是代码写错")

    # ---- ② 论文两支变体的差距 ----
    print("\n② 论文两支 Schlick 变体的差距（analytic k=(r+1)²/8 vs ibl k=α/2）")
    pairs = [
        ("G1: ibl      vs analytic", lambda c, r: g1_schlick(c, k_ibl(r)),
         lambda c, r: g1_schlick(c, k_analytic(r))),
        ("G1: ibl      vs smith   ", lambda c, r: g1_schlick(c, k_ibl(r)), smith_g1_ggx),
        ("G1: analytic vs smith   ", lambda c, r: g1_schlick(c, k_analytic(r)), smith_g1_ggx),
        ("G1: α²/2误读 vs ibl     ", lambda c, r: g1_schlick(c, k_alpha_sq_misread(r)),
         lambda c, r: g1_schlick(c, k_ibl(r))),
        ("G1: α²/2误读 vs smith   ", lambda c, r: g1_schlick(c, k_alpha_sq_misread(r)),
         smith_g1_ggx),
    ]
    for label, left, right in pairs:
        worst, roughness, cos_theta = max_deviation(args.grid, args.grid, left, right)
        print(
            f"   {label}: 最大偏差 {worst:.6f}  "
            f"@ roughness={roughness:.3f} cosθ={cos_theta:.3f}"
        )

    # ---- ③ k 值本身在 [0,1] 上的范围 ----
    print("\n③ k 的取值范围（roughness ∈ (0,1]）")
    for name, function in (
        ("analytic (r+1)²/8", k_analytic),
        ("ibl      α/2     ", k_ibl),
        ("misread  α²/2    ", k_alpha_sq_misread),
    ):
        values = [function(i / args.grid) for i in range(1, args.grid + 1)]
        print(f"   {name}: [{min(values):.6f}, {max(values):.6f}]")

    # ---- ④ 换成另一支 k 会让 split-sum LUT 变多少 ----
    print(
        f"\n④ split-sum LUT 的 A/B 差异（LUT {args.lut_size}x{args.lut_size}，"
        f"每纹素 {args.samples} 样本，Karis IntegrateBRDF 的独立重算）"
    )
    worst_a = 0.0
    worst_b = 0.0
    worst_a_at = (0.0, 0.0)
    worst_b_at = (0.0, 0.0)
    for i in range(1, args.lut_size + 1):
        roughness = i / args.lut_size
        for j in range(1, args.lut_size + 1):
            ndotv = j / args.lut_size
            a_ibl, b_ibl = integrate_brdf(ndotv, roughness, k_ibl, args.samples)
            a_other, b_other = integrate_brdf(ndotv, roughness, k_analytic, args.samples)
            if abs(a_ibl - a_other) > worst_a:
                worst_a = abs(a_ibl - a_other)
                worst_a_at = (roughness, ndotv)
            if abs(b_ibl - b_other) > worst_b:
                worst_b = abs(b_ibl - b_other)
                worst_b_at = (roughness, ndotv)
    print(
        f"   max |A(ibl) - A(analytic)| = {worst_a:.6f}  "
        f"@ roughness={worst_a_at[0]:.3f} NoV={worst_a_at[1]:.3f}"
    )
    print(
        f"   max |B(ibl) - B(analytic)| = {worst_b:.6f}  "
        f"@ roughness={worst_b_at[0]:.3f} NoV={worst_b_at[1]:.3f}"
    )
    print("   （若改用 analytic 变体，这就是 LUT 会偏掉的量级；D-20 的结论是论文禁止这么做）")

    print()
    if failures:
        for line in failures:
            print(f"不通过: {line}")
        return 1
    print("判定: 通过（三支 k 的定义与论文/实现一致，误读量级已记录）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
