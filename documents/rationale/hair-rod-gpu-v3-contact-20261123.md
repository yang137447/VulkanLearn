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
| 深重叠位置投影修复（对应 CPU `RepairInitialContactOverlap`） | 已实现，有实测（第二轮改为「修到贴面」） |
| SDF 代理场接触（Gauss-Newton） | 跑通并有实测（16 guide / 400 帧，0 失败）；本资产上实际不产生修正，见下 |
| 第 10 分量（1% 上拉伸乘子） | 已实现，本阶段**关闭**（见下）；有失败证据 |
| 200/2000 guide 规模 | 200 规模下 3/200 guide 首个瞬态耗尽预算（已可隔离，不再冻结对象）；2000 规模**设备丢失** |
| runtime 从 V1 切到 V3 | **未完成**：V3 在 2000-guide 掉帧到 TDR，切过去是硬崩溃 |

第二轮（同日）修掉了 SDF 的根因，SDF 从 16/16 失败变成 16/16 收敛；同时把重叠修复的
「是否修」与「修到哪」拆成两个阈值。规模问题（3/200 guide）仍未解决。

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

### 5. SDF 梯度在截断带边界爆到 1e10（第二轮，SDF 失败的根因）

第一轮把 SDF 的 16/16 失败记成“未知”。第二轮定位到具体机制：

逐帧栅格只写 `|d| <= truncation`（`truncation = margin + 2*cellSize`，本资产约 2.23 cm）的格子，
带外格子保持 clear 哨兵。`tressfxSdfCommon.glsl::SdfGradient` 的中心差分
`SdfTrilinear(p±h)` 只要**任一**采样角点碰到哨兵就整体返回 `+1e10`，于是带边界附近一侧的
差值是 1e10，梯度模长直接到 1e10 量级。V1 只把它 `normalize()` 当法向用，看不出来；
V3 的 KKT 里原始梯度进约束行/列（`gᵀ dx = gap/L`、`-λ g/F`），1e10 会同时把行放大到无意义、
把几何 gap（`gap/|grad|`）压成 0，结果是首个迭代局部丢秩（`failure=4`）
或乘子爆到 `|λ|/F ~ 1e3` 后耗尽预算（`failure=3`）。

修法（`RodKkt10SdfQuery`）：
1. 六个差分样本任一越界就判定该点场不可用，不猜外插；
2. `|grad|` 必须落在真实 SDF 应有的量级（0.05..20），否则同样判不可用；
3. 输出**单位法向**与 `rawGap / |grad|` 的**几何 gap**，让 SDF 与胶囊在 KKT 里量纲一致。
   对真实 SDF（`|∇d| = 1`）与 CPU 的原始梯度写法等价，对离散三线性场则条件数好得多。

改动后同一 `SC_rod_v3_sdf_16` 场景：16/16 收敛，`gpuStatus=0`，0 失败，
`currentBufferComponentResidualMax` 5.807e-05，400 帧 validation 0 条消息。
因此 `rodGpuRecording.h` 的 V3+SDF throw 已删除。

**但 SDF 在这份资产上实际不产生修正**：逐点统计代理有符号距离，26 个自由点穿透，
其中 25 个在 1.1 cm 以外（栅格中心距离超过 `truncation`，梯度采样必然越界），
只剩 1 个落在可用带内；而带宽内没有「浅穿透」的样本可处理（`(-0.5, 0)` 区间为空）。
`V3 胶囊+SDF` 与 `V3 仅胶囊` 的逐点差 mean 0.00005 cm、max 0.00087 cm，就是这个原因。
V1 的 SDF 有完全相同的盲区（同一批点最深 -4.25 cm、28 个穿透）。这是带宽/资产性质，
不是本轮改动的缺陷；要真正用 SDF 挡住这段穿模，得提高栅格分辨率或改带宽公式。

### 6. 重叠修复「修到哪」写错（第二轮）

