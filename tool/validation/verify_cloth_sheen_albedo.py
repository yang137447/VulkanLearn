"""A 臂 Cloth/Sheen（Estévez & Kulla 2017）的**公式级**离线验证：E_s 方向反照率口径 + 白炉清单。

覆盖的 case（plan 的编号）：

| 层 | case | 判定什么 |
| --- | --- | --- |
| A 臂 | `S-03` | E_s 表的离线积分口径；16×16 起步规格的插值误差，`r→0` 附近是否够 |
| A 臂 | `S-04` | `baseScale = 1 − c·E_s` 的值域；直光对 `min(1−cE)` 两端取小 |
| A 臂 | `S-10` | 白炉：sheen 半球积分 ≤ 1；直接光 BRDF 的均匀环境积分不超 1 |
| A 臂 | `S-33` | 间接光是直接光 BRDF 的半球积分：base 用 `S`，sheen 用软化后的 `E†`，再乘辐照度/π |
| A 臂 | `S-27` | 分层因子口径：`min(1−E)` 与误写 `min(E)` / 直光单端的数值对照 |
| 公式 | `S-32` | 式(3) 的 `L(x)` 形式与式(4) 的软化：旧读法的数值差异（跨实现交叉核对） |

## 为什么必须离线（plan §1.5.3）

判据是**积分恒等式**（方向反照率、能量），要的是积分值而不是某张图的观感。像素只能证明
"shader 画的曲线与解析期望一致"，不能证明"这条曲线积分为 1"。本脚本把论文式(1)–(4) 独立实现
一遍，用于：钉死 `E_s` 表的**离线积分口径**（将来 GPU LUT generator 的公式镜像）、把白炉与值域
断言变成可机检的数字、量化两处读法（`L(x)` 的指数位置、软化的作用层）在数值上差多少。

真正的物理内容是 **`E_s ≤ 1`**。第 4 节的单端 "IBL ≡ 1" 是代数代理，均匀环境下必然成立，
不能当间接光验收。直接光 BRDF 的半球积分在第 11 节（`S-33`）。

本模型（Charlie 分布 / Estévez Λ 拟合）此前在 `tool/validation/` 里没有镜像，所以只在本文件
定义一次；曲线像素测量仍走 `measure_plot_curve.py`，两者互补。

## 口径（本脚本即合同；与知识库 `cloth-sheen` 页 A2/A5/A6 一致）

1. `f_sheen = F·D·G / (4·NoL·NoV)`，`F = 1`（论文不考虑 Fresnel，讲义 slide 60）；
2. `D(θ_h) = (2 + 1/r)·sin^(1/r)θ_h / 2π`，`r` 直接用（**不**做 `α = r²`）；
3. 式(3)：`Λ(θ) = exp(L(x))`（`x<0.5`）或 `exp(2L(0.5) − L(1−x))`（`x≥0.5`），
   `L(x) = a/(1 + b·x^c) + d·x + e` —— **指数在分母的 `x` 上**（旧读法把指数抄到 `d·x^c` 上）；
4. 式(4)：软化只作用于**直光的受光侧**，且在对数域是**乘**系数：
   `Λ'(θi) = Λ(θi)^(1+2(1−cosθi)^8)` ⇔ `ln Λ' = k·ln Λ`，`k = 1 + 2(1−cosθi)^8`
   （法线入射 `k = 1`，掠射 `k = 3`）；
5. `E_s(NoV, r) = ∫_{Ω+} f_sheen·NoL dω_l = (1/(4·NoV))·∫_{Ω+} G·D dω_l`（`NoL` 与积分权重约掉一个）；
   **表用未软化的互易核**：表只有一个自变量轴（cosθ），直光两端都要查它，而软化破坏了互易性；
6. 分层：被嵌套的 base 乘 `1 − c_s·E_s`（`c_s` = sheen color）。表里存的是**不含颜色**的几何
   反照率，所以颜色必须在这里乘一次；直光对**两端取小**。间接光不是把这个因子收成单端
   `1−c·E(NoV)`：那是代数代理，均匀环境下恒等于 1，但不是直接光 BRDF 的半球积分（`S-33`）；
7. 采样：**均匀半球**（论文 §2 自己的结论：*"plain uniform sampling of the upper hemisphere to be
   more effective"*）。离线用 `(cosθ, φ)` 上等立体角的确定性分层中点法，并做 `N → 2N` 收敛自检。

## 钉死的 LUT 规格（`S-03` 的判定结果，见下面 [8]/[9] 两节）

| 项 | 取值 | 判据 |
| --- | --- | --- |
| 分辨率 | **16×16**（讲义规格） | 同一参数化升到 32×32 收益很小，不值得翻四倍内存 |
| `NoV` 轴 | **在 `[cos 80°, 1]` 内按余弦均分**（`u=(NoV−cos80°)/(1−cos80°)`，查询先 clamp） | 深掠射角上论文的 Λ **拟合**会让 `E_s > 1`（`r=0.01` 时到 `NoV=0.087` 才回落），而**限域必须真正作用到轴样本上**：不限域的余弦轴（`NoV=t`）每个 `θmax` 档都会烘进 `E_s>1` 的格子（扫描里 "域内 E_s≤1" 全为否），按角度均分则误差 0.041~0.23 |
| `r` 轴 | **按 `√r` 均分**（`v=√r`） | 线性 `r` 轴最大绝对误差 0.129~0.192，`√r` 轴 **0.012** |
| 采样坐标 | 查询值先夹到轴域（clamp-to-edge），再双线性插值 | 与 GPU 线性采样一致；夹取代价见下面 [9] 节的实测 |
| 颜色 | 不进表 | 换 sheen 颜色不重烘；扣除量在分层时乘 `c_s`（A5.2） |

**已知模型瑕疵（登记，不修）**：`NoV` 很小时（`r ≲ 0.01` 且 `NoV < 0.09`）拟合出来的 `E_s` 会超过 1，
这是式(3) 的 Λ **拟合**在深掠射端的失配，不是积分口径的问题。做法：轴域避开它 +
在文档里登记；如果将来确实需要那段掠射 sheen，再讨论是夹 lobe 还是换 Λ 的解析形式。

用法：

    py -3 tool/validation/verify_cloth_sheen_albedo.py [--n-theta 256] [--n-phi 128] [--lut 16]
    py -3 tool/validation/verify_cloth_sheen_albedo.py --quick           # 粗求积，快速回归
    py -3 tool/validation/verify_cloth_sheen_albedo.py --json out.json   # 机器可读结果

退出码：任一断言不通过即 1（判定与差异归因最终写进 `shading-model-case-records.md`）。
"""

