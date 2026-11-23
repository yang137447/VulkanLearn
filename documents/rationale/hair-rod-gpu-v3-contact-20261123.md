# GPU CosseratImplicitV3 接触活跃集 M1 记录（2026-11-23）

本文记录 M1/A 阶段 GPU 侧 `cosseratImplicitV3` 的 10 分量活跃集 KKT 落地与**有限**实测证据。
它是 rationale，不是 runtime 契约，也不是行为、回正或性能放行。
V1（`cosseratXpbdV1`）仍是运行时求解器；本文不把 V1 的任何数值结论继承给 V3，
也不把“GPU 跑通”写成“物理质量验收通过”。

## 本轮范围与结论

目标（A）：把最终模型 `cosseratImplicitV3` 在 GPU 上带胶囊/SDF 接触跑通，并把 runtime 从 V1
切过去。本轮实际完成与未完成：

| 项 | 状态 |
| --- | --- |
| GPU 10 分量活跃集 KKT（装配 / 分块带状求解 / 乘子延拓 / 活跃集重选） | 已实现（`rodKkt10BandSolve.glsl`） |
| 胶囊接触（≤8 胶囊，每点 ≤3 活跃接触，解析 Hessian） | 跑通并有实测（16 guide / 400 帧，0 失败） |
| 深重叠位置投影修复（对应 CPU `RepairInitialContactOverlap`） | 已实现，有实测 |
| SDF 代理场接触 | 几何与活跃集已实现，**不收敛**，`rodGpuRecording.h` 保留显式 throw |
| 第 10 分量（1% 上拉伸乘子） | 已实现，本阶段**关闭**（见下）；有失败证据 |
| 200/2000 guide 规模 | 200 规模下 3/200 guide 在首个瞬态耗尽牛顿预算 |
| runtime 从 V1 切到 V3 | **未完成**：runtime 场景 `SC_tressfx_rod_static.json` 声明了 `hairCollision.mesh`，会被 SDF 边界拒绝 |

下文所有数字都来自本轮实际运行，命令见“复现入口”。没有重跑 CPU groom 长程，
也没有做 CPU/GPU 逐步数值对照（见“未继承的证据”）。

## 源码现状（本轮新增/改动）

| 文件 | 内容 |
| --- | --- |
| `shader/glsl/hair/rodKkt10BandSolve.glsl` | 10 分量活跃集、几何查询、重叠修复、10 分量测量、等比例全主元 LU、块三对角求解（1/2 点 supernode）。原文件只有 `RodKktLoadBand`/`RodKktLoadResidual` 的**声明**，本轮补齐实现并重写求解循环。 |
| `shader/glsl/hair/rodV3Kkt10.glsl` | 未改。`RodKkt10AssembleRow` 的逐项符号与 CPU `rodAxialKkt.cpp` / `rodContactImplicit.cpp` 对齐，本轮仅作为被调用方复用它。 |
| `shader/glsl/hair/rodV2Kernel.glsl` | 新增 `VL_ROD_V3_KKT10_SOLVE` 路径：每 substep 起点修复+清活跃集、每轮先重选活跃集再测量、10 分量求解、乘子 `λ <- λ + α F dλ` 延拓、可行性门控。牛顿预算改为 `RodV2IterationBudget`（无约束 12 / 带约束 64）。删除 `params.counts.w != 0 -> failure = 1` 的旧“接触未支持”守卫。 |
| `shader/glsl/hair/rodV3ImplicitCrCompensated.comp` | `VL_ROD_V3_CR_SOLVE` → `VL_ROD_V3_KKT10_SOLVE`。 |
| `shader/glsl/hair/rodV3ImplicitSdf.comp` | 新增：同一 kernel + `VL_ROD_SDF`，单独一条 pipeline（SDF 绑定 8/11/26 只有声明了 `hairCollision.mesh` 的对象才有 buffer）。 |
| `source/hair/rodGpuRecording.h` | 去掉 V3+胶囊的 throw；保留 V3+SDF 的 throw；descriptor 池扩到 8 set / 40 UBO / 160 SSBO；新增第 4 类 set（V3 SDF 求解集）。 |
| `source/hair/rodGpuRuntime.h`、`hairSimulationPass.{h,cpp}` | `solveSdfSets`、`rodPipelines[6]`。 |

调试通道：`VL_ROD_KKT10_DEBUG` 会把“活跃 contact 槽数 / contact 分量残差 / 位置分量残差 /
max |λ|/F”写进 V3 未使用的 `rodState.guides[].residuals`，并随 `hair snapshot` 的
`rod-diagnostics.bin` 侧车读回；回溯次数与修复统计写进 `energies`。本轮证据就是这样取的，
默认编译不带该宏。

## 10 分量系统与 CPU 对应关系