上一版用同一个 `threshold = 1% * 总长` 既决定「要不要修」又决定「修到什么程度」，
于是修复会**故意留下最多 3.4 mm 穿透**交给牛顿。现改为两个阈值：`trigger`（1% 总长）
只决定是否介入，`target`（求解器自己的 gap 容差）决定修到贴面。
16-guide 结果不变（该场景的穿透都远超 trigger），但语义正确了。

## 实际证据

测试资产在资源 worktree 的 `Generated/Validation/`（gitignored，不入库）：
`TF_female_rod_v3_16.json` / `TF_female_rod_v3_200.json`（`solver=cosseratImplicitV3`）、
`TF_female_rod_v1_16.json`（同源 16 guide，`cosseratXpbdV1` 基线）、
`SC_rod_v3_capsules_16/200`（仅胶囊）、`SC_rod_v3_sdf_16/200`（胶囊 + `female_body_proxy.obj`）、
`SC_rod_v1_capsules_16`（V1 基线）。生成器是本轮 ad hoc 脚本，不属仓库。
`Maps/` 正式资产未改。

### 接触验证：16 guide / 400 帧，胶囊与 SDF 各一次（第二轮最终版）

`SC_rod_v3_capsules_16` 与 `SC_rod_v3_sdf_16`，`--frame-limit 60 --worker-thread-count 1
--no-dev-ui --exit-after-frames 400`，validation layer 打开，两次都是 0 条消息。

| 指标 | V3 胶囊 | V3 胶囊 + SDF | V1 基线 |
| --- | --- | --- | --- |
| `gpuStatus` / 失败 guide | 0 / 0 | 0 / 0 | 0 / 0 |
| `currentBufferComponentResidualMax` | 3.245e-05 | 5.802e-05 | 1.398e+00（V1 无此归一化） |
| 自由点最小胶囊 gap | **-0.0000 cm** | **-0.0000 cm** | **-0.0000 cm** |
| 全段拉伸最大 | +94.0290% | +94.0290% | +94.0558% |
| 自由段拉伸最大 / 最小 | **+0.9495%** / -0.0001% | +0.9498% / -0.0005% | **+9.4753%** / -0.558% |
| 代理带内穿透点数 | 1 / 26 | 1 / 26 | 2 / 28 |
| 包围盒 x/y/z（cm） | [-11.69,9.46] / [131.86,176.20] / [-16.09,10.36] | 同左 | [-11.70,9.48] / [131.84,176.18] / [-16.10,10.38] |
| 总位置 | 有限 | 有限 | 有限 |

时间债（`--frame-limit 60`）：胶囊 13.233 s accepted / 16.666 s received（债 3.40 s），
SDF 13.233 / 16.808（债 3.54 s）。两次运行的残差与轨迹会因实时 delta 不同而不同，
所以这里的数字是"该次运行"的值，不是可复现的定值。

- 自由点最小 gap 与 V1 完全同为 `-0.0000`：两个模型的接触都精确落到胶囊表面。
  被排除的 root 前缀点（TFX 第 0/1 点逆质量为 0，位于头皮内侧）在两者中都是 -1.5437 cm，
  属作者资产性质，不是求解器缺陷。
- **全段 +94% 拉伸在两个模型里是同一段**：guide `src12666` 的 segment 1
  （rest 0.3433 cm → 0.6661 cm），左端是 pinned root。把它算进来时 V3 与 V1 几乎相同
  （+94.029% vs +94.056%），所以这一项不是 V3 引入的。
- **只看自由段时 V3 明显更紧**：V3 +0.9495%（在 1% 目标内），V1 +9.4753%（9 倍于目标）。
  本轮轴限是关闭的，所以这不是"满足了 1% 不等式"，而是隐式接触解本身比 XPBD 位置投影
  在自由段上拉伸得少。这是一条有限观察，不是回正/风/性能验收。
- `V3 - V1` 逐点位置差：胶囊 mean 0.0624 / p95 0.1846 / max 0.4024 cm；
  SDF 版几乎相同（mean 0.0623 / max 0.4024 cm）。
  这是两个模型（XPBD 位置投影 vs 隐式活跃集 KKT）的差异，不是同一模型的两份实现。