from __future__ import annotations

import argparse
import json
import math
import sys

TWO_PI = 2.0 * math.pi

# 论文 Table 1：Λ 曲线拟合系数，r = 0.0 与 r = 1.0 两组（逐项按 (1−r)² 插值）。
FIT = {
    0.0: (25.3245, 3.32435, 0.16801, -1.27393, -4.85967),
    1.0: (21.5473, 3.82987, 0.19823, -1.97760, -4.32054),
}

# r 的下限（论文 r ∈ (0,1]；式(2) 的 (2 + 1/r) 在 r→0 发散）。
R_MIN = 1e-3
# 掠射端的余弦下限：Λ → ∞ 但 G/(4·NoV) 的极限有限，clamp 后仍要能算。
COS_MIN = 1e-4
# 求积与断言容差。
TOL_NDF = 1e-3
TOL_FURNACE = 5e-4
LOG_OVERFLOW = 700.0


def fit_coefficients(r: float) -> tuple[float, float, float, float, float]:
    """论文 A2.5：只在 r=0 与 r=1 拟合，中间逐项按 `P = (1−r)²·P₀ + (1−(1−r)²)·P₁` 插值。

    注意权重方向：**r=0 取 `P₀`、r=1 取 `P₁`**（`t = (1−r)²` 在 r=0 时为 1）。
    GLSL 侧写成 `mix(P₁, P₀, t)`（`mix(x,y,a) = x·(1−a) + y·a`）。
    """
    r = min(max(r, 0.0), 1.0)
    t = (1.0 - r) * (1.0 - r)
    return tuple(t * p0 + (1.0 - t) * p1 for p0, p1 in zip(FIT[0.0], FIT[1.0]))


def l_curve(x: float, r: float, legacy_exponent: bool = False) -> float:
    """`L(x)`（对数域的 Λ 拟合值）。

    `legacy_exponent=True` 复现旧写法 `a/(1+b·x) + d·x^c + e`，只用于 `S-32` 的差异对照。
    """
    a, b, c, d, e = fit_coefficients(r)
    x = min(max(x, 0.0), 1.0)
    if legacy_exponent:
        return a / (1.0 + b * x) + d * (x**c) + e
    return a / (1.0 + b * (x**c)) + d * x + e


def log_lambda(
    cos_theta: float,
    r: float,
    soften: bool = False,
    legacy_exponent: bool = False,
    legacy_pow: bool = False,
) -> float:
    """`ln Λ(θ)`：式(3) 的分段载体 +（可选）式(4) 的软化，全程对数域，避免中间量溢出。"""
    x = min(max(cos_theta, COS_MIN), 1.0)
    if x < 0.5:
        value = l_curve(x, r, legacy_exponent)
    else:
        value = 2.0 * l_curve(0.5, r, legacy_exponent) - l_curve(1.0 - x, r, legacy_exponent)
    if soften and not legacy_pow:
        # 式(4)：Λ' = Λ^(1+2(1−cosθ)^8) ⇒ ln Λ' = k·ln Λ
        value *= 1.0 + 2.0 * ((1.0 - x) ** 8)
    return value