每个 point 一组未知量：`0..2` 位置增量（缩放）、`3..5` 材料帧 body 右扰动、`6..8` 三个接触乘子、
`9` incoming 上拉伸乘子。装配复用 kernel 已有的 `RodV2Tangent` / `RodV2Residual`（已含
`lengthScale/forceScale` 缩放），接触与轴限项按同一缩放补进 10x10 块：

- 接触：`residual_pos -= λ g/F`、`residual[6+slot] = gap/L`、`J[axis][6+slot] = -g`、
  `J[6+slot][axis] = +g`、`J_pospos -= λ H (L/F)`。与 `rodContactImplicit.cpp::AssembleContact` 逐项一致。
- 轴限：`gap = 1.01 ℓ - |x_r - x_l|`、`direction = t`、`N = (I - t tᵀ)/|Δx|`、
  端点反力 `(+λt, -λt)`、`J_pospos += λ N (L/F)`、跨点块 `-λ N (L/F)`。
  与 `rodAxialLimit.cpp::EvaluateAxialLimit` + `rodAxialKkt.cpp::AssembleAxialContactSystem` 逐项一致。

求解是块三对角：`diagonal -= lower·factorUpper[prev]`、`residual -= lower·factorRight[prev]`，
局部 10x10 秩亏时把下一点并成 20x20 supernode（对应 CPU `SolveAxialContactSystem`），
后向代入 `d[p] = factorRight[p] - factorUpper[p]·d[next]`。

### 明确的 GPU 侧差异

1. **fp32 容差**。CPU 的活跃集阈值与穿透判据是 `1e-9 * totalLength`（double），互补条件是
   `|λ·gap|/(F·L) <= 1e-9`。GPU 收敛判据本身只有 `implicitControls.originTolerance.w`（1e-4），
   沿用 1e-9 会让“穿透”永远为真、互补条件永远不满足——**任何活跃接触都不可能宣告收敛**。
   本轮统一用 `originTolerance.w * lengthScale` 作为活跃集阈值与穿透判据，互补条件改由
   merit 的 gap 分量承担，只单独保留乘子符号条件（`λ < -1e-9 F` 剔除）。
2. **并行结构**。workgroup 只有一个 warp（32 lane = 32 point）。装配由 `lane == point` 单次完成
   写进双槽共享缓冲并广播；顺序块 LU 由所有 lane 冗余执行，所以矩阵/右端/主元工作区放 shared
   （放 private 会按 lane 复制 32 份，79 KB/warp，溢出 L1）。
3. **SDF Hessian 取 0**（Gauss-Newton）。CPU `MakeRawSdfContact` 是中心差分 Hessian，两者不等价。
4. **CPU 的 original linear residual 复核默认关闭**（`VL_ROD_KKT10_RESIDUAL_CHECK`），
   由牛顿 merit 单调下降 + 每步 r10 检查替代。
5. **依赖法向判定加强**：CPU 只用“Gram-Schmidt 后残差范数过小”判平行，反平行（`dot -> -1`）
   时残差被放大到 2 倍而漏判，两块行向量互相矛盾会让 10x10 局部奇异。GPU 先按 `|dot| > 1-1e-6`
   判平行/反平行再做 Gram-Schmidt。

## 本轮修掉的真实缺陷（都有实测支撑）

### 1. 约束几何在“位移空间”而不是物理空间求值

V3 的未知量 `v2Positions` 是**相对固定作者世界 rest 的位移**；物理位置是
`v2Positions + v3PositionLow + displacementReferences.positions + originTolerance.xyz`，
与 kernel 结尾发布到 `currentPositions` 的 `publishedPosition` 一致。最初的实现只加了补偿低位，
于是胶囊 gap 变成“到原点距离 - 半径”的巨大正数、轴限 gap 恒为正：
**接触与轴限的活跃集永远为空**。表现是 16 guide 全部“收敛”（残差 5.8e-5）却仍有 2.04 cm 穿透，
且 V1 在同样位置精确贴面（gap = -0.0000）。

修正后同一场景出现活跃接触、`maxComp` 从“假收敛”变成真实的 0.049 直至失败——暴露了后面几项。

### 2. merit 线搜索无法从“整段穿进粗胶囊”的初始重叠恢复

修正 1 之后 5/16 guide 在首个 post-reset 帧耗尽 24 轮（后来又试 64 轮）预算，
`contact 残差 0.049`、`回溯 744 次`、`λ` 几乎不增长：`α` 只能退到极小值。
原因是 merit 是整根 guide 的残差平方和，一次到位的大位移会让相邻段的剪切/弯曲能暴涨，
Armijo 只接受极小步，迭代停在“穿透但没有更差”的不动点。