### SDF 接触：第二轮已收敛，但在这份资产上不产生修正

第一轮记录的是 16/16 失败并保留 throw；第二轮定位并修掉根因（见上文“修掉的真实缺陷”第 5 条），
现在 `SC_rod_v3_sdf_16` 400 帧 `gpuStatus=0`、0 失败、残差 5.807e-05。
throw 已删除，SDF 与胶囊都可以走 V3。

`V3 胶囊+SDF` 与 `V3 仅胶囊` 的逐点差只有 mean 0.00005 cm、max 0.00087 cm：
仪器里 SDF 每 substep 每 guide 有 1–22 次查询被接受（不是被守卫全拒），
但这份 groom 的 26 个穿透自由点里 25 个比可用带（约 1.1 cm）更深，
带宽内没有浅穿透样本可推。所以"SDF 收敛且在工作，但本资产没有它能处理的重叠"。
要真正挡住这段穿模需要提高栅格分辨率或改带宽公式，属另一项决策。

### 200 guide 规模：3/200 失败（本轮未解决）

`SC_rod_v3_capsules_200`，200 帧：`gpuStatus=3`，`gpu-failed-guides.csv` 591 行，
197/200 guide 收敛（且稳态 0 轮即退出），3 个在首个瞬态耗尽 64 轮预算：

| guide (src) | 失败码 | merit | 迭代 | 活跃槽 | contact 残差 | 位置残差 | max λ/F | 回溯 | 修复 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 3510 | 3（预算） | 6.003e-04 | 64 | 9 | 2.93e-03 | **2.238e-02** | 0.50 | 372 | 0 |
| 4569 | 3（预算） | 8.803e-04 | 64 | 8 | 3.53e-03 | **2.339e-02** | 0.47 | 424 | 0 |
| 10794 | 3（预算） | 6.221e-02 | 64 | 10 | 1.47e-06 | **2.070e-01** | 57.1 | 454 | 3 |

前两个 guide 的 contact 残差已经很小（3e-3，约 1 mm），卡住的是**位置行**
（动量/弹性平衡 2.2e-2），且回溯 372/424 次说明线搜索每轮只能接受小步；
第三个是修复触发后乘子变大（λ/F 57）且位置残差 0.21。
因此规模瓶颈不是活跃集选错，而是**带接触项后 merit 线搜索在这些 guide 上收不动**，
属下一轮要攻的点（候选方向：把乘子延拓移出线搜索，或对位置行单独做 merit 缩放）。

失败是**粘滞**的：kernel 里失败的 guide 把 `failure` 写进 `rodState.guides[].diagnostic`，
下一帧开头的 `if (failure != 0u && rod.flags.x == 0u) return;` 会直接返回；
再叠加 C++ 侧 `state.gpuStatus != 0 -> steps = 0`，**一个 guide 失败会让整个对象冻结**。
所以把 runtime 切到 V3 会让 2000-guide 场景停摆，本轮**没有切换**：
`TF_female_rod_validation.json` 仍为 `solver=cosseratXpbdV1`，resources worktree 干净。
runtime 场景 `SC_tressfx_rod_static.json` 声明了 `hairCollision.mesh`，
即使切过去也已经是可用的组合（SDF 不再抛错），阻塞点只剩这 3/200 的收敛与粘滞策略。

### 第 10 分量（1% 上拉伸乘子）：实现但关闭

`VL_ROD_KKT10_AXIAL_LIMIT` 打开时（24 轮与 64 轮都试过），16 guide 场景里
`src12666` 与 `src16242` 在 `iteration 4–5` 丢秩，`|λ|/F` 到 1.0e3–1.7e3，
其余 guide 正常。这两个正是重叠修复触发的 guide：修复把点推出胶囊时局部段被拉长到
远超 1%（例如 0.34 cm → 0.67 cm），轴限随即要求压回，接触又要求推出，两个乘子对打。
修复与 1% 不可伸长约束的交互是 B 阶段（clump/体积）要一起重做的部分。
当前 comp 不定义 `VL_ROD_KKT10_AXIAL_LIMIT`，row 9 退化为单位行，等价于 CPU 的 9x9
接触系统（`SelectContactActiveSet`/`AssembleContactSystem`/`SolveContactSystem`）。