def lambda_(
    cos_theta: float,
    r: float,
    soften: bool = False,
    legacy_exponent: bool = False,
    legacy_pow: bool = False,
) -> float:
    """`Λ(θ)`。

    `legacy_pow=True` 复现旧写法：把 `L` 本身当 `Λ` 返回，软化写成 `pow(L, k)`（仅用于对照）。
    """
    if legacy_pow:
        x = min(max(cos_theta, COS_MIN), 1.0)
        a, b, c, d, e = fit_coefficients(r)
        if x < 0.5:
            value = a / (1.0 + b * x) + d * (x**c) + e
        else:
            l_half = a / (1.0 + b * 0.5) + d * (0.5**c) + e
            l_mirror = a / (1.0 + b * (1.0 - x)) + d * ((1.0 - x) ** c) + e
            value = 2.0 * l_half - l_mirror
        return value ** (1.0 + 2.0 * ((1.0 - x) ** 8)) if soften else value
    log_value = log_lambda(cos_theta, r, soften, legacy_exponent, legacy_pow)
    return math.inf if log_value > LOG_OVERFLOW else math.exp(log_value)


def charlie_d(noh: float, r: float) -> float:
    """式(2)：`D(θ_h) = (2 + 1/r)·sin^(1/r)θ_h / 2π`。"""
    r = max(r, R_MIN)
    sin2 = max(0.0, 1.0 - noh * noh)
    return (2.0 + 1.0 / r) * (sin2 ** (0.5 / r)) / TWO_PI


def geometry_term(nol: float, nov: float, r: float, soften: bool = True, **kwargs) -> float:
    """式(3) + 式(4)：`G = 1/(1 + Λ(ωo) + Λ'(ωi))`。"""
    return 1.0 / (
        1.0 + lambda_(nov, r, False, **kwargs) + lambda_(nol, r, soften, **kwargs)
    )


def ndf_projected_area(n_theta: int, r: float) -> float:
    """`∫ D(h)(n·h) dω_h`（上半球，等立体角分层中点法）：理论值 1。"""
    d_cos = 1.0 / n_theta
    total = 0.0
    for i in range(n_theta):
        cos_h = (i + 0.5) * d_cos
        noh = math.sqrt(max(0.0, 1.0 - cos_h * cos_h))
        total += charlie_d(noh, r) * cos_h
    return TWO_PI * total * d_cos


def directional_albedo(
    nov: float,
    r: float,
    n_theta: int,
    n_phi: int,
    soften: bool = False,
    **kwargs,
) -> float:
    """`E_s(NoV, r) = (1/(4·NoV))·∫_{Ω+} G·D dω_l`（等立体角分层中点法）。

    出射方向放在 `n = (0,0,1)`、`v = (sinθv, 0, cosθv)`；入射方向遍历上半球，
    `dω = dφ·d(cosθ)`（均匀半球），`NoH = h·n`、`h = normalize(l + v)`。
    """
    nov_c = max(nov, COS_MIN)
    sin_v = math.sqrt(max(0.0, 1.0 - nov_c * nov_c))
    lambda_v = lambda_(nov_c, r)
    d_cos = 1.0 / n_theta
    d_phi = TWO_PI / n_phi
    total = 0.0
    for i in range(n_theta):
        cos_i = (i + 0.5) * d_cos
        sin_i = math.sqrt(max(0.0, 1.0 - cos_i * cos_i))
        g_i = 1.0 / (1.0 + lambda_v + lambda_(cos_i, r, soften, **kwargs))
        row = 0.0
        for j in range(n_phi):
            phi = (j + 0.5) * d_phi
            lx, ly, lz = sin_i * math.cos(phi), sin_i * math.sin(phi), cos_i
            hx, hy, hz = lx + sin_v, ly, lz + nov_c
            h_len = math.sqrt(hx * hx + hy * hy + hz * hz)
            if h_len < 1e-12:
                continue
            row += charlie_d(hz / h_len, r)
        total += row * g_i * d_cos * d_phi
    return total / (4.0 * nov_c)


def bake_lut(nov_axis, r_axis, n_theta: int, n_phi: int) -> list:
    return [[directional_albedo(nov, r, n_theta, n_phi) for r in r_axis] for nov in nov_axis]


def axis_samples(kind: str, n: int, theta_max_deg: float) -> list:
    """LUT 的归一化轴坐标 → 物理量（`t = (k+0.5)/n`，与 GPU 线性采样一致）。

    - `cos`：`NoV = cos(θmax) + t·(1 − cos(θmax))`——在**限制域内**按余弦均分（推荐）；
    - `cos_full`：`NoV = t`——余弦均分但不限域（会覆盖到 `E_s > 1` 的深掠射角）；
    - `angle`：`NoV = cos(θmax·(1−t))`——按**角度**均分到 `θmax`（`t=0` 是域最掠射端）；
    - `sqrt`：`NoV = t²`。
    """
    theta_max = math.radians(theta_max_deg)
    nov_min = math.cos(theta_max)
    out = []
    for k in range(n):
        t = (k + 0.5) / n
        if kind == "cos":
            out.append(max(nov_min + t * (1.0 - nov_min), COS_MIN))
        elif kind == "cos_full":
            out.append(max(t, COS_MIN))
        elif kind == "sqrt":
            out.append(max(t * t, COS_MIN))
        elif kind == "angle":
            out.append(max(math.cos(theta_max * (1.0 - t)), COS_MIN))
        else:
            raise ValueError(f"未知轴参数化：{kind}")
    return out