CPU 因此在步进前有独立的 `RepairInitialContactOverlap` 投影阶段。本轮在 GPU 加了对应阶段
`RodKkt10RepairOverlap`：每个 substep 起点、只在几何重叠超过总长 1% 时才做，
逐点沿约束梯度投影到表面，上限 16 轮。实测触发深度 3.4–3.6 mm，触发 guides 由 2.04 cm
穿透降为 `-0.0000 cm`（与 V1 一致）。这是**声明的稳定化步骤**：直接改位置、不携带动量，
不属于隐式解的一部分。

### 3. 牛顿预算 12 轮不够

带约束路径的预算从 12 提到 64（无约束路径保持 12）。16-guide 场景里 6 个 guide 稳态需要
8 次接受步，首个瞬态最多用满预算；24 轮时有 1/16 guide 停在 `merit 7.7e-4`、`contact 残差 4.1e-3`
（1.6 mm 穿透）不能收敛，64 轮时 16/16 收敛到 `4.3e-5`。

### 4. 遗留的“接触未支持”守卫

`rodV2Kernel.glsl` 里原有 `if (params.counts.w != 0u) failure = 1u;`（`counts.w` 是胶囊数），
配合 `rodGpuRecording.h` 的 throw 一起把 V3 挡在接触之外。throw 去掉后这一行仍然让所有
16 个 guide 在上第一帧就 `failure=1`。本轮删除。

## 实际证据

测试资产在资源 worktree 的 `Generated/Validation/`（gitignored，不入库）：
`TF_female_rod_v3_16.json` / `TF_female_rod_v3_200.json`（`solver=cosseratImplicitV3`）、
`TF_female_rod_v1_16.json`（同源 16 guide，`cosseratXpbdV1` 基线）、
`SC_rod_v3_capsules_16/200`（仅胶囊）、`SC_rod_v3_sdf_16/200`（胶囊 + `female_body_proxy.obj`）、
`SC_rod_v1_capsules_16`（V1 基线）。生成器是本轮 ad hoc 脚本，不属仓库。
`Maps/` 正式资产未改。

### 胶囊接触：16 guide / 400 帧（`run16-final-cap16.log`）

`SC_rod_v3_capsules_16`，`--frame-limit 60 --worker-thread-count 1 --no-dev-ui
--exit-after-frames 400`，validation layer 打开，0 条消息。

| 指标 | V3（frame 371） | V1 基线（frame 241） |
| --- | --- | --- |
| `gpuStatus` / 失败 guide | 0 / 0 | 0 / 0 |
| `currentBufferComponentResidualMax` | 4.295e-05 | 1.398e+00（V1 无此归一化） |
| 自由点最小胶囊 gap | **-0.0000 cm** | **-0.0000 cm** |
| 段拉伸最大 / 最小 | +94.029% / -0.0021% | +94.056% / -0.558% |
| 包围盒 x/y/z（cm） | [-11.69,9.46] / [131.86,176.20] / [-16.09,10.36] | [-11.70,9.48] / [131.84,176.18] / [-16.10,10.38] |
| 总位置 | 有限 | 有限 |

- 自由点最小 gap 与 V1 完全同为 `-0.0000`：两个模型的接触都精确落到胶囊表面。
  被排除的 root 前缀点（TFX 第 0/1 点逆质量为 0，位于头皮内侧）在两者中都是 -1.5437 cm，
  属作者资产性质，不是求解器缺陷。
- 唯一的 +94% 段拉伸出现在 guide `src12666` 的 segment 1（rest 0.3433 cm → 0.6661 cm，
  相邻 pinned root），V1 在同一段同样是 +94.06%。**两个模型一致，本项不是 V3 引入的**，
  但本阶段关闭轴限，所以 V3 侧确实没有 1% 上界保护。
- 16 guide 的 `V3 - V1` 逐点位置差：mean 0.062 cm、p95 0.185 cm、max 0.402 cm。
  这是两个模型（XPBD 位置投影 vs 隐式活跃集 KKT）的差异，不是同一个模型的两份实现。

### 200 guide 规模：3/200 失败

`SC_rod_v3_capsules_200`，200 帧：`gpuStatus=3`，`gpu-failed-guides.csv` 591 行，
3 个 guide 在首个瞬态耗尽 64 轮预算（merit 0.062、contact 残差 0.207）。
现有策略下 `gpuStatus != 0` 会令整个对象 `steps = 0` 冻结，因此这 3 个 guide
会让 2000-guide 级别场景停摆。这是本轮**未解决**的规模问题。

### SDF 接触：不收敛，保留 throw

`SC_rod_v3_sdf_16`（胶囊 + `female_body_proxy.obj`，resolution 48³、margin 0.05 cm），160 帧：
16/16 guide 失败。两类失败：
- 多数 guide 在 `iteration 0` 就 `bandStatus != 0`（10x10 与 20x20 supernode 都丢秩）→ `failure=4`；
- 少数 guide 耗尽 64 轮（`merit 1.4e4`、`maxComp 101`、`λ/F` 到 1003）→ `failure=3`。