## 第三轮：隔离失败、否掉两个假设、定位运行时规模的真实阻塞

### 失败粘滞改为按 guide 隔离（已落地）

旧策略有两层粘滞：shader 把失败码写进 `rodState.guides[].diagnostic.x`，下一帧在
`if (failure != 0u && rod.flags.x == 0u) return;` 直接返回；C++ 侧再用
`state.gpuStatus != 0` 把 `steps` 归零。结果是**一个 guide 的牛顿预算耗尽让整个对象永久冻结**。

现在：
- shader 每帧从 0 开始重试（不再把上一帧的失败码读回 `failure`，也删掉那条早退）；
  失败的 guide 因为不发布而保持上一次接受的状态，其余 guide 继续推进；
- C++ 的 `steps` 不再受 `gpuStatus` 门控，`gpuStatus` 变成逐帧报告量（每帧从 0 重新求最大值），
  也不再改写求解器侧的时间债；
- `accepted` 仍是「所有 guide 都接受」的最小模拟时间，因此有 guide 停住时该列会停在它的时间上，
  这是有意的信息，调度不再依赖它。

实测（`SC_rod_v3_capsules_200`，200 帧）：对象持续派发（`dispatched` 6.6 s 单调增长），
197/200 guide 正常推进，3 个每帧重试并保持旧状态，`failedGuides` 列稳定为 3。
16-guide 胶囊与 SDF 两条路径回归：`gpuStatus=0`、0 失败、残差 7.757e-05 / 5.814e-05、
validation 0 条消息，没有退化。

### 两个被实测否掉的求解假设（保留为反例）

对着 3/200 卡住的 guide 试了两个改动，都更差，已回退：

| 改动 | 结果 | 结论 |
| --- | --- | --- |
| 接触二阶项换成 Gauss-Newton（`H=0`） | 失败 guide 3 → **4**，最大 merit 0.062 → **0.112** | 解析几何刚度是净收益，不能为条件数丢掉 |
| 线搜索期间冻结乘子（只接受后再延拓 λ） | 失败 guide 3 → **96**，merit 0.062 → **1246** | 乘子必须随 trial 一起延拓，否则解耦后失稳 |

两者都留在代码里以宏开关形式（`VL_ROD_KKT10_GAUSS_NEWTON_CONTACT`）和注释里。

### 失败状态导出：卡的是 merit 全局化，不是线性代数

用既有的 `VL_ROD_CAPTURE_SOURCE_GUIDE` 导出失败 guide（src 3510）的完整状态与 6x6 带状矩阵：
- 32 个点的**对角块行列式全部为正且量级 1e4–1e7**（lane 0 是单位行=1），
  说明线性系统本身不奇异，band LU 不是瓶颈；
- 残差集中在 root 附近 lane 2–8（`|r_lin|` 最大 0.51·F），向 tip 单调衰减到 1.4e-3，
  且方向沿重力（诊断值 `rlin[2] = (0.236, -0.410, -0.193)`）；
- 角向残差全程 ~1e-6，是纯平动不平衡。

所以瓶颈是**带接触项后 merit 线搜索在这些 guide 上只能接受极小步**（回溯 372–454 次），
不是装配或 LU 出错。下一轮的方向应是把位置行与接触行在 merit 里分开加权，
或给失败 substep 加一个「全量投影后重试」的阶梯，而不是继续调 Hessian。

### 运行时规模的真实阻塞：瞬态成本导致设备丢失

在 `--frame-limit 60 --gpu-stats` 下测（`perf-*.jsonl` 的 `hairSimulation` pass）：