def cosine_weighted_average(weight_of_mu, n_mu: int) -> float:
    """`(1/π) ∫ g(NoL) NoL dω = 2 ∫_0^1 g(μ) μ dμ`，μ 上的中点法。"""
    acc = 0.0
    for i in range(n_mu):
        mu = (i + 0.5) / n_mu
        acc += weight_of_mu(mu) * mu
    return 2.0 * acc / n_mu


def base_retention(nov: float, r: float, color: float, n_mu: int, n_theta: int, n_phi: int) -> float:
    """直接光 base 在单位 radiance 下的半球响应。

    `S(NoV,r,c) = 2 ∫_0^1 min(1−c·E(μ), 1−c·E(NoV)) μ dμ`。
    `E` 用未软化核，和直光两端查表是同一个量。
    """
    e_view = directional_albedo(nov, r, n_theta, n_phi, soften=False)
    cache = {}

    def albedo(mu: float) -> float:
        key = round(mu, 6)
        if key not in cache:
            cache[key] = directional_albedo(mu, r, n_theta, n_phi, soften=False)
        return cache[key]

    def weight(mu: float) -> float:
        return min(1.0 - color * albedo(mu), 1.0 - color * e_view)

    return cosine_weighted_average(weight, n_mu)


def lut_sample(table, nov_axis, r_axis, nov: float, r: float) -> float:
    """双线性采样：先把查询值夹到轴域内（= GPU 的 clamp-to-edge），再在归一化坐标上插值。"""

    def locate(axis, value):
        if value <= axis[0]:
            return 0, 0.0
        if value >= axis[-1]:
            return len(axis) - 2, 1.0
        for k in range(len(axis) - 1):
            if axis[k] <= value <= axis[k + 1]:
                span = axis[k + 1] - axis[k]
                return k, 0.0 if span <= 0.0 else (value - axis[k]) / span
        return len(axis) - 2, 1.0

    i, ti = locate(nov_axis, nov)
    j, tj = locate(r_axis, r)
    a = table[i][j] * (1.0 - ti) + table[i + 1][j] * ti
    b = table[i][j + 1] * (1.0 - ti) + table[i + 1][j + 1] * ti
    return a * (1.0 - tj) + b * tj


def lut_error(
    nov_axis,
    r_axis,
    table,
    probes,
    n_theta: int,
    n_phi: int,
) -> tuple[float, tuple, float, tuple]:
    """返回 (最大绝对误差, 位置, 最大相对误差, 位置)；相对误差按参考值归一。"""
    errors = []
    for nov, r in probes:
        ref = directional_albedo(nov, r, n_theta, n_phi)
        got = lut_sample(table, nov_axis, r_axis, nov, r)
        errors.append((abs(got - ref), abs(got - ref) / max(ref, 1e-9), nov, r))
    max_abs = max(errors, key=lambda item: item[0])
    max_rel = max(errors, key=lambda item: item[1])
    return max_abs[0], (max_abs[2], max_abs[3]), max_rel[1], (max_rel[2], max_rel[3])


