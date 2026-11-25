# 头发求解器替换计划：Stable Cosserat Rods（V4）

状态：计划（plan 层，**不是契约**）。本文描述打算做什么、依据是什么、怎么验收、怎么回滚；
当前行为一律以 `contracts/` 与源码为准。本文的技术细节可能过时，不得当作现状引用。

日期 2026-11-24（2026-11-25 修订性能推导与验收口径）。目标算法：Jerry Hsu / Tongtong Wang / Kui Wu / Cem Yuksel,
"Stable Cosserat Rods", SIGGRAPH 2025, DOI [10.1145/3721238.3730618](https://doi.org/10.1145/3721238.3730618)。

---

## 0. 摘要

- 把 `cosseratImplicitV3` 的每根 guide 隐式牛顿 + 活跃集 KKT + 块循环约化，换成
  "位置步 = 局部 3×3 块下降（VBD）+ 朝向步 = 闭式 Gauss-Seidel 松弛 + 接触 = 逐顶点投影"，
  固定 4 次迭代/子步、无线性系统、无乘子。
- **它能买到什么**：每根 guide 每次迭代的固定工作量、以及每 workgroup 的静态 shared
  占用（两条 V3 管线现在都是 14,700 B、6 个驻留 workgroup，见 §2.5）。**它买不到并行度**：
  当前实现已经是每根 guide 一个 32 lane workgroup、guide 之间零同步（§2.1），不存在
  "全局求解器"可以拆掉。
- **核心前提现在有实测支撑（2026-11-25）**：当前成本的三分之二是耦合的 6×6 带状求解。84.53 ms
  的分解是 CR 57.0 ms（67%）、condensation 8.2 ms（10%）、其余 19.3 ms（23%，接触查询、
  残差、线搜索、状态装填与写回，§9.1）。位置/朝向拆分正是把 6×6 耦合换成两个 3×3 局部
  问题，所以计划的核心主张现在有实测支撑，比写作时更强。
- **它买不到什么**：那 23% 的其余成本。它是实测地板，先于 V4 的一条指令就给出收益上限
  **84.53/19.3 = 4.4×**；V4 自己的算术成本未知，中心估计 **约 3×**（84.53 → 约 28 ms），
  区间 **2–4.3×**。旧版的 5× 中心、2–10× 区间与 10–20× 乐观分支作废，因为它们建立在
  过时的 47 ms 基线与"CR 只占 12%"的旧读数上。失败分支是与现状持平甚至更慢（§9.2）。
- 四个必须先由人决定的取舍：模型身份（J = 0 会破坏"GPU 与 CPU 同一套方程"这条 G 门，
  §7.1）、`maxBlockDefect` 这个验收列没有动态范围（§7.2）、验收口径的变更（§7.3：
  允许少量穿透与非收敛 guide，不再要求零失败）、以及 V4 恒 4 轮会让现有"硬帧"门失效
  （§9.6）。
- **在写任何 shader 之前，先看 §11.2 的便宜实验**。其中"把 SDF 管线切到 shuffle CR"
  已于 2026-11-25 落地并计入 84.53 ms 基线；剩下的第 3 项（SDF 单次采样 + 解析梯度）
  仍可为 V3 单独落地，也是那 23% 里唯一还能单独压缩的大块。

### 0.1 本文纠正或补上了 9 条前提

简报中的若干前提经审计为错。下面每条都在正文给出源码行号，且标注我的判断：

| # | 简报前提 | 审计后的实际情况 | 处理 |
| --- | --- | --- | --- |
| 1 | 现状是"分布式全局带状求解器"，替换成局部方案能买到并行度 | 每根 guide 一个 workgroup（`rodV2Kernel.glsl:721-727`、`rodV2Kernel.glsl:7`、`rodGpuRecording.h:556`），guide 间无同步 | 已改写：只谈每 guide 工作量，不提并行度 |
| 2 | 每子步装配 10 自由度 KKT | 10 宽装配是**死代码**，活跃路径直接装配 6×6 + 接触行 | 已改写：不再称"装配 10×10"；死代码列入可删清单 |
| 3 | 轴向乘子行仍在系统里 | 该行被 `#ifndef VL_ROD_KKT10_AXIAL_LIMIT` 编译掉，没有任何 `.comp` 定义它 | 已改写：问题变成"没有不等式约束怎么守 1% 门" |
| 4 | 新方案可以保留摩擦 | 现状**完全没有摩擦**（全仓 grep 零命中） | 已改写：摩擦是新增行为，不在本期 |
| 5 | 静态 shared 14,700 B，6 个驻留 workgroup | 2026-11-25 的两行 shuffle CR 修复后，**两条 V3 管线都是 14,700 B、6 个驻留**；旧的 34,668 B / 2 驻留是修复前的读数 | 已改写：从 14,700 B 起算，且 occupancy 不再是 2→6 的增量 |
| 6 | 现状每子步几十轮牛顿，换成 4 轮能省很多 | 旧直方图（接触关闭、max-over-guides 门）是每子步 0.3–1.2 轮；接触默认开启、门改计 bad guides 之后**未重测**（§2.6） | 已改写：该论证降级为待测，不再作为收益依据 |
| 7 | — | 论文的 GPU 变体是 Jacobi，而单调能量保证只对 Gauss-Seidel 成立 | 新增风险项 R8 |
| 8 | — | 验收列 `maxBlockDefect` 恒等于收敛地板，没有动态范围 | 新增 §7.2 指标替换方案 |
| 9 | — | 硬件是 3070 Laptop（对 3090 约 2–2.5×），论文 7 ms 不含碰撞；2026-11-25 的分解给出 23% 的非求解地板 | 已改写：性能中心 3×、区间 2–4.3×，5× 不再可达 |

第 1、2、3、4 条我独立复核并同意。第 5 条已随 2026-11-25 的两行修复改变：SDF 管线补上
`VL_ROD_CR_SHUFFLE` 之后，`SC_tressfx_rod_*` 与 `SC_rod_v3_capsules_16` 跑的都是 14,700 B
的相同足迹（§2.5 有证据）。因此"验收场景只驻留 2 个 workgroup"这句作废，本计划也不再
把它当作收益来源。

---

## 1. 目标、非目标、回退

### 1.1 目标

1. 在 2000 模拟 guide / 40,334 渲染发丝的验收场景上，把
   `SC_tressfx_rod_sdf_animated` 的 `passes[hairSimulation].ms` 从 **实测 post-change V3
   基线 84.53 ms**（固定 4 子步、300 帧、CV 0.8–1.4%）降到 **≤ 0.45× 基线（≤ 38 ms）**，
   中心目标 **≈ 0.36×（≈ 30 ms）**；同一场景、同一子步数、同一测量窗口。默认子步下的
   参考读数是 57.9 ms/帧（16.7 ms 模拟时间/帧），它只作为量级，不作为门。
2. 把每 workgroup 的静态 shared 从 **14,700 B**（两条 V3 管线现状）降到 **≤ 2 KB**。
   注意 14,700 B 已经是 6 个驻留 workgroup，`102400 B / 足迹` 这条阶梯只剩 72 B 就跨到
   7 wg；≤ 2 KB 能否换到更多占用取决于寄存器文件与 warp 槽，**未知**（§9.4），所以这条
   目标降级为"不为 occupancy 设障"，不再宣称 2→6 的收益。
3. 保持所有对外可见的数据契约不变：`RodGuide` 状态布局、trace v3 文件、snapshot 侧车、
   诊断 CSV 前 10 列、follow 绑定、reset 语义、debt/退休策略（§4.2）。
4. 让 V3 继续可选中、可对比、可回退，直到 V4 在四条验收场景上稳定通过。
5. 验收口径按 §7.3 执行：静态场景保持零失败；动画场景允许少量非收敛 guide 与轻微穿透
   （owner 已裁定），但**整体运动不得比 post-change V3 更差**。

### 1.2 非目标

- 不加摩擦、不加自碰撞、不加新约束类型（这些是**新行为**，各自需要自己的验收）。
- 不改 guide 抽稀 / follow 绑定算法（`tressfxFollowBinding.*` 不动）。
- 不改渲染侧（strand 几何、着色、阴影）。
- 不改 1/120 s 步长与 4 子步上限（策略常量不动，§9.6 只讨论耦合）。
- 不删除 V3 的任何 shader，直到 V4 成为默认且 A/B 需求结束。
- 不追求 CPU/GPU 位级一致；只要求 §7.1 的模型身份决策被显式记录。

### 1.3 "V3 仍可回退"的真实机制（与简报的措辞不同）

简报说"通过现有 `solverProfile` 开关回退"。实际机制是两层：

- `simulation.solver` 是真正的选择器：`TressfxSimulationSolver` 有
  `Tressfx` / `CosseratXpbdV1` / `CosseratImplicitV3` 三个值（`source/mesh/meshAssetTypes.h:53-58`），
  由 `meshAssetValidator.cpp:182-194` 解析，字符串由 `tressfxBehavior.h:19-28` 给出。
- `simulation.solverProfile` 目前**只有一个枚举值** `EffectiveBundleV1`
  （`source/mesh/meshAssetTypes.h:60-63`），且被硬编码在 5 处：
  `meshAssetValidator.cpp:196-201`（非此值直接抛错）、`meshAssetValidator.cpp:1321`、
  `tressfxGroupSimulation.cpp:95`、`source/editor/ui/tressfxGroupEditor.cpp:381`、
  `schema/tressfx_behavior.schema.json:11`。

因此新增 V4 需要在 7 处同步：枚举、名字函数、算法版本、schema 两处枚举、validator 两处
解析/拒绝、group identity、编辑器下拉（`source/editor/ui/scriptComponentControls.h:202-207`）、
以及 `tressfxGroupSimulation.h:22` 的字段白名单。**这是可用性成本，不是物理成本**，
但在 Stage 4 之前必须全部落地，否则资产无法选择 V4。

回退方式：`simulation.solver` 保持 `cosseratImplicitV3`（或新场景写死 V3）即回到现状；
因为本文不删任何 V3 shader（§4.3），回退不需要重新编译任何东西，只换一个 JSON 值。

---

## 2. 现状（全部可核对）

### 2.1 不是全局求解器

`rodV2Kernel.glsl:7` 是 `layout(local_size_x = 32) in;`；`rodV2Kernel.glsl:723-727` 取
`slot = gl_WorkGroupID.x`、`lane = gl_LocalInvocationID.x`；`rodGpuRecording.h:556` 的
dispatch 是 `solver->Dispatch(commandBuffer, state.guides, 1, 1)`。即 2000 guides = 2000 个
互不通信的 workgroup，每 warp 一根 32 点发丝。所谓"分布式块循环约化"
（`rodKkt10BandSolve.glsl:1170-1203`）只在**一根发丝的 32 个块**之间分布，仍是 warp 内
通信，不是跨 guide 的全局系统。

结论：任何"局部化能换来并行度"的论证在这里是错的。V4 只能减少每 guide 的工作量。

### 2.2 活跃路径不是 10 自由度装配

`shader/glsl/hair/rodV3Kkt10.glsl:36-178` 里的 `RodKkt10Embed` / `RodKkt10AddPosition` /
`RodKkt10AssembleRow` / `RodKkt10PivotSolve` 在任何 `.comp`/`.glsl` 中**没有调用点**
（全仓 grep 只命中定义与注释）。活跃路径是 `RodKkt10Condense`
（`rodKkt10BandSolve.glsl:918-1090`）：直接装配 6×6 位置块 + 最多 3 个接触行，
`RodV2Inverse` 求 6×6 逆，构造 `S = GᵀA⁻¹G`、`Q = I − G S⁻¹ Gᵀ A⁻¹`，再把 32 个 6×6 块
交给 `RodCrSolveBandShuffle`（`rodKkt10BandSolve.glsl:1187`）。

源码给出的量级：压缩路径把"每 warp 指令数"从 10 宽路径的约 83k 降到约 12k
（`rodKkt10BandSolve.glsl:1128-1131`）。**这 12k 是 V4 要打败的对手**；2026-11-25 的分解
给出它对应的墙钟份额是 77%（§9.2）。

### 2.3 轴向行已被编译掉，拉伸本来就是软约束

`RodKkt10UpdateAxial`（`rodKkt10BandSolve.glsl:488-521`）在 `#ifndef VL_ROD_KKT10_AXIAL_LIMIT`
下直接 `return`，而没有任何 `.comp` 定义该宏（`rodV3ImplicitCrCompensated.comp`、
`rodV3ImplicitSdf.comp`、`rodV3ImplicitCr.comp` 都没有）。压缩解也无条件写
`direction.values[9] = 0.0`（`rodKkt10BandSolve.glsl:1213`）。

拉伸本身是弹性力：`RodV2Elastic` 用 `resultant = constraint / (compliance · restLength)`
（`rodV2Kernel.glsl:219-220`）合成线力与力矩，compliance 来自
`rodKernel.glsl:323-324` 的 `1/(l·A·E)`。所以"去掉轴向乘子行"**不是**一个新问题——
现在就已经没有它了。真正的开放问题是 §6.1：没有不等式约束时怎么守住 1% 那条 G 门。

### 2.4 接触没有摩擦

`friction` 在 `source/hair/**` 与 `shader/glsl/hair/**` 中零命中。接触完全是几何的、
位置层面的、与速率无关的：`RodKkt10CapsuleDistance` 返回点到线段距离减半径，
`RodKkt10ContactGap` 返回几何 gap 与单位法向（`rodKkt10BandSolve.glsl:123-345`），
KKT 里只有法向行。因此"V4 保留摩擦"这个说法不成立；加摩擦是**新行为**，见 §5.3。

### 2.5 静态 shared 足迹与驻留 workgroup 数

我用 `artifacts/tmp/spirv_shared.py` 对已发布 SPIR-V 逐个求 `Workgroup` 总量：

| 管线 | 对应场景 | 静态 shared | 102400 B ÷ 足迹 |
| --- | --- | --- | --- |
| `rodV3ImplicitCrCompensated` | `SC_rod_v3_capsules_16/2000`（无 `hairCollision.mesh`） | **14,700 B** | 6 |
| `rodV3ImplicitSdf` | `SC_tressfx_rod_{,sdf_}{static,animated}`（声明了 mesh） | **14,700 B**（2026-11-25 修复后） | **6** |
| `rodV3ImplicitCr` | 无 pipeline：只在 `shader/spv/shader-build-cache.json` 有记录，源码里没有 `CreateComputePipelineCandidate` 引用 | 30,616 B | 3 |
| `rodSolve`（V1） | `cosseratXpbdV1` | 13,240 B | 7 |
| `rodSolveSdf`（V1+SDF） | `cosseratXpbdV1` + mesh | 13,368 B | 7 |

"102400 ÷ 足迹"这一列与 `../../rationale/hair-rod-gpu-v3-contact-20261123.md:2208-2213`
实测推出的阈值表完全同构（17066=102400/6、14628=/7、12800=/8、11377=/9、10240=/10），
所以它是 100 KiB carveout 下的直接算术，不是新测量。

2026-11-25 的两行修复给 `rodV3ImplicitSdf.comp` 补上了 `#extension
GL_KHR_shader_subgroup_shuffle` 与 `#define VL_ROD_CR_SHUFFLE`（此前只有
`rodV3ImplicitCrCompensated.comp` 有），SDF 管线的足迹从 34,668 B 降到 **14,700 B**，与胶囊
管线相同，驻留从 2 个 workgroup 变成 **6 个**。因此：

- 旧版"验收场景只有 2 个驻留 workgroup"的推论作废（§9.4 已重写）；
- 旧版 §11.2 #2 的"19,968 B 可以白拿"**已经拿掉**，并计入 84.53 ms 基线；它不再是本计划
  的预留收益；
- 记录里 7.5%/29.3% 的 shuffle 收益现在两条管线都收了，但那只是那一次非 SDF 测量的量级，
  不能当成新基线的分解。

### 2.6 稳态迭代数（我实测的每 guide 直方图）

用 `artifacts/tmp/iter_hist.py` 解 `.rod-diagnostics.bin`（64 B/guide，字段 3 =
`v2Iterations`，字段 12 = 每 guide 已接受仿真秒数；布局见
`shader/glsl/hair/rodCommon.glsl:7-24` 与 `hairSimulationPassTrace.cpp:232-234`）：

| 快照 | 场景/子步 | guides | 失败 | 总迭代 | 每 guide 每帧均值 | 直方图（迭代数: guide 数） |
| --- | --- | --- | --- | --- | --- | --- |
| `snap-bind-fixed1-f240` | bind，1 固定子步 | 2000 | 3 | 607 | **0.30** | 0:1393, 1:607 |
| `snap-anim-fixed8-f400` | 动画，8 固定子步 | 2000 | 2 | **19,234** | **9.62**（即每子步 1.20） | 0:2, 8:1546, 9–34:452 |
| `snap-bind-new-f400` | bind，自适应 | 2000 | 2 | 4,992 | 2.50 | 0:1390, 8:608, 64:2 |
| `snap-bind-legacy-f400` | bind，旧 1/240×8 | 2000 | 2 | 736 | 0.37 | 0:1390, 1:608, 64:2 |

这张表同时解释了简报里那个 19,234：它就是"动画 + 8 强制子步"下的一帧总 guide-iteration。
**稳态不是几十轮**：绝大多数 guide 每子步只走 1 步牛顿；bind 场景的总轮数几乎全在那两根
卡住的 guide 上（64 轮），动画场景则是 8 子步 × 每子步约 1.2 轮的累加（分布 8–34，没有
64 轮长尾）。所以 V4 的收益**不能**用"4 轮换 64 轮"论证。

**2026-11-25 的重要限定**：上表采于真实 SDF 接触默认开启之前，当时 SDF 接触被单位 bug
静默关闭，且子步门是 max-over-guides（动画场景只跑 1 子步/帧）。现在接触默认开启、门改计
"超过 12 轮的 guide 比例"（超过 1% 才降载），动画场景跑 4 子步/帧；**当前配置下每子步的
牛顿轮数没有重测**。所以这张表只能作为"接触关闭时"的历史证据，不能用来论证 V4 的固定 4 轮；
§9 的收益推导也不再用它。

### 2.7 `maxBlockDefect` 没有动态范围

验收每一行读的 `maxBlockDefect` 是 `diagnostic[1] = v2Maximum`
（`rodGpuRecording.h:213`，写入见 `rodV2Kernel.glsl:1288`），即"32 点 × 10 分量里最大的
归一化分量残差"（`rodKkt10BandSolve.glsl:745-757, 805-811`）。而收敛判据是
`max(1e-4, RodKkt10ConvergenceFloor)`，`RodKkt10ConvergenceFloor = 1.0e-2`
（`rodKkt10BandSolve.glsl:49`、`rodV2Kernel.glsl:1019-1020`）。

于是静态基线 0.009791187942 / 0.00996868033 / 0.008789401501 全都**贴在地板上**——它们
能证明"收敛到了地板"，不能证明"解是对的"，更不能跨模型比较。这是 §7.2 要替换指标的根因。
动画场景在 4 子步下不再贴地板：一次实测是 0.28709 / 0.29802（各 4 / 13 根失败 guide），
最终 gate 的最后一行为 0.009989947、2 个瞬态失败行。所以"全都贴地板"只对静态成立；动画的
残差虽然有动态范围，但定义与 V4 不同，仍不能跨模型比较。

---

## 3. 目标算法（我们的术语）

### 3.1 符号与可用数据

一根 guide = 32 点（`x_i`, i = 0..31）、31 段（`q_i`, i = 0..30，段 i 接在点 i 之后）。
每根 guide 的持久状态已经存在 `RodGuide`（`shader/glsl/hair/rodCommon.glsl:7-24`）里：

| 现有字段 | 含义 | V4 用途 |
| --- | --- | --- |
| `points[i].position` | 位置（米，相对固定世界原点） | 位置未知量 |
| `points[i].velocity` | 速度 | 预测项 `y = x + h v + h²a` |
| `points[i].rootLocal` | 点在本段 rest frame 下的偏移 | 根部锚定目标（§6.2） |
| `points[i].massSupport` | `(mass, stiffness, damping, 0)` | 惯性项质量 |
| `segments[i].orientation` | 段朝向 | 朝向未知量 |
| `segments[i].restOrientation` | rest 朝向 | 弯曲 rest |
| `segments[i].restStrainLength` | `xyz` rest 应变、`w` rest 段长 | 拉伸 rest、段长 |
| `segments[i].restJointLength` | rest 相对转角、平均段长 | 弯曲 rest |
| `segments[i].shearCompliance` | `1/(l·A·E)` 等 | 拉伸刚度（沿用现有约定） |
| `segments[i].bendCompliance` | 弯曲/扭转 compliance | 弯曲刚度 |
| `segments[i].inertia` | 段惯量 | **V4 不用**（J = 0） |

V4 **不需要新 UBO 字段、不需要新 SSBO 字段**。这是它能保住 §4.2 那些格式契约的原因。

### 3.2 时间步结构

论文 Algorithm 2（[全文 TEI](https://par.nsf.gov/biblio/10668229) 的 §4）：
每个时间步先按 VBD 初始化 `x ← x^t + h v^t + h² ã`，然后固定 `N` 次交替迭代
（先位置、后朝向）。

我们映射成：每个子步（h = 1/120 s，`RodGpuStepSeconds()`，`rodGpuRuntime.h:39-47`）内

```
预测:  y_i = x_i + h v_i + h² a_ext          (a_ext = 重力 + 风 + probe，同 §3.2 现有 externalAcceleration)
重复 N 次 (N = 4 默认；论文值，离散相关，见 §9.5 / Q9):
  A. 位置步:  对所有自由点并行做一次 3×3 局部块更新（Jacobi，双缓冲）
  B. 朝向步:  对所有段并行做一次闭式更新（Jacobi，双缓冲）
发布:  v_i = (x_i − x_i^old)/h，ω_i = log(q_i^old⁻¹ q_i)/h   （与现状同一发布公式）
```

子步循环仍然留在 kernel 内（现状 `rodV2Kernel.glsl:833`，一次 dispatch 跑完
`rod.counts.y` 个子步），因此每帧仍是每对象 1 次 dispatch，trace 的 `dispatchCount`
不变。

### 3.3 位置步

固定 q 后，拉伸能是位置的二次函数（论文 §4 "the energy is reduced to be quadratic
without the orientation component, the Hessian becomes the simple diagonal matrix"）：

```
E_ss,i(x) = |C_ss,i|² / (2 c_i l_i)
C_ss,i    = R(q_i)ᵀ (x_{i+1} − x_i) / l_i − s_i          (s_i = rest 应变, l_i = rest 段长)
c_i       = segments[i].shearCompliance
```

`C_ss,i` 的形式与现有 `RodV2StableShear` 一致（`rodV2Kernel.glsl:183-185`），
`c_i l_i` 的分母与现有 `resultant = constraint/(compliance·l)` 一致
（`rodV2Kernel.glsl:219-220`）——**刚度单位因此与 V3 完全相同**，这是"V4 不会悄悄改硬"
的唯一保证，必须逐项核对。

点 i 的局部二次型：

```
H_i = (m_i / h²) I + Σ_{e ∈ {i−1, i}} H_edge,e [+ H_contact,i]
g_i = (m_i / h²)(x_i − y_i) + Σ_e (∂C_e/∂x_i)ᵀ (C_e / (c_e l_e)) [+ g_contact,i]
x_i ← x_i − H_i⁻¹ g_i          (3×3 解; 只含惯性+拉伸时 H_i 是对角阵 → 3 次除法)
```

`H_edge,e` 的具体常数必须由 `c_i l_i` 约定推出（`∂C/∂x = Rᵀ/l`）。**2026-11-25 已由
作者 VBD 实现确认**：`vbd.cpp:36-42` 给出 `H_edge = (κ_i/l_i²)·I`、`g_edge = ±(κ_i/l_i)·C`，
所以只含惯性+拉伸时 H 是各向同性对角阵，3 次除法即可解（见 §12 Q2）。

### 3.4 朝向步

设 `v` 为该段自身拉伸的力矩四元数、`b` 为接在该段上所有弯曲约束的力矩之和
（论文 Eq. 15/16；单链即 `b = b_{i−1} + b_i`，"b becomes the sum of all bending
constraint contributions"）。论文 Eq. 18 + Eq. 22：

```
λ = |v| + |b|
q_i ← normalize( v ⊗ b ⊗ e₃ + λ b )
```

这一式已与作者实现核对（2026-11-25）：`StableCosseratRods/KittenEngine/Cosserat/sim/lambda.cpp:41`
就是 `normalize((Rotor(v) * Rotor(b) * Rotor(1)).v + lambda * b)`。`v` / `b` 的代数形式在
论文 Eq. 15/16 中被 TEI 剥掉，实现给出 `v = -(2 k_ss,i / l_i) · edge`（`:15`，纯四元数）与
`b = Σ_j k_bt,j · φ_j · (q_other ⊗ q_rest_relative)`（`:17-35`，全四元数），并与论文 §3.3 的
零应变推论 `v = -|v| q e₃ q̄`、`b = |b| q` 一致。注意作者代码的 `Rotor::v` 是**整个 vec4**，
所以 `φ` 用四分量点积、不是三分量；这一点决定零应变时 `φ = +1` 而不是 `-1`（写错会让
零应变变成 `φ = -1`，b 反号、rest 形状不再是不动点）。离线原型
（`artifacts/tmp/v4orient/`）按同一式实现，复现了零应变不动点与四元数范数。

其中 `⊗` 是四元数乘法。**注意简报把这一式写成 `normalize(v e3 + λ b)`，漏掉了一个 `b`
因子**；按论文原文与 §3.5 的自洽性推导，第一项是 `v b e₃`。丢掉 `b` 会同时改变旋转轴
和两项的相对权重，不是等价写法。

我对该式做过一次自洽性验算（可在评审时复查）：零应变时有 `v = −|v| q e₃ q̄`、
`b = |b| q`（论文 Eq. 15/16 的零应变推论），代入得

```
v b e₃ + λ b = |b|(λ + |v|) q ,      λ² − |v|² = (λ − |v|)(λ + |v|)
⇒ q_i(λ) = [v b e₃ + λ b] / (λ² − |v|²) = |b| q / (λ − |v|) = q     (当 λ = |v| + |b|)
```

即论文的完整式 `q_i(λ) = (v b e₃ + λ b)/(λ² − |v|²)` 在零应变下**不需要归一化就精确**
（论文 §3.3 的结论），而归一化只是为有应变时兜底。因为 `λ² − |v|² > 0` 是正标量，
"先除再归一化"与"直接归一化分子"等价——这正是 `normalize(v b e₃ + λ b)` 成立的理由，
也是"不能省掉 b"的证据。

论文同时证明 `λ = |v| + |b|` 是最大正根的上界、且在零应变时与精确解重合
（Eq. 21–22），并给出单调性论证：每次朝向更新都让全局能量对该自由度单调下降。

### 3.5 精确 λ（可选，默认不用）

论文 Algorithm 1 与我由 `|q(λ)| = 1` 反解的结论一致：

```
λ ← |v| + clamp(γ_i, 1e-3, 1)|b|
λ ← sqrt(|v b e₃ + λ b| + |v|²)        (由 |q(λ)|=1 即 λ² − |v|² = |v b e₃ + λ b| 得到)
γ_i ← (λ − |v|)/|b|
```

按 `shader/glsl/hair/rodCommon.glsl` 的现有字段，`γ_i` 需要每段一个额外持久标量，
但不改 `RodGuide` 布局的话没有地方放（`RodGuide` 是固定结构，改动会破坏
`RodGpuGuideVec4Count = 412` 与两个 Python 脚本，见 R4）。论文自述近似版"在全部测试里
精度足够"（§5.1：近似版单位四元数约束 MSE 5e-12，精确版 8e-14，平均收敛 γ = 0.99999）。
我们的 `qnorm` 门（[hair-acceptance-metrics.md](hair-acceptance-metrics.md):39）在归一化
之后自动满足——归一化本身把 `|q|` 置 1，代价是把误差推到"旋转量"上而不是范数上。
**结论：V4 v1 只用近似版，精确版留作后续可选项。**

### 3.6 Jacobi 与 Gauss-Seidel 的取舍（风险 R8）

论文的理论保证（§3.2–3.3：每次更新使全局能量单调下降）是**对 Gauss-Seidel 逐段松弛**
成立的；论文 §5.7 说 GPU 实现"只要位置与朝向分成两个 pass，简单 Jacobi 就够了"。
这两句不是同一件事：Jacobi 并行更新会破坏单调性证明。

我们的处境：一根发丝就是一个 32 宽 warp，warp 内做 Gauss-Seidel 等于串行 31 步，
代价太高。因此 v1 走 Jacobi（双缓冲 + `barrier()`），并把"能量是否单调"变成一个
**被测量的量**而不是被假设的性质（§7.2 的 M4）。如果 M4 在难帧上不成立，退路是
红黑/两色更新，或最后一轮改串行。

---

## 4. 代码映射表

### 4.1 替换

| 现有 | 位置 | 被什么替换 |
| --- | --- | --- |
| V3 kernel 主循环（子步/牛顿/信任域/回溯） | `shader/glsl/hair/rodV2Kernel.glsl:721-1324` | 新增 `shader/glsl/hair/rodV4Kernel.glsl`（V3 不删） |
| 6×6 压缩 + 乘子恢复 | `rodKkt10BandSolve.glsl:918-1215` | 3×3 局部解（§3.3） |
| 32 块 CR | `RodCrSolveBandShuffle`（`rodKkt10BandSolve.glsl:1187`） | 无（没有线性系统） |
| 活跃集（3 槽 / 9 候选 / 依赖剔除） | `rodKkt10BandSolve.glsl:346-485` | 每 guide 的胶囊候选掩码 + 逐顶点投影（§5.2） |
| 重叠修复 | `RodKkt10RepairOverlap`（`rodKkt10BandSolve.glsl:668-736`） | 每次位置更新后的投影（保留其不变式，§5.2） |
| merit / 线搜索 | `RodKkt10Measure`（`:745`）+ `rodV2Kernel.glsl:1126-1175` | 无；改为每子步测一次势残差（§7.2 M4） |
| 6×6 切向装配 | `RodV2ElasticDerivative`（`rodV2Kernel.glsl:249`） | 3×3 对角块 + 四元数导数 |
| pipeline 表 | `hairSimulationPass.cpp:89-97`（`rodPipelines[7]`） | 扩到 9：新增 `rodV4Stable` / `rodV4StableSdf` |

### 4.2 存活（一行不改）

| 资产 | 证据 |
| --- | --- |
| 接触几何缓存 `RodKkt10ContactGap` 与其 9 槽缓存 | `rodKkt10BandSolve.glsl:244-345`；V4 直接调用，只加"每 guide 掩码"参数 |
| 胶囊几何 / 胶囊解析 | `source/hair/tressfxCollision.h`（`TressfxMaxColliders = 8`，`:15`） |
| SDF 场查询与量纲归一 | `RodKkt10SdfQuery`（`rodKkt10BandSolve.glsl:184-241`）——单位法向 + `rawGap/|grad|` 归一，必须原样保留 |
| SDF 构建/代理/蒙皮 | `source/hair/tressfxSdfGrid.*`、`sdfBuilder.*`、`collisionProxy.*`、`tressfxProxySkinning.*` |
| 每 guide 状态布局 | `RodGuide`（`rodCommon.glsl:7-24`）、`RodGpuGuideVec4Count = 412`（`rodGpuRuntime.h:89`） |
| pose 历史与采样 | `source/hair/tressfxSimulationHistory.h`、`RodGpuObject::intervals/Sample`（`rodGpuRuntime.h:270-327`）、`params.roots[9]` 采样（`rodGpuRecording.h:412-416`） |
| reset 路径 | `RodInitialize`（`rodKernel.glsl:253-330`）、`ClearTimeline`（`rodGpuRuntime.h:283-305`）、`flags.x`、`RequestReset`（`hairSimulationPass.cpp:147-152`） |
| follow 绑定与插值 | `tressfxFollowBinding.*`、`rodFollowKernel.glsl`、pipeline `[5]` |
| 调度与债务 | `rodGpuRuntime.h:25-104`（1/120 s、4 子步、33.3 ms 退休、0.5 s 债务） |
| 诊断 CSV（14 列，前 10 列定序） | `rodGpuRecording.h:259-272` |
| trace v3 / snapshot 侧车 | `hairTraceFormat.h:22-45`、`hairSimulationPassTrace.cpp:55-284` |
| 根点/作者固定点语义 | `rodKernel.glsl:292-296`（`inverseMass == 0` 即不动点） |

### 4.3 删除

| 对象 | 条件 | 理由 |
| --- | --- | --- |
| `rodV3Kkt10.glsl:36-178`（`RodKkt10Zero/Embed/AddPosition/AssembleRow/PivotSolve`） | **可立即删**，与 V4 无关 | 无调用点的死代码（§2.2）；删后只剩结构体，其中 `RodKkt10Edge`/`RodKkt10Contact` 仍被活跃路径使用（`rodKkt10BandSolve.glsl:527`、`:574`） |
| `rodCrBand.glsl`（shared CR，约 20 KB） | Stage 3 之后 | SDF 管线目前还在用它（§2.5），要等 V4 SDF 或 shuffle 化之后 |
| `VL_ROD_KKT10_CONDENSE_PROBE` / `*_DOUBLE` / `KKT10_DEBUG` 探针块 | Stage 4 | 只在 `#define` 下编译，A/B 需求结束后可删 |
| V3 kernel 本体 | **不删** | 它是回退路径（§1.3） |

---

## 5. 接触

### 5.1 现状

每点最多 3 个活跃接触槽，候选 9 个 = 8 胶囊 + 1 个 SDF 场（id 8），
`RodKkt10MaxContactCandidates = 9`（`rodKkt10BandSolve.glsl:75`）。每个槽是一行法向约束：
RHS 为 `gap / lengthScale`，乘子单位是牛顿（`rodKkt10BandSolve.glsl:18-22`）。
依赖法向先按 `|dot|` 判平行/反平行再做 Gram-Schmidt 剔除（`:350-371`），否则位置块丢秩
（注释按 10×10 写，活跃路径是 6×6）。子步开始时另有 `RodKkt10RepairOverlap` 做位置投影
修复（`:668-736`），其触发阈值是总长 1%、目标是把点"推到贴面"。

### 5.2 V4 取代方案：每 guide 粗筛 + 逐顶点投影

1. **每帧每 guide 一次胶囊粗筛**：用 guide 当前 AABB + 胶囊半径包络得到一个 8 bit 掩码，
   mask 在整个 dispatch 内对 warp 是**一致**的（不随 lane 变化）。被筛掉的胶囊对任何点
   的贡献恒为零（距离 > 半径 ⇒ gap > 0 ⇒ 无约束），所以这是**精确**裁剪，不是近似。
2. **每次位置更新后逐顶点投影**：对自由点求 `gap`（id 在掩码内），若 `gap < 0` 则
   `x_i += n · (−gap + ε)`，`n` 用单位法向。多槽重叠时按现有 `RodKkt10ContactGap` 的
   id 顺序处理（与 V1 `tressfxSimCollision.comp` 的"按作者顺序逐个应用"同语义）。
3. **SDF 查询改单次三线性 + 解析梯度**：现在一次查询是 7 次三线性采样
   （`rodKkt10BandSolve.glsl:196-214`：1 次基准 + 6 次差分）。改成 1 次三线性读出
   8 个角点值，再用三线性插值函数自身的解析梯度求法向——对**同一个**三线性插值面，
   解析梯度比中心差分更一致，且省 6 次采样。这是**能为 V3 单独落地**的优化（§11.2 #3）。
4. **不引入 IPC barrier**（论文 §4 用法）。理由：barrier 会引入 `k`、厚度 `r`、
   法向/摩擦耦合和一整套新的数值参数，而我们的接触是解析胶囊 + 现成 SDF，
   V3 的 `RepairOverlap` 已经证明"位置投影"足够维持不穿透
   （记录里的不变式："自由点最小胶囊 gap = -0.0000 cm"）。
   代价在 §5.4。

### 5.3 摩擦：本期不做

现状没有摩擦（§2.4），所以"V4 保持摩擦"是不成立的表述。论文的摩擦
（`k_friction = min(k_friction, μ |δx⊥|⁻¹ n·f_collision)`，[全文](https://par.nsf.gov/biblio/10668229) §4 Friction）
需要**接触力** `f_collision`；纯投影方案里没有这个量，所以摩擦与 barrier 是绑在一起的。
结论：V4 v1 不加油摩擦；若后续要加，必须走 barrier 变体并单独立验收。

### 5.4 保真度损失清单（必须写进验收结论）

| 损失 | 机制 | 可观察后果 | 对冲 |
| --- | --- | --- | --- |
| 无法向冲量记账 | 投影改位置，速度由 `(x−x_old)/h` 反推（`rodV2Kernel.glsl:1200`） | 推出速度可能注入能量；贴面抖动 | 投影放进迭代内（让弹性步吸收），并把法向速度单向 clamp 列为**可选**新行为（需独立验收） |
| 无静摩擦 | 无切向行 | 发丝沿胶囊面自由滑，造型漂移 | 记录为已知差异；不在 v1 修 |
| 无接触刚度 | 无乘子 | 快速运动下"先沉后弹"，穿透深度不受控 | 每子步末强制投影（保证发布状态不穿透） |
| 无互补条件 | 无 KKT | `gap ≥ 0 ∧ λ ≥ 0` 不再被求解，只是被投影近似 | 指标 M1（以同场景 V3 为基线，§7.3）替代 |
| 依赖法向剔除消失 | 无 3 槽上限 | 多接触点会做多次投影（顺序依赖） | 粗筛 + 固定 id 顺序保证确定性 |

---

## 6. 拉伸不等式与根部锚定

### 6.1 1% 上拉伸这条 G 门怎么办

事实：GPU 侧现在**没有**不等式约束（§2.3），1% 这条 G 门只在 CPU 验证工具里判
（[hair-acceptance-metrics.md](hair-acceptance-metrics.md):38 的 `stretch` 行，阈值 1%，
实测记录是 `≤1.0000000321%`）。CPU 侧常量为 `MaximumTensileStrain = 0.01`
（`source/hair/rod/rodAxialLimit.h:8`）。所以现状是靠"软拉伸已经足够硬"在守门，
而不是靠约束。评审给的余量是实测最大应变 0.9495% 对 1.0% 上限——**余量只有 0.05 个
百分点**，非常薄。

V4 的对策（按优先级）：

1. **沿用同一 compliance**（§3.3），使静态刚度与 V3 相同。这是必要条件，不是充分条件。
2. **每子步末一次不等式投影**：对每条边，若 `|x_{i+1} − x_i| > (1+0.01) l_i`，把超出的
   长度在两个自由端点之间对半修正（PBD 式，不动 q）。代价是每点 2 次减法 + 1 次比较，
   且**只在上拉伸时触发**。这条投影就是被编译掉的轴向行在位置层面的等价物：
   `RodKkt10UpdateAxial` 的 gap 定义正是 `(1 + ε) l − |Δx|`（`rodKkt10BandSolve.glsl:507`）。
3. **把 `stretch` 变成每次 GPU 验收都测的量**（§7.2 M2），而不是只在 CPU 工具里测。

我建议 v1 **同时**上 1 和 2，并把 2 做成可关闭的宏，用 A/B 证明它是否必要。
只上 1 的风险是：Jacobi + 4 次迭代下应变峰值可能超过 V3，而这种失败**不会**
在 `maxBlockDefect` 里表现出来（§2.7）。

### 6.2 根部锚定（Dirichlet，无乘子行）

现状：根点是 `inverseMass == 0` 的钉住点（`rodKernel.glsl:292-296`），作者也可把任意点
标成不动点（`restPositions.positions[point].w == 0`）。子步内的目标由
`params.roots[substep]` 与 `params.roots[substep+1]` 线性/刚体插值给出
（`rodV2Kernel.glsl:852-876`：`firstAnchor`/`lastAnchor`/`omegaWorld`/`rootLocals`）。

V4 的做法：钉住点的 `x_i` 直接写 `lastAnchor + R(lastRoot)·q_rest·rootLocal_i`，
不参与 §3.3 的更新；`q_0` 由根帧增量给出（与 `rodV2Kernel.glsl:907-913` 同一构造）。
因为乘子的**唯一对外用途是诊断**（`rodKkt10BandSolve.glsl:788-789` 把 `max|λ|/F` 写进
调试通道；求解内部另有装配与线搜索读取，但不发布），删掉乘子**不损失任何对外可见量**
——这是"删轴向行"真正便宜的地方。钉住点也因此不需要反力，V4 不做任何约束反力记账。

### 6.3 follow 绑定

follow 用 `RodPointFrame(segments[].orientation, restOrientation)` 造帧
（`rodFollowKernel.glsl:82-107`），再用 rest-frame offset 反推渲染点。只要 V4 继续把
**绝对朝向**写进 `segments[i].orientation`（现状见 `rodV2Kernel.glsl:1247-1252` 的
`restFrame * orientation` 组合），follow 与 pipeline `[5]` 一行不用改。
风险：V4 若改成"直接存绝对 q"，`frameDeltas` 与 `displacementReferences` 的语义就变了，
follow 会静默错位（R2）。

---

## 7. 模型身份与验收口径（必须先决策）

### 7.1 J = 0 会破坏"GPU 与 CPU 同一套方程"这条 G 门

[hair-acceptance-metrics.md](hair-acceptance-metrics.md):40 把"GPU 与 CPU 必须是同一套方程"
登记为 G 门（分量 1e-4、方向相对误差 1.37e-4、J 相对误差 3.8e-7）。论文的核心假设
`J = 0`（[全文](https://par.nsf.gov/biblio/10668229) §3）**改变了模型**，不只是求解器：
朝向不再有惯量，动力学轨迹会与 V3 分离。因此 V4 **不能**用现有 CPU oracle
（`source/hair/rod/rodImplicit.*`、`rodContactImplicit.*`）来满足这条门。

需要人拍板的三选一：

- **A（推荐）**：把 V4 当成**新模型**。`algorithmVersion` 3 → 4
  （`tressfxBehavior.h:30-39`）、`kineticMathVersion` 同步（`rodGpuRecording.h:27`），
  在 `source/hair/rod/` 新增 V4 的 CPU 镜像（double），让"同方程"门对新 oracle 成立；
  V3 的 oracle 与记录原样保留。代价：多一个 CPU 实现（Stage 1 本来就要写）。
- **B**：只把 J = 0 用到朝向，位置仍走现有隐式位置方程 → 模型近似不变，但这已经不是论文
  的算法，收益也未验证。不推荐。
- **C**：放弃 J = 0，只借 VBD 的局部块。收益大幅缩水（朝向仍要非线性求解）。

work certificate（`source/hair/rod/rodWorkCertificate.*`，阈值 `Wabs ≤ 5e-11 Eref`、
平衡/恒等 ≤1e-10，见 [hair-acceptance-metrics.md](hair-acceptance-metrics.md):39）是
**牛顿残差账本**（`freeResidualWork`、`rootConstraintWork`）。V4 没有牛顿残差，
该账本无法原样复用：A 方案下要把它换成"增量势单调账本"（每个 Gauss-Seidel 块更新使 E
下降；Jacobi 下只能要求一个子步内不上升）。**这是本文最不确定的一处，列 Q4。**

### 7.2 用物理定义的指标替换 `maxBlockDefect`

因为 0.0098 那个数字贴在地板上（§2.7），跨模型比较没有意义。V4 的验收改用下面 6 个
指标，全部可从现有侧车/CSV 推出：

| 指标 | 定义 | 现状可用数据 | V4 目标 |
| --- | --- | --- | --- |
| **M1 穿透** | 自由点里 `max(0, −gap)` 的最大值（cm），胶囊与 SDF 分开报 | 现有侧车的 16 个 float 里 `residuals` 那 4 个是诊断专用（`VL_ROD_KKT10_DEBUG` 下已放 SDF 采样统计，`rodKkt10BandSolve.glsl:1293-1304`），可扩成穿透；旧记录的"最小胶囊 gap = -0.0000 cm"是接触关闭时测的，不能再用 | 不劣于同场景 post-change V3 基线（静态当前为 0；动画允许少量，量级未量化，见 §7.3） |
| **M2 拉伸** | 每帧最大正应变（相对 rest 段长），与 CPU `stretch` 同定义 | rest 段长就在 `.rod-state.bin` 的段记录里（`segments[].restStrainLength.w`，`rodCommon.glsl:7-24`；`rodSettledWindow.py:15` 已按这个 8×vec4 布局解析） | ≤ 1%（沿用 G 门） |
| **M3 形状保真** | 与冻结的 V3 快照逐点位移 / 杆长，取最大值 | snapshot `.bin`（40,334×32 点，hair-local cm）+ `.json` 元数据 | 静态 ≤ 1.5e-2 L；动画只记录 |
| **M4 能量单调** | 每子步前后增量势 `E`（惯性 + 拉伸 + 弯曲），报 `ΔE > 0` 的子步比例 | 需要新增一个诊断 float4 | 静态场景 = 0；动画记录 |
| **M5 有限性** | NaN/Inf 点数、失败 guide 数、`gpuStatus` | 现有 | NaN/Inf = 0；失败 guide 数 ≤ 同场景 V3 基线（动画当前为个位数到十余根 / 2000） |
| **M6 整体运动** | 与冻结的 post-change V3 轨迹逐帧逐点比较（hair-local cm），取均值与最大值 | snapshot 序列 | 均值 ≤ 1.5e-2 L，且末 100 帧均值不劣于前 100 帧；最大值只记录 |

重定基流程（**保号不覆盖**）：

1. 旧数字（0.009791177697 / 0.0099687 / 0.0087894）保留在
   `../../rationale/hair-rod-gpu-v3-contact-20261123.md:2254-2265` 与
   [hair-acceptance-metrics.md](hair-acceptance-metrics.md) 里，不重判、不改写。
2. 新记录另起一份（Stage 3 结束写），注明 `algorithmVersion=4`、指标定义、阈值、实测值。
3. 同一 build、同一天，对同一场景跑 V3 与 V4 两遍，双方都用 M1–M6，形成可比行；
   CSV **前 10 列不动**，新指标只追加在第 15 列之后或走侧车。

### 7.3 验收口径变更（2026-11-25）：允许少量穿透与非收敛 guide

真实 SDF 接触现在**默认开启**（它此前被一个单位 bug 静默关闭），两条动画验收场景因此
第一次真正跑在接触打开的状态。后果：

- 静态场景仍然干净（0 失败、0 VUID，§2.7）；
- 两条动画场景带**少量非收敛 guide**（实测个位数到十余根 / 2000，随 run 与子步数波动），
  并伴随轻微穿透。穿透量级本轮**没有量化**：旧记录的"自由点最小胶囊 gap = -0.0000 cm"
  是在接触关闭时测的，不能再用；Stage 0 必须用 M1 补测。
- owner 的裁定是："a little interpenetration is fine as long as the overall dynamics is
  right"。因此 V4 **不再需要**做到零失败或零穿透，但**必须不把整体运动做得更差**。

门槛因此改成：

| 旧口径 | 新口径 |
| --- | --- |
| M1 = 0（≤1e-5 cm） | M1 ≤ 同场景 post-change V3 的 M1（静态仍是 0；动画以 V3 为基线） |
| 失败 guide = 0 | 失败 guide 数 ≤ 同场景 post-change V3（动画允许个位数到十余根） |
| 无 | 新增 M6：整体运动对冻结的 V3 轨迹不劣化（静态形状仍用 M3 的 1.5e-2 L） |

这把"计划必须证明零失败"换成"计划必须证明不劣化"，也意味着 Stage 2/3 的失败 guide 与
穿透门都改成**相对基线**。代价是验收结论必须带 V3 同帧对照，不能再只报 V4 的绝对值。
M6 目前没有历史基线：必须先跑一遍 post-change V3、冻结 300 帧轨迹，才有"不劣化"的分母
（§12 Q10）。

---

## 8. 分阶段迁移

每个阶段的"门"都必须能在**不改物理**的前提下跑完；任何阶段失败都可以只回退该阶段。

### Stage 0 — 只读测量与仪器校验（不改物理）

- **改动**：只加两个默认关闭的诊断计数（`#define` gated）：每点每迭代命中的胶囊数、
  每 guide 每子步的迭代数直方图（后者已有侧车，见 §2.6）。
- **已完成（2026-11-25）**：§9.1 的代价分解（CR 67% / condensation 10% / 其余 23%）
  取代了 Stage 0 原本要测的 `f`。旧版第 2 项"SDF 管线 vs 胶囊管线"（34,668 vs 14,700）
  在两条管线都变成 14,700 B 之后不再有对照意义，作废。
- **仍需测**（4 个数）：
  1. 仪器校验：`passes[hairSimulation].ms` 与同帧 `gpustats` 的差 ≤2%；`VL_ROD_FIXED_SUBSTEPS=1/2/4`
     下 ms 近似线性；hair scope 不含 GPU idle。
  2. 每 guide 每子步迭代数（§2.6 的方法，在接触开启 + 4 子步的新配置下重测）。
  3. 每点每迭代命中胶囊数（1–8 的分布），回答 §5.2 粗筛能省多少。
  4. M1 穿透的基线值（§7.3 要求，静态与动画各一条）。
- **验收门（仪器）**：`passes[hairSimulation].ms` 与同帧 `gpustats` 表的 `hairSimulation`
  相差 ≤ 2%；固定子步控制组与自适应组的 ms/子步比值在 ±10% 内；每行是 120 帧滚动平均、
  每秒最多一行，因此报告必须给 CV 与"每 run 均值"（见 §11.1）。
- **回滚**：删两个 `#define`；无行为变化。

Stage 0 不再是"决定 `f`"的阶段（`f` 已经被分解回答）；它现在的职责是**把新口径下的仪器与
基线钉住**，让 Stage 3 的相对门有分母。

### Stage 1 — CPU 原型（数学与验收口径）

- **改动**：新增 `source/hair/rod/rodStableCosserat.{h,cpp}`（double，Jacobi 与
  Gauss-Seidel 两个变体）+ `tool/hair-tests/rodStableCosseratTests.cpp`；挂进
  `vl_hair_assets` 与 `hair-tests`（`source/hair/CMakeLists.txt:1-18` 的库列表）。
  不改 runtime、不改 shader。
- **可测量**：`tool/hair/rodGroomValidation.cpp` 的 8 根 groom 与 24 fixtures 的
  `settled` / `Dmax` / `Rtail` / `stretch`；与 V3 CPU 解的逐点最大位移；每步 CPU 耗时。
- **验收门**：`PASS = settled && Dmax<=.015 && Rtail<=.005 && stretch<=.01`
  （[hair-acceptance-metrics.md](hair-acceptance-metrics.md):53）；
  新增 `max|x_V4 − x_V3| / L <= .015`（静态）。任一不达标 → 回到 §7.1 的模型决策。
- **回滚**：删两个文件 + 两行 CMake。

### Stage 2 — GPU 单条管线（16-guide，非 SDF）

- **改动**：新增 `shader/glsl/hair/rodV4Kernel.glsl` + `rodV4Stable.comp`；
  `hairSimulationPass.cpp:89-97` 的 `rodPipelines` 扩到 9；新增 solver 枚举值
  （7 处，§1.3）；`SC_rod_v3_capsules_16` 的 solver 覆盖变体（`Generated/Validation`，
  不入库）。
- **可测量**：CSV 新列 M1/M2/M4、snapshot 逐点对 V3、`hairSimulation` ms（只记录）。
- **验收门**：exit 0、0 VUID、failed guides ≤ 同场景 V3、`gpuStatus` 与 V3 同档、
  M1 ≤ V3、M2 ≤ 1%、M3 ≤ 1.5e-2 L。**性能不判**（16 guide 没有统计意义）。
- **回滚**：`solver` 值改回 `cosseratImplicitV3`（默认不动，本阶段只是新增选项）。

### Stage 3 — 规模 + SDF 管线 + 调度耦合

- **改动**：`rodV4StableSdf.comp`（声明同样 3 个 SDF 绑定 8/11/26，与
  `rodV3ImplicitSdf.comp` 对齐）；2000-guide 场景；替换"硬帧"门（§9.6）。
- **可测量**：四条 `SC_tressfx_rod_*` 场景 + `SC_rod_v3_capsules_16/2000` 的
  M1–M6 与 `hairSimulation` ms；子步分布；债务/丢弃秒数。
- **验收门**：五条场景 M1 ≤ 各自 V3 基线（静态仍为 0）、M2 ≤ 1%、M5 的失败 guide ≤ V3、
  静态 M3 ≤ 1.5e-2 L、动画 M6 不劣化；**性能门（相对基线）**：同一 build、同一场景、
  `VL_ROD_FIXED_SUBSTEPS=4`、同一 300 帧窗口，动画 SDF 场景 `hair` ≤ **0.45 × 实测
  post-change V3 基线**（当前 84.53 ms ⇒ ≤ 38 ms），中心目标 0.36×（≈30 ms）；静态 SDF
  场景 ≤ **0.50 × 自己的 post-change V3 基线**（该基线须在 Stage 2 同一 build 上补测，
  本文不沿用旧的 8 ms）。未达 → 不进 Stage 4，先做 §11.2 的接触改造再复测。
- **回滚**：切回 V3；V4 的 pipeline 与 shader 留着但不再被选中。

### Stage 4 — 默认切换 + 文档 + 删除

- **改动**：7 处选择器同步（§1.3）；删除 §4.3 的死代码与探针；更新
  `contracts/testing.md`、`documents/routing.md` 行、以及一份新的 rationale 记录
  （把 Stage 0/3 的数字写进库内，因为 `artifacts/` 是 gitignored）。
- **可测量**：`hair_asset_tests` 全量、四条验收场景、`py -3 tool\validate-docs.py`。
- **验收门**：与 Stage 3 同，加上文档校验通过。
- **回滚**：一个 JSON 值（`solver`）。

---

## 9. 预期性能（2026-11-25 重算）

### 9.1 事实基础（全部可核对）

| 事实 | 值 | 来源 |
| --- | --- | --- |
| 当前 V3 基线：`SC_tressfx_rod_sdf_animated`，固定 4 子步，300 帧，`passes[hairSimulation].ms` | **84.53 ms**，run 间 CV 0.8–1.4% | 2026-11-25 的 `perf-split-none` 探针；与下面两个双跑探针同 build、同日、同参数 |
| 同一场景，块循环约化（CR）做两遍 | 141.56 ms ⇒ **CR = 57.0 ms（67%）** | `VL_ROD_CR_DOUBLE`；探针带运行期假条件引用，防止 DCE |
| 同一场景，`RodKkt10Condense` 做两遍 | 92.76 ms ⇒ **condensation = 8.2 ms（10%）** | `VL_ROD_CONDENSE_DOUBLE`；同上 |
| 其余 | **19.3 ms（23%）**：接触查询、残差、线搜索、状态装填与写回 | 84.53 − 57.0 − 8.2 |
| 默认子步（自适应，实测 4 子步/帧） | 动画对 57.9 ms/帧；16.7 ms 模拟时间/帧 | 2026-11-25 的 gate 记录 |
| 两条 V3 管线的静态 shared | 都是 **14,700 B** ⇒ 6 个驻留 workgroup/SM | §2.5（SPIR-V 实测） |
| 硬件 | RTX 3070 Laptop（对 3090 约 2–2.5×） | 评审 |
| 论文头发 | 1,464,704 顶点、7 ms/帧、不含碰撞、RTX 3090 | 论文 §5.6 |

旧的 47 ms 基线、4.21 µs/guide-iteration、0.8 ms 无碰撞地板、34,668 B / 2 wg 全部**作废**：
47 ms 是接触被单位 bug 关闭时的读数；4.21 µs 由那组读数与一个 8 强制子步侧车反推；0.8 ms
来自"去掉碰撞体"的探针（把接触查询与整条约束路径一起删掉，而验收场景现在需要它）；
34,668 B / 2 wg 在 2026-11-25 的两行 shuffle CR 修复后变成 14,700 B / 6 wg。这些旧值不再
参与推导。

默认子步的"实时性"口径：57.9 ms 帧、16.7 ms 模拟时间/帧。owner 的记录把这说成"约 39%
实时"；按这两个数直接算是 **28.8%**，39% 对应旧的 43 ms 帧时间。两者不是同一口径，
本计划记 28.8%，并把 39% 保留为 owner 原话。

### 9.2 比值模型（只用实测份额，不叠加绝对毫秒）

记 `T_V3 = 84.53 ms`。实测份额：

| 组成 | 绝对 | 份额 |
| --- | --- | --- |
| 6×6 压缩系统的块循环约化（CR） | 57.0 ms | 0.674 |
| 6×6 压缩装配（`RodKkt10Condense`：切向装配、接触几何、残差、压缩代数） | 8.2 ms | 0.097 |
| 其余（接触查询、残差/merit、线搜索、状态装填、写回） | 19.3 ms | 0.228 |

V4 拿掉 CR 与大部分 condensation，换成 4 次位置 + 4 次朝向的局部更新；其余仍然要做，
而且 V4 每子步的查询组数可能更多。设

- `α` = V4 每子步位置+朝向算术成本 ÷ V3 的 (CR + condensation) 成本；
- `β` = V4 每子步接触/装填/写回成本 ÷ V3 的其余成本。

则 `T_V4 / T_V3 ≈ 0.771 α + 0.228 β`。

| α | β | 比值 | 加速 |
| --- | --- | --- | --- |
| 1/10 | 1.0 | 0.305 | 3.3× |
| 1/6 | 0.8 | 0.311 | 3.2× |
| 1/6 | 1.0 | 0.357 | 2.8× |
| 1/4 | 1.0 | 0.421 | 2.4× |
| 1/3 | 1.0 | 0.485 | 2.1× |
| 1/6 | 1.67 | 0.510 | 2.0× |
| 0 | 1.0 | 0.228 | **4.4×（硬上限）** |

所以：**中心估计约 3×（84.53 → 约 28 ms），区间 2–4.3×；4.4× 是 α = 0（V4 算术免费）
的硬上限，5× 在本次分解下不可达。** 只有 V4 同时把接触查询也砍掉（β < 1）才可能超过
4.4×；而 §5.2 的"单次三线性 + 解析梯度"同样能用在 V3 上，不能只记在 V4 账上。

α 与 β 都**未知**，必须由 Stage 2/3 的 GPU 探针测量。本表的用途是把"V4 算术必须比当前
耦合求解便宜 4–10 倍"翻译成 Stage 3 可判定的相对门（§8）。

### 9.3 地板：19.3 ms，不是 0.7–0.8 ms

旧版的独立地板（论文 7 ms 按顶点/硬件折算约 0.69 ms，与 V3 去碰撞的 0.8 ms 相符）现在
**不再是这个场景的地板**：那个 0.8 ms 探针把接触查询和约束路径一起删掉了，而验收场景
现在默认开启真实 SDF 接触。当前实测的非求解地板是 **19.3 ms（23%）**；它包含接触查询，
所以地板还会随接触形态变化。V4 的收益上限因此是 `84.53/19.3 = 4.4×`，而不是"47 → 10"。

### 9.4 与 occupancy 阈值的交互（已改变）

- 现状：两条 V3 管线都是 14,700 B ⇒ `102400/14700 = 6.96` ⇒ **6 个驻留 workgroup/SM**。
  旧版的 34,668 B / 2 wg 已作废；验收场景不再跑在 2 wg。
- V4 目标 ≤2 KB ⇒ 按 shared 可到 50 wg/SM，但**这不再是 2→6 的增量**：shared 阶梯
  （17066/14628/12800/11377/10240）在 14,700 B 时只剩 72 B 就跨到 7 wg。真正的下一个
  限制（寄存器文件、warp 槽、每 SM workgroup 上限）**未知**，必须用 Stage 2 的 pad
  标定法测；如果寄存器先撞墙，≤2 KB 的 shared 目标对 occupancy 没有收益。
- 2000 guides = 2000 workgroup：按 6 wg/SM × 46 SM ≈ 7 波（旧版按 2 wg 算的 22 波作废）。
  波数只是吞吐视角的量级感，不是预测。
- 记录里"7 个驻留比 6 个更差"的非单调陷阱在 14,700 B 附近仍然相关（再降 72 B 就到 7 wg），
  V4 落地前应复测这个点。

### 9.5 计划前提是否成立（how the premise fared）

写这份计划时，中心判断是"成本的大头不在求解器，而在接触查询；CR 只是小头"
（旧 §9.1 的 0.8 ms 地板 + 12% 的 CR 读数；那个 12% 是 1 子步 / 27.5 ms 的配置测的，
与 4 子步 / 84.53 ms 的验收配置不可比）。2026-11-25 的分解**推翻了这个判断**：

- **67% 是 CR、10% 是 6×6 压缩装配**——即 77% 是耦合的 6×6 带状求解。位置/朝向拆分正是
  把 6×6 耦合换成两个 3×3 局部问题，所以计划的核心主张（"钱在耦合求解里，拆开就能省"）
  现在有实测支撑，比写作时更强。
- 同时，**23% 的其余成本**给出硬地板：即使 V4 的算术完全免费，也只能到 4.4×；旧版的 5×
  中心与 2–10× 区间因此作废。
- 接触查询仍在其余 23% 里，所以"接触会限制收益"这句警告方向正确，但量级比旧版假设的小：
  它不是 46/47 ms 的主体，而是 ≤19.3 ms 的一部分。
- 这条分解**没有**回答 V4 自己的算术成本（α）和接触查询在 V4 下的变化（β）；它们是
  Stage 2/3 的测量任务，不是本节的推论。
- Stage 0 的离线方向步原型（`artifacts/tmp/v4orient/`，gitignored）在作者自己的参数尺度上
  复现了闭式朝向步的收敛、rest 保持与单精度 unit-norm；但它同时给出一个警示：**耦合**的
  位置+朝向在 32 顶点杆上 4 轮只释放约 22% 的步内势降，而 11 顶点参考悬臂释放约 97%。
  "4 轮"因此是离散与参数的函数，不是算法常数（§12 Q9）。这不改变 §9.2 的收益模型，
  但改变 Stage 1 的验收方式：CPU 镜像必须在真实头发参数上测每步残差，不能假定 N=4 够用。

### 9.6 与 1/120 s 步长和债务策略的耦合（原 §9.4）

- 步长与子步上限不变（`rodGpuRuntime.h:31-35`），退休上限 33.3 ms、债务上限 0.5 s 不变。
- **子步门已经改过**（2026-11-25）：不再取 max-over-guides，而是数"超过 12 轮的 guide
  比例 > 1%"。动画场景因此从 1 子步/帧变为 **4 子步/帧**（模拟时间 4.2 → 16.7 ms/帧，
  帧时间 44.7 → 57.9 ms）。旧版"约 2 子步"的段落作废；`VL_ROD_GATE_MODE=legacy` 可以
  回到旧判据做 A/B。
- **"硬帧"门对 V4 仍然失效**：V4 恒 4 轮，按牛顿轮数的门永不触发。替代信号候选不变
  （每子步末势残差、投影激活点比例、或只留债务限流），属于 Stage 3 设计任务。
- 子步序列随求解器成本变化，所以跨求解器的 ±5e-8 复现不可能；A/B 一律用
  `VL_ROD_FIXED_SUBSTEPS` 固定步数。默认子步下的 57.9 ms / 16.7 ms 是本计划唯一认可的
  "实时性"口径：若默认子步下的成本分解与固定 4 子步相近，3× 会把 57.9 ms 降到约
  19 ms/帧（约 87% 实时），但这个外推未测量。

## 10. 风险登记册

| ID | 风险 | 证据/机制 | 缓解 | 止损（回退条件） |
| --- | --- | --- | --- | --- |
| R1 | **接触保真度下降**：无乘子、无摩擦、无互补条件 | §5.4；现状无边值摩擦（§2.4）、`RepairOverlap` 只保证贴面 | 每子步末强制投影（M1 以 V3 为基线）；粗筛保持精确；投影放进迭代内 | M1 劣于同场景 V3，或肉眼可见滑移/塌陷 → 保留 V3 |
| R2 | **根锚定与 follow 插值**错位 | follow 依赖绝对朝向（§6.3）；`frameDeltas` 语义（`rodV2Kernel.glsl:1246-1252`） | v1 保留同一发布路径；新增 M3（对 V3 快照的逐点差） | M3 静态 > 1.5e-2 L，或 follow 点错位 → 回退 |
| R3 | **确定性与验收基线**：`maxBlockDefect` 无动态范围；±5e-8 不可跨求解器复现 | §2.7、§9.6；记录自身就有 1e-8…3e-5 的跑间抖动（`…20261123.md:2271`） | §7.2 换 M1–M6；保号不覆盖；同 build 同日 A/B | 新指标无法区分好坏（例如 M2 在 V3 上也超限）→ 先修指标 |
| R4 | **trace / snapshot 侧车格式**：`RodGpuGuideVec4Count = 412` 被两个 Python 脚本硬编码；`rod-diagnostics.bin` 16 float 的字段含义是 V3 专属 | `rodGpuRuntime.h:89`；`tool/hair/validation/rodSettledWindow.py:11`、`rodMaterialFollowOracle.py:46-47` 都写死 `(2000,412,4)`；`hairSimulationPassTrace.cpp:232-234` | 不改 `RodGuide` 布局；新指标走第 15+ 列或新侧车；`algorithmVersion=4` 让 `rodHasReferences` 自动为 false（`hairSimulationPassTrace.cpp:60`） | 任何布局改动 → 必须先改两个脚本并留旧脚本兼容分支 |
| R5 | **1/120 s 与债务策略**：硬帧门失效（§9.6）；V4 变快改变子步序列 ⇒ 轨迹不可比 | `rodGpuRuntime.h:76-88`、`rodGpuRecording.h:349-358` | 新降载信号（Stage 3）；A/B 一律用 `VL_ROD_FIXED_SUBSTEPS` 固定步数 | 债务/丢弃秒数显著恶化（>`droppedSeconds` 基线 2× ）→ 回退 |
| R6 | **shader 编译时间与分歧** | V3 内核 GLSL 约 117 KB（`rodV2Kernel.glsl` 60 KB + `rodKkt10BandSolve.glsl` 57 KB）⇒ 编译慢；V4 预计 15–20 KB（估）。分歧：V3 有数据依赖的 64 轮循环 + 21 次回溯；V4 固定 4 轮但接触分支仍分歧 | V4 把接触集合决策提升到 warp 一致（每 guide 掩码）；编译时间用 `elapsedMs`（`source/shaderCompiler.cpp:707`）与 cache hits/misses 记录 | 编译时间 > 现状 1.5×，或出现新的 device-lost（参考 [hair-gpu-crash-20261123](../../rationale/hair-gpu-crash-20261123.md)）→ 回退 |
| R7 | **模型身份**：J = 0 破坏"GPU 与 CPU 同方程"G 门；work certificate 不可复用 | §7.1；`hair-acceptance-metrics.md:39-40` | 选 §7.1 的 A 方案：`algorithmVersion`/`kineticMathVersion` 递增 + 新 CPU 镜像 + 新账本 | 若产品侧不接受模型变更 → 改走 ADMM（附录 B 的对比）或放弃 |
| R8 | **Jacobi 破坏单调能量保证** | 论文 §3.2–3.3 的保证是 Gauss-Seidel；§5.7 的 GPU 版是 Jacobi | 把"能量单调"变成被测量（M4），先测再判 | M4 在静态场景出现 `ΔE > 0` 的子步比例 > 1% → 改两色更新或最后一轮串行 |
| R9 | **SDF 穿透/隧穿**（投影是事后修正） | 投影只保证子步末贴面，快速运动时子步内可穿过薄代理 | 每子步末投影 + 不改步长；用 M1 逐帧监控 | 动画场景 M1 尖峰 > 0.5 cm → 回退或加连续检测 |

---

## 11. 验证计划与最便宜实验

### 11.1 复用的现有门

| 门 | 入口 | 用途 |
| --- | --- | --- |
| CPU rod 模块 | `ctest --test-dir build -R <suite> --output-on-failure`；套件名与覆盖见 [testing.md](../../contracts/testing.md):114-167（rod 断言落在 `hair_asset_tests`） | V4 CPU 镜像的单元与长程 |
| CPU 物理 G 门 | `tool/hair/rodGroomValidation.cpp`（8 groom × 3,240 步 / 24 fixtures） | settled / Dmax / Rtail / stretch |
| 运行时验收 | `build\bin\main_editor.exe --initial-scene <scene> --frame-limit 60 --worker-thread-count 1 --no-dev-ui --exit-after-frames 400`（命令行形状见 `…20261123.md:2280-2287`） | M1–M6、gpuStatus、失败 guide、VUID |
| 性能 | `--perf-jsonl` 的 `passes[hairSimulation].ms`（不钳制），绝不用 `fps`/`cpuMs`（被钳制） | A/B |
| 诊断 CSV | `artifacts/hair-landing-20261119/gpu-runtime-diagnostics.csv`（`rodGpuRecording.h:191`） | 14 列，前 10 列定序 |
| 快照 | `hair snapshot <path>` + `.json` + `.rod-diagnostics.bin` 等侧车 | M1、M2、M3、M6 |
| 文档 | `py -3 tool\validate-docs.py` | 本文件本身 |

### 11.2 最便宜实验（按性价比排序，全部在任何重写之前）

1. **测量仪器校验**（半天，0 行物理改动）。交叉核对 `passes[hairSimulation].ms` 与
   `gpustats` 表（差 ≤2%）；用 `VL_ROD_FIXED_SUBSTEPS=1/2/4` 确认 ms 近似线性；
   确认 hair scope 不再含 GPU idle（记录里的 profiler 修复需在本 build 复验，
   见 `artifacts/hairsim-ab-20261124/summary.md`）。**这个仓库已经有四类假读数**
   （fps 钳制、cpuMs 钳在 100、每行 120 帧滚动平均、pass 计时把 idle 算进去），
   不先验仪器，后面所有数字都不可信。
2. **把 SDF 管线切到 shuffle CR**（2026-11-25 已落地，改 2 行）。
   `rodV3ImplicitSdf.comp` 现在有 `#extension GL_KHR_shader_subgroup_shuffle` 与
   `#define VL_ROD_CR_SHUFFLE`，足迹 34,668 → 14,700 B（6 wg）。收益已计入 84.53 ms 基线，
   不再作为"可白拿"项；保留此条只为记录它是新基线的一部分，以及旧注释"与
   `rodV3ImplicitCrCompensated.comp` 的唯一差异是 `VL_ROD_SDF`"确实是错的。
3. **SDF 单次采样 + 解析梯度**（1–2 天，V3 可独立上线）。把 7 次三线性采样
   （`rodKkt10BandSolve.glsl:196-214`）换成 1 次 + 解析梯度，量 SDF 场景的 `hair` 变化。
   它同时是 V4 接触方案的一半，且可独立发布。
4. **胶囊命中率统计**（半天，2 个 atomic 计数）。回答"每点每迭代实际有几个胶囊在半径内"。
   如果答案是 1–2 个而不是 8 个，粗筛能省 4–8 倍胶囊查询；§9.2 的其余 23% 里接触查询的份额也随之下降。
5. **接触成本分离探针**（2026-11-25 已由 §9.1 的双跑探针完成）。CR、condensation 与其余
   已经分开；接触查询现在被证明落在其余 23% 内，而不是主体。若还要把"查询"与"残差/线搜索"
   再分开，需要新的常量场探针，但 §9.2 的收益上限已经不依赖它。
6. **每 guide 迭代直方图**（已可做，脚本已在 `artifacts/tmp/iter_hist.py`）。在接触开启 + 4 子步的新配置下重跑，确认新的稳态
   轮数（旧值 0.3–1.2 是接触关闭时的读数）；若某场景稳态就有 8–16 轮，V4 的"固定 4 轮"
   反而更省。

2 已经落地并计入基线；3 仍可为 V3 单独落地。**建议先把 3 做成"不重写的 V3 最好版本"，
再判断 V4 还值不值得**——因为 §9.2 的其余 23% 里，SDF 查询是唯一还能单独压缩的大块。

### 11.3 验收数字怎么处理（诚实结论）

简报要求"任何替换都必须守住 0.009791177697 ± 5e-8"这类基线。按 §2.7 与 §9.6，这组
数字是**贴在地板上的收敛残差**，且子步序列由墙钟债务决定，因此：

- 静态场景（`rod_static`、`sdf_static`）：同一模型的不同求解器应落到同一形状，
  `maxBlockDefect` 有望停在同量级；但 V4 的残差定义不同，**不能要求同值**。
- 动画场景：旧带 0.0098–0.0100 已不成立（4 子步下实测 0.287 / 0.298，最终 gate 为
  0.009989947）；V4 的模型不同（J = 0）、子步序列也不同，**同值不可能成立**，只能同带
  或同形状，且新带要以 post-change V3 为准。
- `SC_rod_v3_capsules_16` 的 ±5e-8：这是最严的一条，它依赖完全相同的轨迹；
  V4 一旦改变每帧成本就必然破坏它。

因此本计划把"守住基线"实现为三件可执行的事：(a) M1/M5 **以同场景 V3 为基线**（静态仍是
穿透 0、失败 0；动画允许少量，§7.3），M2 拉伸 ≤1% 不变；(b) 静态形状 M3 对着**冻结的快照**
比，阈值 1.5e-2 L；
(c) 旧数字保号，新数字另起记录并标注算法版本。**如果评审坚持要求逐位复现，
唯一满足方式是保留 V3 作为默认——那就不该做这次重写。**

---

## 12. 未解问题（我无法从来源确认）

- **Q1（原问题，2026-11-25 已解决）**：论文 Eq. 15/16 里 `v` 与 `b` 的**精确代数形式**（含 `k_ss`/`k_bt` 的标度与符号）
  在 [NSF TEI 全文](https://par.nsf.gov/biblio/10668229) 中被剥掉了（显示公式未进 XML），
  我只有"v 是拉伸贡献、b 是弯曲贡献之和"这个结构性描述，以及零应变自洽性验算（§3.4）。
  落地前必须从 [PDF](https://graphics.cs.utah.edu/research/projects/stable-cosserat-rods/Stable_Cosserat_Rods-SIGGRAPH25.pdf)
  或 [YarnBall 源码](https://github.com/jerry060599/YarnBall) 取出这两式。（2026-11-25 已从作者 CPU 实现 `lambda.cpp:15/17-35` 取出并核对，见 §3.4；Stage 1 不再被这一项阻塞。）
- **Q2（已解决，2026-11-25）**：`H_edge` 的常数由作者 VBD 实现直接给出
  （`vbd.cpp:36-42`：`k = |kss|/l`、`d = k/l = kss/l²`），即 `H_edge = (κ_i/l_i²)·I`、
  `g_edge = ±(κ_i/l_i)·C_ss`；§3.3 的"尚未完成"可以关闭。
- **Q3**：论文的 GPU 头发用"背景 Eulerian 网格"处理碰撞，我们用的是解析胶囊 + SDF；
  论文的性能数字**不覆盖**我们的接触形态，因此 §9.2 的比值法无法用论文数据校准 `α`/`β`。
- **Q4**：增量势单调账本能否替代 `rodWorkCertificate`（`Wabs ≤ 5e-11 Eref`）我无法判断，
  需要先看 V4 CPU 镜像的实际 `ΔE` 分布。
- **Q5**：V4 恒 4 轮后"硬帧"门（`rodGpuRuntime.h:76-83`）用什么信号替代（§9.6）。
- **Q6**：论文的头发是 Afro 风格（发散、无自碰撞、靠背景网格）；我们的 groom 是
  贴头皮的密发、4 胶囊 + 身体 SDF、38,334 根 follow。两者的接触拓扑差异很大，
  "4 轮收敛"能否在我们的 SDF 缝隙里成立没有证据。
- **Q7**：`SC_rod_v3_capsules_2000` 这类生成场景与 `SC_tressfx_rod_*` 入库场景的
  guide 数、蒙皮源不同，我把它们混在同一张性能表里比较是有前提的（同 2000 guides、
  固定子步）；这一步需要在 Stage 0 里核实。
- **Q8**：`solverProfile` 是否应该新增值，还是复用现有唯一值 + 新增 `solver` 值，
  属于 API 设计选择；本文倾向后者（改动面更小），但没有与编辑器侧确认过。
- **Q9**：V4 的固定 4 轮在 32 顶点、`h/l≈1` 的离散上是否够用？离线原型
  （`artifacts/tmp/v4orient/`）在作者自己的参数尺度上给出：朝向更新单独 4 轮后残余
  转角 ≤6e-4 rad（11 顶点）/ 2.3e-5 rad（32 顶点），但**耦合**的位置+朝向在 32 顶点杆上 4 轮只释放约 22% 的步内
  势降（11 顶点参考悬臂约 97%）。这把"4 轮"从算法性质变成了**离散/参数的函数**；
  Stage 1 的 CPU 镜像必须在真实头发参数上重测每步残差，不能把 4 轮当常量。
- **Q10**：M6（整体运动）没有历史基线，需要先用 post-change V3 跑一遍 300 帧并冻结轨迹，
  才能给"不劣化"一个分母；本轮未做。

---

## 附录 A：内部证据索引（我实际读过的文件与行）

| 主题 | 位置 |
| --- | --- |
| 每 guide 一个 warp | `shader/glsl/hair/rodV2Kernel.glsl:7`、`:721-727`、`source/hair/rodGpuRecording.h:556` |
| 10 宽装配是死代码 | `shader/glsl/hair/rodV3Kkt10.glsl:36-178`（无调用点） |
| 活跃压缩路径 | `shader/glsl/hair/rodKkt10BandSolve.glsl:918-1090`、`:1154-1215` |
| 指令数量级 12k vs 83k | `rodKkt10BandSolve.glsl:1128-1131` |
| 轴向行被编译掉 | `rodKkt10BandSolve.glsl:488-521`、`:1213`；`rodAxialLimit.h:8` |
| 拉伸是软弹性 | `rodV2Kernel.glsl:188-247`、`:219-220`；`rodKernel.glsl:323-324` |
| 无摩擦 | 全仓 grep `friction` 零命中 |
| 静态 shared | 我用 `artifacts/tmp/spirv_shared.py` 量的 SPIR-V；阈值表见 `../../rationale/hair-rod-gpu-v3-contact-20261123.md:2208-2213` |
| SDF 管线已启用 shuffle（2026-11-25） | `rodV3ImplicitSdf.comp:3/14`（有 `VL_ROD_CR_SHUFFLE`）；修复前该文件只有 `VL_ROD_SDF` 这一处差异 |
| 收敛地板 1e-2 | `rodKkt10BandSolve.glsl:49`、`rodV2Kernel.glsl:1019-1020` |
| `maxBlockDefect` 来源 | `rodV2Kernel.glsl:1288`、`rodGpuRecording.h:213` |
| 迭代直方图字段 | `rodCommon.glsl:7-24`、`hairSimulationPassTrace.cpp:232-234` |
| 诊断 CSV 14 列 | `rodGpuRecording.h:259-272`（写入 `:191`） |
| 快照/侧车格式 | `hairTraceFormat.h:22-45`、`hairSimulationPassTrace.cpp:55-284` |
| 侧车被脚本硬编码 | `tool/hair/validation/rodSettledWindow.py:11`、`rodMaterialFollowOracle.py:46-47` |
| reset 与 pose 历史 | `tressfxSimulationHistory.h:7-109`、`rodGpuRuntime.h:283-305`、`hairSimulationPass.cpp:147-152` |
| 调度常量 | `rodGpuRuntime.h:25-104` |
| 硬帧门 | `rodGpuRuntime.h:76-83`、`rodGpuRecording.h:349-358` |
| solver/profile 选择 | `meshAssetTypes.h:53-63`、`meshAssetValidator.cpp:182-206`、`:1321`、`tressfxGroupSimulation.cpp:95`、`scriptComponentControls.h:202-207` |
| follow 帧构造 | `shader/glsl/hair/rodFollowKernel.glsl:82-107`、`tressfxFollowBinding.h:31-57` |
| 根点钉住 | `rodKernel.glsl:253-330`（`:292-296`）；`rodV2Kernel.glsl:852-876`、`:907-913` |
| 接触几何 | `rodKkt10BandSolve.glsl:123-345`；`tressfxCollision.h:15`、`:98-112` |
| 无碰撞地板 0.8 ms（已作废，§9.1） | `rodKkt10BandSolve.glsl:250-253` |
| V4 代价分解（2026-11-25） | `perf-split-none` / `VL_ROD_CR_DOUBLE` / `VL_ROD_CONDENSE_DOUBLE`，固定 4 子步、300 帧、CV 0.8–1.4% |
| 朝向步公式核对（2026-11-25） | 作者实现 `lambda.cpp:15/17-35/41`、`init.cpp:26/65`、`xpbd.cpp:44-117`；离线原型 `artifacts/tmp/v4orient/proto.py` |
| 子步门改动 | `rodGpuRuntime.h:76-106`（bad-guide 比例；`VL_ROD_GATE_MODE=legacy` 回旧判据） |
| 编译耗时日志 | `source/shaderCompiler.cpp:707` |

## 附录 B：外部来源

- Hsu, Wang, Wu, Yuksel, "Stable Cosserat Rods", SIGGRAPH 2025 —
  [DOI](https://doi.org/10.1145/3721238.3730618)、
  [作者页（PDF/视频/代码）](https://graphics.cs.utah.edu/research/projects/stable-cosserat-rods/)、
  [NSF 全文 TEI](https://par.nsf.gov/biblio/10668229)（我实际读的是这一份；显示公式被剥离）、
  [GPU 实现 YarnBall](https://github.com/jerry060599/YarnBall)（未阅读其源码，仅引用为可用实现）。
  我引用的具体结论：J = 0 与位置/朝向分离（§3）、闭式 Gauss-Seidel 与 `λ ≈ |v| + |b|`
  （Eq. 18/21/22）、精确 λ 的定点迭代（Algorithm 1，Eq. 25）、4 次迭代收敛
  （§5.1，XPBD 需上千次）、头发 1,464,704 顶点 7 ms/帧不含碰撞（§5.6）、
  "32 顶点/发丝适合放进 shared memory"与"位置/朝向分成两个 pass 的 Jacobi"（§5.6/§5.7）、
  IPC barrier + 弹簧摩擦（§4，我们**不采用**）。
- Chen, Liu, Yang, Yuksel, "Vertex Block Descent", SIGGRAPH 2024 —
  [arXiv](https://arxiv.org/abs/2403.06321)、[PDF](https://www.cemyuksel.com/research/papers/vbd-siggraph2024.pdf)。
  引用点：顶点级 Gauss-Seidel 块坐标下降、局部更新、无全局线性系统、可限制迭代数而保持稳定。
- Daviet, "Interactive Hair Simulation on the GPU using ADMM", SIGGRAPH 2023 —
  [ACM 全文](https://dl.acm.org/doi/fullHtml/10.1145/3588432.3591551)、
  [NVIDIA PDF](https://research.nvidia.com/labs/prl/daviet23/interactivehair2023.pdf)。
  引用点（来自 ACM 全文的检索片段，**未逐段通读**）：每时间步 10 次 ADMM 迭代、
  每次 2 次可行投影；"忽略碰撞时，一次 ADMM 迭代约比 cuSPARSE sbric02 的牛顿迭代便宜 30×"。

### 为什么选 Stable Cosserat 而不是 ADMM

| 维度 | Stable Cosserat（本文） | ADMM（Daviet 2023） |
| --- | --- | --- |
| 每步求解结构 | 闭式局部更新，无线性系统 | 局部/全局交替，全局步要解带状系统 |
| 迭代数 | 4/步（论文值；离散/参数相关，见 §9.5 与 Q9） | 10/步（+2 次投影） |
| 与现有模型的关系 | **改模型**（J = 0），破坏 CPU/GPU 同方程 G 门 | 不改模型（仍是 DER 隐式），能保住 G 门 |
| 实现面 | 一个 kernel + 一个 CPU 镜像 | 全局求解器、接触着色、摩擦互补求解器 |
| 已知风险 | Jacobi 破单调性、接触用法与论文不同 | 收敛性理解不如牛顿、权重靠启发式 |

选择理由：目标场景是**只有胶囊 + SDF 接触、无自碰撞**的头发；Stable Cosserat 的
实现面小一个数量级，而 ADMM 的"保模型身份"优势我们用 §7.1 的 A 方案（递增
`algorithmVersion` + 新 CPU 镜像）也能补上。**但如果 §7.1 的模型变更最终不可接受，
ADMM 是本计划的首选替代**——它能在不改方程的前提下拿到"每次迭代便宜 30×"的同类收益。