| 场景 | solver | 首个阶梯帧 | 稳态 | 结果 |
| --- | --- | --- | --- | --- |
| 运行时 `SC_tressfx_rod_static`（2000 模拟 / 40334 根） | V1 | — | **6.83 ms**，60.1 fps | 正常 |
| 派生的 2000-guide 胶囊场景 | V3 | **超过 OS 单次提交上限** | — | `vk::DeviceLostError`（`waitForFences: ErrorDeviceLost`） |
| 200 guides | V3 | **476 ms** | 0.34 ms | 正常 |
| 16 guides | V3 | 76 ms | 36 ms（这批 guide 稳态仍需多轮） | 正常 |

`476 ms × 10 = 4.8 s`，远超 Windows 的 2 s 提交上限，2000 guide 必然 TDR。
成本按 guide 数线性放大，所以**这只是规模问题，不是算法错误**；稳态成本
（0.34 ms @200 ≈ 3.4 ms @2000）与 V1 的 6.83 ms 同量级。

结论：**runtime 不切**。切过去是硬崩溃，比留在 V1 更差。
`TF_female_rod_validation.json` 保持 `solver=cosseratXpbdV1`，resources worktree 干净。

要让 V3 在运行时规模可用，二选一（都属下一轮）：
1. **降低瞬态单帧成本**：给 V3 限制每帧 substep 数与迭代预算（有隔离后可以安全地跨帧摊开），
   代价是瞬态期间模拟落后实时；
2. **把 10x10 顺序 LU 改成分布式块循环约化**（当前是 32 个点顺序求解、全部 lane 冗余执行，
   单次 pivot 约 200+ 周期）。共享内存预算（48 KB）放不下 10x10 的 CR 工作集，
   这是第一轮就量过的硬约束，需要改数据布局（例如把因子放 global 并把 2000 guide 的
   工作集压在 L2 内）或缩小活跃集宽度。

## 第四轮：单帧预算与 6x6 压缩求解的尝试

### 单帧 substep 预算（已落地，必要但不充分）

`RodGpuV3HardFrameIterations = 12` / `RodGpuV3HardFrameSteps = 1`：用上一批读回里
单 guide 的最大牛顿轮数当“硬帧”信号，硬帧只派发 1 个 substep，稳态仍是 8 个。
实测 16-guide 两次运行（胶囊 / SDF）仍是 `gpuStatus=0`、0 失败、残差 5.805e-05，
而 `received 8.3 s / dispatched 6.45 s` 说明限制确实生效。

但它**不足以救 2000 规模**：`SC_rod_v3_capsules_2000` 在 `dispatched=0.00417`
（正好 1 个 substep）时仍然 `vk::DeviceLostError`。也就是说单个 substep 的
2000-guide dispatch 本身就超过 OS 提交上限，瓶颈必须从“每帧派发多少”转到
“每个 substep 多贵”。

### 6x6 压缩求解：推导对、GPU 实现没复现（已回退）

思路：10x10 的 KKT 块是 `[A  -G; G^T 0]`，轴限关闭时乘子无跨点耦合，把 m<=3 个
局部乘子消掉后**仍是块三对角**，可以复用 `rodCrBand.glsl` 的分布式块循环约化，
把「32 个点顺序求解 + 全 lane 冗余执行」换成「每 lane 一个块」：

```text
A dx_p + (Q_p L_p) dx_{p-1} + (Q_p U_p) dx_{p+1} = Q_p b_p + G_p S_p^-1 h_p
S_p = G_p^T A_p^-1 G_p,   Q_p = I - G_p S_p^-1 G_p^T A_p^-1
dlambda_p = S_p^-1 G_p^T A_p^-1 (A_p dx_p + L_p dx_{p-1} + U_p dx_{p+1} - b_p)
```

恒等式先用 Python 对 10n 维直接解校验：n=1..8、m=1..3、**A 取非对称**
（`RodV2Tangent` 的 6x6 块本来就不对称），相对误差 < 5e-15，全部 OK。
（第一版校验用了对称 A，掩盖了 `Q` 的右因子必须是 `G^T A^-1`、即需要 `A^-T g` 而不是
`A^-1 g` 这个坑；换成非对称 A 后才覆盖到。）