已尝试并**未解决**：把反平行法向加入依赖判定（改动后同一场景数字逐位不变，说明首轮丢秩另有原因）、
把预算提到 64、加 SDF 重叠修复（修复确实触发，深度 3.4–3.6 mm，但仍丢秩）。
因此 `rodGpuRecording.h` 对 `progressive && HasTressfxSdfCollision()` 保留显式 throw：

```text
cosseratImplicitV3 SDF contact does not converge yet; author capsules only
```

这条边界是有意的：宁可让场景明确失败，也不让场景静默拿到一个不收敛的 solver。
runtime 场景 `SC_tressfx_rod_static.json` 声明了 `hairCollision.mesh`，所以
**本轮没有把 runtime 从 V1 切到 V3**；`TF_female_rod_validation.json` 保持
`solver=cosseratXpbdV1` 未改。

### 第 10 分量（1% 上拉伸乘子）：实现但关闭

`VL_ROD_KKT10_AXIAL_LIMIT` 打开时（24 轮与 64 轮都试过），16 guide 场景里
`src12666` 与 `src16242` 在 `iteration 4–5` 丢秩，`|λ|/F` 到 1.0e3–1.7e3，
其余 guide 正常。这两个正是重叠修复触发的 guide：修复把点推出胶囊时局部段被拉长到
远超 1%（例如 0.34 cm → 0.67 cm），轴限随即要求压回，接触又要求推出，两个乘子对打。
修复与 1% 不可伸长约束的交互是 B 阶段（clump/体积）要一起重做的部分。
当前 comp 不定义 `VL_ROD_KKT10_AXIAL_LIMIT`，row 9 退化为单位行，等价于 CPU 的 9x9
接触系统（`SelectContactActiveSet`/`AssembleContactSystem`/`SolveContactSystem`）。

## 复现入口

在代码 worktree 根目录、resource worktree 已生成派生资产的前提下：

```powershell
$env:VK_LAYER_PATH="C:\VulkanSDK\1.4.363.0\Bin"
$env:VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation"
build\bin\main_editor.exe --initial-scene Generated/Validation/SC_rod_v3_capsules_16.json `
  --frame-limit 60 --worker-thread-count 1 --no-dev-ui --exit-after-frames 400 `
  --console-command "hair snapshot D:\...\artifacts\hair-v3-contact\snap-v3-cap16-final.bin" `
  --console-command-delay 370
```

输出：
- `artifacts/hair-landing-20261119/gpu-runtime-diagnostics.csv`（每帧 gpuStatus、maxComp、merit、accepted/received/debt）
- `artifacts/hair-landing-20261119/gpu-failed-guides.csv`（逐失败 guide）
- `hair snapshot` 的 `.bin`（40,334×32 当前点，hair-local 厘米）+ `.json` 元数据 +
  `.rod-state.bin` / `.rod-diagnostics.bin` 侧车（诊断侧车是本轮取数来源）

本轮分析脚本（rest 段长、拉伸、胶囊 gap、V1/V3 逐点差）是 ad hoc Python，放在
gitignored `artifacts/tmp/`，不入库。

## 未继承的证据与明确边界

- 没有 CPU/GPU 逐步数值对照。CPU oracle `StepImplicitMomentumContact` 与
  `AssembleContactSystem`/`SolveContactSystem` 存在，但本轮没有搭出“同一 guide、同一胶囊、
  同一 root 支撑”的 CPU runner；`tool/hair/validation/exportRodKkt10.cpp` 与
  `rodKkt10AssemblyProbe.comp` 等探针也没有 runner。GPU 侧只按**约束不变量**验证
  （贴面 gap、有限性、与 V1 基线一致、逐帧残差），不是逐步等价证明。
- 1% 上拉伸约束本阶段关闭，所以本轮 V3 **不是** CPU 最终模型 `axial004` 的同身份实现；
  不能引用 CPU axial 的长程结论（`documents/rationale/hair-rod-final-cpu-20261120.md`）。
- 本轮未做风、回正、settled 窗口或性能验收；`--frame-limit 60` 下 16-guide 运行
  仍有 4.9 s/18.2 s 的时间债，说明即使 16 guide 也没跑满实时。
  40,334 根规模、风/回正与性能属 C 阶段。
- 200/2000 guide 规模、SDF 接触、轴限交互、以及 `gpuStatus` 粘滞导致的对象冻结策略，
  都是本轮未解决项；`SC_rod_v3_sdf_16/200`、`SC_rod_v3_capsules_200` 只在本地
  `Generated/Validation` 存在，不是入库契约。
- 渲染效果（`M_strandPbr` 只是 DefaultLit 预览）不在本轮范围，未改动。