class Report:
    def __init__(self) -> None:
        self.checks: list[tuple[str, bool, str]] = []
        self.data: dict = {}

    def check(self, name: str, ok: bool, detail: str) -> None:
        self.checks.append((name, ok, detail))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}\n         {detail}")

    @property
    def failed(self) -> int:
        return sum(1 for _, ok, _ in self.checks if not ok)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--n-theta", type=int, default=256, help="极角分层数（等立体角，均匀半球）")
    parser.add_argument("--n-phi", type=int, default=128, help="方位角分层数")
    parser.add_argument("--quick", action="store_true", help="粗求积快速回归（96×48）")
    parser.add_argument("--converge", action="store_true", help="只做 N→2N 收敛自检")
    parser.add_argument("--json", type=str, default="", help="把结果写到 JSON")
    args = parser.parse_args()

    n_theta = 96 if args.quick else args.n_theta
    n_phi = 48 if args.quick else args.n_phi
    report = Report()

    print("=" * 78)
    print(f"A 臂 E_s 离线验证：求积 {n_theta}×{n_phi}（均匀半球分层中点法）")
    print("=" * 78)

    if args.converge:
        print("\n[收敛自检] E_s 在 N → 2N 下的变化")
        for nov, r in ((0.5, 0.05), (0.5, 0.5), (0.1, 0.3), (0.9, 0.9)):
            a = directional_albedo(nov, r, n_theta, n_phi)
            b = directional_albedo(nov, r, n_theta * 2, n_phi * 2)
            c = directional_albedo(nov, r, n_theta * 4, n_phi * 4)
            print(
                f"  NoV={nov:<4} r={r:<4} E_s: {a:.8f} → {b:.8f} → {c:.8f}"
                f"  |Δ| {abs(b - a):.2e} → {abs(c - b):.2e}"
            )
        return 0

    # ---- 1. 求积自检：∫ D(n·h) dω = 1 --------------------------------------------
    print("\n[1] 求积自检：∫ D(h)(n·h) dω_h = 1（式(2) 的投影面积归一化）")
    for r in (0.1, 0.3, 0.5, 1.0):
        value = ndf_projected_area(n_theta, r)
        report.check(
            f"D 归一化 r={r}",
            abs(value - 1.0) <= TOL_NDF,
            f"∫D(n·h)dω = {value:.9f}（偏差 {value - 1.0:+.2e}）",
        )

    # ---- 2. Λ 的端点与式(4) 的作用层 ---------------------------------------------
    print("\n[2] Λ 与式(4)：端点（法线端小、掠射端大）与软化只压掠射侧")
    lam = {x: lambda_(x, 0.5) for x in (1.0, 0.5, 0.1, 1e-3)}
    report.data["lambda_r05"] = lam
    report.check(
        "Λ 随 cosθ 减小而增大",
        lam[1.0] < lam[0.5] < lam[0.1] < lam[1e-3],
        "  ".join(f"Λ({x})={v:.6g}" for x, v in lam.items()),
    )
    k = {x: 1.0 + 2.0 * ((1.0 - x) ** 8) for x in (1.0, 0.5, 0.1, 1e-3)}
    report.data["softening_exponent"] = k
    report.check(
        "式(4) 的指数：法线入射 = 1（无软化）、掠射 → 3",
        abs(k[1.0] - 1.0) < 1e-12 and k[1e-3] > 2.9,
        "  ".join(f"k({x})={v:.6f}" for x, v in k.items()),
    )
    g_soft = geometry_term(0.05, 0.7, 0.5, soften=True)
    g_raw = geometry_term(0.05, 0.7, 0.5, soften=False)
    report.check(
        "软化把掠射受光侧的 G 压低（更软的 terminator）",
        g_soft < g_raw,
        f"G(NoL=0.05, NoV=0.7, r=0.5)：软化 {g_soft:.6g} < 未软化 {g_raw:.6g}",
    )

    # ---- 3. E_s 值域与剖面 ---------------------------------------------------------
    print("\n[3] E_s(NoV, r) 的值域与剖面（这是白炉的物理内容）")
    nov_samples = (0.05, 0.25, 0.5, 0.75, 1.0)
    r_samples = (0.05, 0.3, 0.6, 1.0)
    profile = {r: {nov: directional_albedo(nov, r, n_theta, n_phi) for nov in nov_samples} for r in r_samples}
    report.data["es_profile"] = {str(r): {str(n): v for n, v in row.items()} for r, row in profile.items()}
    worst = max(profile[r][nov] for r in r_samples for nov in nov_samples)
    report.check(
        "E_s ≤ 1",
        worst <= 1.0 + TOL_FURNACE,
        "最大 E_s = {:.6f}；NoV=1 剖面 {}".format(
            worst, ", ".join(f"r={r}: {profile[r][1.0]:.4f}" for r in r_samples)
        ),
    )
    report.check(
        "E_s > 0（每个方向都能拿到能量）",
        min(profile[r][nov] for r in r_samples for nov in nov_samples) > 0.0,
        "最小 E_s = {:.6f}；NoV=0.5 剖面 {}".format(
            min(profile[r][nov] for r in r_samples for nov in nov_samples),
            ", ".join(f"r={r}: {profile[r][0.5]:.4f}" for r in r_samples),
        ),
    )

    # ---- 4. 白炉：扣几次、扣哪一端 ------------------------------------------------
    print("\n[4] 白炉：直光两端取小。下一行的单端恒等式是代理，真正的半球积分在第 11 节")
    direct_worst, ibl_worst = 0.0, 0.0
    for r in r_samples:
        albedo = {x: directional_albedo(x, r, n_theta, n_phi) for x in nov_samples}
        for nov in nov_samples:
            for nol in nov_samples:
                direct_worst = max(direct_worst, min(1.0 - albedo[nol], 1.0 - albedo[nov]) + albedo[nov])
            ibl_worst = max(ibl_worst, (1.0 - albedo[nov]) + albedo[nov])
    report.check(
        "直光：`base·min(1−E(NoL),1−E(NoV)) + sheen ≤ 1`",
        direct_worst <= 1.0 + TOL_FURNACE,
        f"最大值 {direct_worst:.9f}（超 1 的量 {direct_worst - 1.0:+.2e}）",
    )
    report.check(
        "代数代理：`base·(1−E(NoV)) + E(NoV) ≡ 1`（这不是 shader 积分，见第 11 节）",
        abs(ibl_worst - 1.0) <= TOL_FURNACE,
        f"最大值 {ibl_worst:.9f}（偏差 {ibl_worst - 1.0:+.2e}）",
    )

    # ---- 5. 分层口径对照（S-27） --------------------------------------------------
    print("\n[5] S-27：分层口径的数值对照（NoL=0.4、NoV=0.9、r=0.5）")
    e_l = directional_albedo(0.4, 0.5, n_theta, n_phi)
    e_v = directional_albedo(0.9, 0.5, n_theta, n_phi)
    variants = {
        "min(1−E(NoL),1−E(NoV)) 正确": min(1.0 - e_l, 1.0 - e_v),
        "min(E) 误写": min(e_l, e_v),
        "直光误用单端 1−E(NoV)": 1.0 - e_v,
        "直光误用单端 1−E(NoL)": 1.0 - e_l,
    }
    for name, value in variants.items():
        print(f"       E(NoL)={e_l:.4f}  E(NoV)={e_v:.4f}   {name:<28} baseScale = {value:.4f}")
    report.data["layering_variants"] = variants
    report.check(
        "误写 min(E) 会把 base 压到正确口径的 20% 以下",
        variants["min(E) 误写"] < 0.2 * variants["min(1−E(NoL),1−E(NoV)) 正确"],
        "正确 {:.4f} vs min(E) {:.4f}".format(
            variants["min(1−E(NoL),1−E(NoV)) 正确"], variants["min(E) 误写"]
        ),
    )

    # ---- 6. 表不含颜色 ⇒ 分层必须乘 sheen 颜色 ------------------------------------
    print("\n[6] S-04：表存几何反照率 ⇒ 分层因子是 `1 − c·E`；漏乘颜色会过扣")
    for c in (1.0, 0.5, 0.2):
        with_color = (1.0 - c * e_v) + c * e_v
        without = (1.0 - e_v) + c * e_v
        print(f"       c={c:<4} `1−c·E` + c·E = {with_color:.6f}    `1−E` + c·E = {without:.6f}")
    c = 0.2
    report.check(
        "白炉只对 `1 − c·E` 闭合",
        abs((1.0 - c * e_v) + c * e_v - 1.0) < 1e-12 and (1.0 - e_v) + c * e_v < 1.0 - 1e-3,
        "c=0.2、E=%.4f：正确口径 %.6f，漏乘颜色 %.6f（暗 %.4f）"
        % (e_v, (1.0 - c * e_v) + c * e_v, (1.0 - e_v) + c * e_v, e_v - c * e_v),
    )

    # ---- 7. 两处读法的数值差异（S-32） --------------------------------------------
    print("\n[7] S-32：旧读法差多少（指数位置 / Λ 漏 exp / 软化 pow）")
    x_probe, r_probe = 0.25, 0.0
    l_ok = l_curve(x_probe, r_probe)
    l_legacy = l_curve(x_probe, r_probe, legacy_exponent=True)
    print(
        "      指数位置（r=0, x=0.25）：论文 L={:.4f}（Λ={:.4f}） vs 旧 L={:.4f}（Λ={:.4f}）".format(
            l_ok, math.exp(l_ok), l_legacy, math.exp(l_legacy)
        )
    )
    g_ok = geometry_term(0.5, 1.0, 0.5, soften=True)
    g_exp = geometry_term(0.5, 1.0, 0.5, soften=True, legacy_exponent=True)
    g_pow = geometry_term(0.5, 1.0, 0.5, soften=True, legacy_pow=True)
    print(
        "      G(NoL=0.5, NoV=1.0, r=0.5)：论文 {:.6g}；仅指数抄反 {:.6g}；旧写法（L 当 Λ + pow 软化）{:.6g}".format(
            g_ok, g_exp, g_pow
        )
    )
    report.data["legacy"] = {"G_correct": g_ok, "G_legacy_exponent": g_exp, "G_legacy_pow": g_pow}
    report.check(
        "论文读法的 G 落在 (0,1)",
        0.0 < g_ok < 1.0,
        f"G = {g_ok:.6f}",
    )
    report.check(
        "指数抄反（旧读法）让中段 G 相差 > 2 倍",
        max(g_ok, g_exp) / min(g_ok, g_exp) > 2.0,
        f"论文 {g_ok:.6f} vs 旧 {g_exp:.6f}（比值 {g_ok / g_exp:.3f}）",
    )
    report.check(
        "把 L 当 Λ 且软化写 pow(L,k) 会算出**负的 G**（无需犹豫即可判废）",
        g_pow < 0.0,
        f"旧写法的 G = {g_pow:.6g}（负值，物理上不可能）",
    )

    # ---- 8. LUT 规格（S-03）：轴参数化的扫描 --------------------------------------
    print("\n[8] S-03：LUT 轴参数化扫描（16×16；误差 = 双线性重建 vs 参考积分）")
    sweep_theta, sweep_phi = (64, 32)
    r_probes = (0.01, 0.02, 0.05, 0.1, 0.3, 0.6, 0.9)
    sweep_rows = []
    for nov_kind in ("cos", "cos_full", "sqrt", "angle"):
        for r_kind in ("linear", "sqrt"):
            for theta_max in (89.0, 85.0, 80.0):
                n = 16
                nov_axis = axis_samples(nov_kind, n, theta_max)
                r_axis = [(k + 0.5) / n for k in range(n)] if r_kind == "linear" else axis_samples("sqrt", n, 0.0)
                table = bake_lut(nov_axis, r_axis, sweep_theta, sweep_phi)
                nov_probes = [math.cos(math.radians(theta_max * f)) for f in (0.98, 0.9, 0.7, 0.4)]
                nov_probes += [0.25, 0.5, 0.75, 1.0]
                probes = [
                    (nov, r)
                    for nov in nov_probes
                    if nov >= math.cos(math.radians(theta_max))
                    for r in r_probes
                ]
                max_abs, at, max_rel, at_rel = lut_error(
                    nov_axis, r_axis, table, probes, sweep_theta, sweep_phi
                )
                # 域有效性：表里**所有格子**都必须满足 E_s ≤ 1（否则分层因子会变负）。
                # 这一列不能省：限域参数只有在真正作用到轴样本上时才算数。
                domain_ok = all(
                    directional_albedo(nov, r, sweep_theta, sweep_phi) <= 1.0 + TOL_FURNACE
                    for nov in nov_axis
                    for r in r_axis
                )
                sweep_rows.append((nov_kind, r_kind, theta_max, n, max_abs, at, max_rel, domain_ok))
                print(
                    "      {:<9} r={:<7} θmax={:<5.0f} max|ΔE|={:.5f} @NoV={:.4f},r={:.3f}"
                    "  域内 E_s≤1: {:<3} 最大相对 {:8.1f}%".format(
                        nov_kind, r_kind, theta_max, max_abs, at[0], at[1],
                        "是" if domain_ok else "否", 100.0 * max_rel
                    )
                )
    report.data["axis_sweep"] = [
        {"nov_axis": a, "r_axis": b, "theta_max": c, "n": d, "max_abs": e, "at": list(f),
         "max_rel": g, "domain_ok": h}
        for a, b, c, d, e, f, g, h in sweep_rows
    ]
    valid_rows = [row for row in sweep_rows if row[7]]
    best = min(valid_rows, key=lambda row: row[4])
    report.check(
        "存在 16×16 的轴参数化同时满足：域内 E_s ≤ 1 且 |ΔE| ≤ 0.02",
        best[4] <= 0.02,
        "最佳有效组合：NoV 轴 {}、r 轴 {}、θmax={:.0f} → max|ΔE|={:.5f} @NoV={:.4f},r={:.3f}".format(
            best[0], best[1], best[2], best[4], best[5][0], best[5][1]
        ),
    )
    report.check(
        "不限域的余弦轴（`NoV = t`）会用 E_s > 1 的格子（域无效），且深掠射误差 > 0.02",
        any(not row[7] for row in sweep_rows if row[0] == "cos_full"),
        "cos_full 的 θmax=89/85/80 三档域内 E_s≤1 判定："
        + ", ".join(str(row[7]) for row in sweep_rows if row[0] == "cos_full" and row[1] == "sqrt"),
    )
    if not args.quick:
        n = 32
        nov_axis = axis_samples(best[0], n, best[2])
        r_axis = [(k + 0.5) / n for k in range(n)] if best[1] == "linear" else axis_samples("sqrt", n, 0.0)
        table = bake_lut(nov_axis, r_axis, sweep_theta, sweep_phi)
        nov_probes = [math.cos(math.radians(best[2] * f)) for f in (0.98, 0.9, 0.7, 0.4)]
        nov_probes += [0.25, 0.5, 0.75, 1.0]
        probes = [
            (nov, r) for nov in nov_probes if nov >= math.cos(math.radians(best[2])) for r in r_probes
        ]
        max_abs32, at32, max_rel32, _ = lut_error(
            nov_axis, r_axis, table, probes, sweep_theta, sweep_phi
        )
        report.data["best_spec_n32"] = {"max_abs": max_abs32, "at": list(at32), "max_rel": max_rel32}
        print(
            "      同一参数化升到 32×32：max|ΔE|={:.5f} @NoV={:.4f},r={:.3f}".format(
                max_abs32, at32[0], at32[1]
            )
        )

    # ---- 9. E_s ≤ 1 的适用域（S-04 的前提） ---------------------------------------
    print("\n[9] S-04：E_s > 1 的适用域扫描（论文 Λ 拟合在深掠射角的已知行为）")
    scan_nov = (1e-4, 1e-3, 0.005, 0.01, 0.02, 0.05, 0.0872, 0.1736, 0.3, 0.5, 1.0)
    scan_r = (1e-3, 0.01, 0.05, 0.1, 0.3, 0.6, 1.0)
    onset = {}
    for r in scan_r:
        first_ok = None
        for nov in scan_nov:
            value = directional_albedo(nov, r, n_theta, n_phi)
            if value <= 1.0 and first_ok is None:
                first_ok = (nov, value)
        onset[str(r)] = first_ok
    report.data["es_le_one_onset"] = {k: (list(v) if v else None) for k, v in onset.items()}
    print("      r → E_s ≤ 1 所需的 NoV 下限（扫描栅格内）：")
    for r in scan_r:
        info = onset[str(r)]
        print(
            "        r={:<5} {}".format(
                r,
                "NoV ≥ {:.4f}（E_s={:.4f}）".format(info[0], info[1]) if info else "扫描域内始终 > 1（异常）",
            )
        )
    domain_nov = math.cos(math.radians(80.0))
    in_domain_ok = all(
        directional_albedo(max(nov, domain_nov), r, n_theta, n_phi) <= 1.0 + TOL_FURNACE
        for nov in (domain_nov, 0.3, 0.5, 1.0)
        for r in scan_r
    )
    report.check(
        "把轴域限制在 θv ≤ 80°（NoV ≥ 0.174）后 E_s ≤ 1 恒成立",
        in_domain_ok,
        "θmax=80°、r∈[1e-3,1] 上逐点检查通过 → 表内不需要 clamp，`1 − c·E ≥ 0` 随即可证",
    )
    clamp_cost = {}
    for r in (0.02, 0.05, 0.3, 0.9):
        e_edge = directional_albedo(math.cos(math.radians(80.0)), r, n_theta, n_phi)
        for theta in (85.0, 89.0):
            e_true = directional_albedo(math.cos(math.radians(theta)), r, n_theta, n_phi)
            clamp_cost[f"r={r},θv={theta:.0f}"] = e_true - e_edge
    report.data["clamp_cost"] = clamp_cost
    print(
        "      轴域外（θv>80°）按 clamp-to-edge 取 80° 行的代价 ΔE = "
        + "；".join(f"{k}: {v:+.4f}" for k, v in clamp_cost.items())
    )

    # ---- 10. 软化的能量让步（已知误差，不作失败判定） -----------------------------
    print("\n[10] 软化对 E_s 的影响（登记为已知误差：软化只减不增）")
    softening = {}
    for nov in (0.1, 0.5, 0.9):
        raw = directional_albedo(nov, 0.5, n_theta, n_phi, soften=False)
        soft = directional_albedo(nov, 0.5, n_theta, n_phi, soften=True)
        softening[str(nov)] = {"raw": raw, "soft": soft}
        print(f"       NoV={nov:<4} r=0.5：未软化 E_s={raw:.5f}   软化后 E_s={soft:.5f}   比值 {soft / raw:.4f}")
    report.data["softening"] = softening
    report.check(
        "软化后 E_s ≤ 未软化（表用未软化口径不会低估扣除量）",
        all(v["soft"] <= v["raw"] * (1.0 + 1e-9) for v in softening.values()),
        "三个 NoV 的软化/未软化比值：" + ", ".join(f"{v['soft'] / v['raw']:.4f}" for v in softening.values()),
    )

    # ---- 11. 间接光 = 直接光 BRDF 的半球积分（S-33） ------------------------------
    print("\n[11] S-33：均匀 radiance 下，间接光必须等于直接光 BRDF 的半球积分")
    identity = cosine_weighted_average(lambda _mu: 1.0, 64)
    report.check(
        "余弦加权约化：g=1 时 2∫μ dμ = 1",
        abs(identity - 1.0) <= 1e-12,
        f"得到 {identity:.12f}",
    )
    nov_probe, r_probe, n_mu = 0.9, 0.5, 48
    probes = {}
    for color in (1.0, 0.2):
        retain = base_retention(nov_probe, r_probe, color, n_mu, n_theta, n_phi)
        e_raw = directional_albedo(nov_probe, r_probe, n_theta, n_phi, soften=False)
        e_soft = directional_albedo(nov_probe, r_probe, n_theta, n_phi, soften=True)
        single = 1.0 - color * e_raw
        integrated = retain + color * e_soft
        proxy = single + color * e_raw
        probes[str(color)] = {
            "S": retain,
            "E": e_raw,
            "E_soft": e_soft,
            "single": single,
            "integrated": integrated,
            "proxy": proxy,
        }
        print(
            "       c={:.1f}  S={:.4f}  单端 1−cE={:.4f}  E†={:.4f}  "
            "积分 S+cE†={:.4f}  旧代理={:.4f}  差额={:.4f}".format(
                color, retain, single, e_soft, integrated, proxy, proxy - integrated
            )
        )
    report.data["indirect_integral"] = probes
    full = probes["1.0"]
    dark = probes["0.2"]
    report.check(
        "单位环境积分严格小于旧的单端代理，且仍 ≤ 1",
        full["integrated"] < full["proxy"] - 0.05 and full["integrated"] <= 1.0 + TOL_FURNACE,
        "c=1：积分 {:.4f}，旧代理 {:.4f}，差额 {:.4f}".format(
            full["integrated"], full["proxy"], full["proxy"] - full["integrated"]
        ),
    )
    report.check(
        "差额拆得开：入射侧 min 的多扣 + 软化少加的 sheen",
        abs((full["single"] - full["S"]) + (full["E"] - full["E_soft"]) - (full["proxy"] - full["integrated"])) <= 1e-9,
        "多扣 {:.4f} + 少加 {:.4f}".format(full["single"] - full["S"], full["E"] - full["E_soft"]),
    )
    report.check(
        "c=0.2 时积分仍用 c·E†，不是把 sheen 按白色 E 加回去",
        dark["integrated"] < 1.0 and abs(dark["integrated"] - (dark["S"] + 0.2 * dark["E_soft"])) <= 1e-12,
        "积分 {:.4f}；若 sheen 误用未乘颜色的 E† 会得到 {:.4f}".format(
            dark["integrated"], dark["S"] + dark["E_soft"]
        ),
    )

    print("\n" + "=" * 78)
    print(f"结果：{len(report.checks) - report.failed} 通过, {report.failed} 失败")
    print("=" * 78)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as handle:
            json.dump(
                {"checks": [{"name": n, "ok": ok, "detail": d} for n, ok, d in report.checks], "data": report.data},
                handle,
                ensure_ascii=False,
                indent=2,
            )
        print(f"JSON 已写入 {args.json}")
    return 1 if report.failed else 0


if __name__ == "__main__":
    sys.exit(main())