但 GLSL 实现没有复现旧路径的收敛：同一 16-guide 场景从 0 失败变成 6 失败，
`maxLam/F` 从工作路径的 ≤76 涨到 296，位置残差 0.92（工作路径 0.022），
16 个 guide 里 11 个仍收敛。症状是“方向不算下降方向”而不是发散到 NaN，
说明压缩后的系统与实测残差不自洽，但**没有定位到具体项**——按项对照 Python 与 GLSL
（S、Q、extra、乘子恢复的 index 顺序与列主序约定）都一致。

因此本轮**回退**到已证的 10x10 顺序求解（两个 shader 文件 `git checkout` 回 HEAD），
16-guide 胶囊/SDF 两条路径重新跑通：`gpuStatus=0`、0 失败、残差 5.805e-05、
validation 0 条消息。

下一轮重试这条路线前必须先有**离线的压缩系统对照**（把 `RodKkt10Condense` 的
六个输出写进 buffer，用 CPU 复算 `A dx + L dx- + U dx+ - G dlambda - b` 的范数），
而不是靠整场景收敛与否来反推——这一轮的教训就是整场景信号无法区分
“压缩错”和“CR 用错”。

## 第五轮：压缩代数的对照探针（代数已验证，CR 仍未接回）

上一轮的教训是「整场景收敛与否」无法区分「压缩代数错」和「压缩系统求解错」。
本轮补上了那个缺失的仪器：`VL_ROD_KKT10_CONDENSE_PROBE` 在**工作路径已解出
dx / dlambda 之后**，用它们反过来检验压缩恒等式，不改动工作解，因此可以跑真实轨迹：

```text
checkA = max |S^-1 G^T A^-1 (A dx + L dx- + U dx+ - b) - dlambda_10wide|   乘子恢复
checkB = max |G^T dx - h|                                                  接触行残差
```

16-guide 胶囊场景、帧 41 的实测（有活跃接触的 6 个 guide）：

| guide | 活跃槽 | max λ/F | checkA | checkB |
| --- | --- | --- | --- | --- |
| 12666 | 4 | 76.3 | 4.66e-10 | 3.33e-10 |
| 16242 | 3 | 76.4 | 7.63e-06 | 7.08e-16 |
| 16478 | 1 | 6.35e-04 | 1.16e-10 | 5.02e-15 |
| 21165 | 16 | 0.727 | 5.96e-08 | 6.59e-12 |
| 29512 | 6 | 1.71e-02 | 1.86e-09 | 5.94e-13 |
| 39804 | 9 | 0.226 | 1.49e-08 | 1.86e-12 |

**结论：压缩代数在真实状态上是对的**（乘子恢复与接触行都到 1e-6 以下）。
所以上一轮的失败只能在「压缩后的系统怎么解」这一侧——也就是当时直接接上的
`RodCrSolveBand32` 用 Q 修正后的块给不出与 10 宽解一致的方向。

一个必须记下来的诊断陷阱：第一版探针把 checkB 写成
`|A dx + Q(L dx- + U dx+) - Q b - G S^-1 h|`，实测约 `0.9 * max|lambda|/F`
（52.9 / 75.5 / 0.70 …），看着像压缩代数错了。但那个量恒等于
`|G S^-1 (G^T dx - h)|`，被 `|S^-1| ~ |A| ~ 1e3` 放大：10 宽求解器 1e-5 量级的
接触行容差会被放大成 1e-2 量级的假偏置。换成直接量 `|G^T dx - h|` 后立刻下降到
1e-10 以下。**任何在 λ 归一化单位下比较的量，都要先检查是否被 S^-1 放大。**

探针默认关闭（`#define` gated），关闭后 16-guide 胶囊路径仍是 `gpuStatus=0`、
0 失败、残差 5.802e-05、validation 0 条消息。

下一轮：把压缩系统接回 `RodCrSolveBand32`，用 checkA/checkB 之外再加一个
「dx_condensed 与 dx_10wide 的最大差」探针直接对照（需要在探针里临时保留 10 宽解，
共享内存预算允许，因为 10 宽路径本来就是常驻的）。

## 第六轮：压缩系统与 10 宽解逐分量一致（Q 的转置是错的）

在第五轮探针上加了解压缩系统本身的能力：用顺序块 Thomas（复用 10 宽那套主元与
factor 数组，零额外 shared）解压缩后的 6x6 系统，再和 10 宽解逐分量对照，
并直接测 `|Q G|`。16-guide 胶囊场景、帧 41、有活跃接触的 6 个 guide：

| guide | 槽 | 第五轮（Q 用 `A^-T g`） |Q G| / |dx 差| | 本轮（Q 用 `A^-1 g`） |Q G| / |dx 差| |
| --- | --- | --- | --- |
| 12666 | 4 | 8.14e-01 / 2.21e-01 | **0 / 0** |
| 16242 | 3 | 9.89e-01 / 1.46e+00 | **0 / 0** |
| 16478 | 1 | 7.38e-01 / 4.55e-04 | **0 / 0** |
| 21165 | 16 | 9.95e-01 / 9.50e-02 | **0 / 0** |
| 29512 | 6 | 7.83e-01 / 1.77e-02 | **0 / 0** |
| 39804 | 9 | 9.97e-01 / 5.84e-02 | **0 / 0** |

所以：**压缩后 6x6 系统的解与 10 宽 KKT 的解逐分量完全相同**（差为 0，不是 1e-6），
`Q` 必须用 `A^-1 g` 而不是 `A^-T g`。

这一轮同时纠正了第五轮的一个误判：第五轮我把 `checkB` 当成“Q 错了”的证据，把 `Q` 的右因子
从 `A^-1 g` 改成了 `A^-T g`——**那次“修复”本身就是 bug**。`checkB` 当时测的是
`|G S^-1 (G^T dx - h)|`，被 `|S^-1|` 放大，与 Q 无关；改成直接量 `|G^T dx - h|` 后
第五轮就已经是 1e-10。教训：先给每个诊断量找到它的代数恒等变形，再决定它能不能证明某件事。

至此压缩代数与压缩求解都被实测钉住，可以确定**第四轮接 CR 失败的根因在 CR 那一侧**，
压缩本身没有问题。这也解释了为什么第四轮（当时用的正是 `A^-1 g`）会失败。

默认编译不带探针：16-guide 胶囊路径仍是 `gpuStatus=0`、0 失败、残差 5.810e-05、
validation 0 条消息。

下一轮的净收益测算（决定要不要继续走 CR）：当前每 lane 执行整条 32 点顺序 Thomas，
10 宽每点约 2.6k 指令 → 每 warp 约 83k；压缩后每点约 1.3k（压缩）+ CR 分摊约几百，
但**必须用分布式 CR**才有这个量级的收益——若仍用顺序 Thomas，只有约 2x，不够 2000 guide。

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
  仍有 3.37 s/16.6 s（胶囊）与 3.59 s/16.86 s（SDF）的时间债，说明即使 16 guide
  也没跑满实时。40,334 根规模、风/回正与性能属 C 阶段。
- 200/2000 guide 规模与失败粘滞策略是**当前最主要的未解决项**，也是 runtime 切换的唯一阻塞点。
  SDF 接触本身已不再是阻塞点（第二轮修好），但它在当前资产上不产生修正。
  `SC_rod_v3_sdf_16/200`、`SC_rod_v3_capsules_200` 只在本地 `Generated/Validation` 存在，
  不是入库契约。`SC_rod_v3_sdf_wide*_16`（第二轮试图用更大作者 cellSize 拓宽带宽）无效：
  分辨率被 clamp 到 48..64，作者 cellSize 只能改分辨率、改不了 `truncation = margin + 2*实际格距`，
  已废弃，不入库。
- 渲染效果（`M_strandPbr` 只是 DefaultLit 预览）不在本轮范围，未改动。
