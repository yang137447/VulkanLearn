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

## 第七轮：把压缩求解接成默认路径——失败，且发现第六轮的验证是空验证

本轮把 `RodKkt10Condense` + 6 宽块 Thomas + 乘子恢复接成默认求解（旧 10 宽路径保留作对照），
结果 16/16 guide 失败。两个发现：

### 1. 6 宽 Thomas 的右端符号写错（已修）

压缩后的 `right` 已经是 `b = -(10 分量残差)`，块 Thomas 解的是 `M d = right`，不能再取负；
我照抄了 10 宽路径里「把残差取负成 b」的那一行，于是解出的是 `-d`。
这个错在第六轮的探针里完全看不出来，因为探针在**收敛状态**上比较
`|dx_condensed - dx_10wide|`——那时 `dx ~ 0`，`-0` 和 `+0` 一样，量出来就是 0。

**所以第六轮「压缩系统与 10 宽解逐分量一致」的结论是空验证，本轮撤回。**
仍然成立的是同轮的合成量检验 `|Q G| = 0`（用一个固定探针向量，与是否收敛无关）
以及 `checkA`/`checkB`（在 λ/F 达到 76 的状态上测的，也非空）。

### 2. 修符号后仍然失败，且只失败在带活跃接触的 guide

`|Q G| = 0`、`checkA ≈ 0` 都成立，说明 S、A^-1、G、Q 与乘子恢复没问题；
但换成压缩系统驱动后，**无接触的点/guide 全部正常**，带接触的 6 个 guide
（12666/16242/16478/21165/29512/39804）仍在首个瞬态失败（merit 8.5、
位置残差 0.9、maxλ/F 到 84）。所以剩下的嫌疑集中在压缩系统的**装配**
（`Q L`、`Q U`、`Q b + G S^-1 h`）或它的求解，而不是压缩代数本身。

已回退：kernel 回到 `RodKkt10SolveBand(32u)`，压缩实现与探针重新 gated。
16-guide 胶囊/SDF 两条路径重新跑通：`gpuStatus=0`、0 失败、残差 5.807e-05、
validation 0 条消息。

### 下一轮的验证指标（非空）

必须量**压缩系统的求解残差** `|M d - b_right|`（M 为压缩后的块三对角、
b_right 为其右端），它在任何状态上都有意义，不依赖是否收敛。
再配合「在非收敛帧上」比较 `|dx_condensed - dx_10wide|`（例如只跑 3 帧）。

## 第八轮：成本随 guide 数的缩放实测，以及探针可信度的教训

### 缩放曲线（本轮新增的关键测量）

沿用 2000-guide 派生资产，只改 `simulationGuideCount`，`--frame-limit 60 --gpu-stats`：

| 模拟 guide 数 | `hairSimulation` | 结果 |
| --- | --- | --- |
| 16 | 36 ms（稳态，含难 guide 的多轮迭代） | 正常 |
| 200 | 476 ms（首个阶梯帧）/ 0.34 ms（稳态） | 正常 |
| 400 | **607.88 ms** | 正常，`gpuStatus=3`（5/400 guide 失败，已隔离） |
| 800 | — | **帧 1 即 `vk::DeviceLostError`** |
| 1600 / 2000 | — | 同上 |

400 → 800 直接越过 2 s 提交上限，说明成本**不是线性**（400 guide 时单帧 608 ms，
按线性外推 800 只有约 1.2 s）。合理原因是共享内存把 occupancy 压到 1 workgroup/SM，
再到 800 guide 时 L2 装不下 13 MB 的 state 反复读写。

结论：**光靠限制 substep 数救不了 2000 规模**（上一轮已验证 1 substep 仍然 TDR），
必须把每个 substep 的求解成本降约 10x。400 guide 能在 608 ms/帧下工作，
说明功能上没问题，纯是成本。

### 探针可信度：本轮又一次假读数

本轮把探针改成量**压缩系统的求解残差** `|M d - b_right| / |b_right|`（与是否收敛无关），
实测 3.3e-6 – 8.5e-6，说明块 Thomas 本身是准的。但同一批读数里
`|Q G|` 仍是 0.74 – 1.00——也就是说上一轮把 `Q` 从 `A^-T g` 改回 `A^-1 g` 之后，
`Q` 依然不清零 `G`，而第六轮那次读到的「0/0」是又一次**假读数**
（`energies.zw` 当时并没有真的写进探针值）。

所以现在的确定结论只有两条：
- 压缩**系统**的求解（块 Thomas）是准的（相对残差 1e-5 量级）；
- 压缩**装配**（`Q` / `Q L` / `Q U` / `Q b + G S^-1 h`）仍然不对，`|Q G| ≈ 1` 是直接证据。

教训（第二次记）：**每次加探针都要先用一个已知答案的输入自检**。本轮和第六轮
各自因为「探针写到了没人读的槽位」而把 0 当成了通过。可用的自检是
`|Q G|`（已知必须为 0）与 `|M d - b|`（已知必须远小于 `|b|`）同时成立——单看一个都可能假。

默认编译不带探针，16-guide 胶囊路径仍是 `gpuStatus=0`、0 失败、残差 5.810e-05、
validation 0 条消息。

## 第九轮：压缩求解接成默认路径成功，2000 guide 不再 TDR

### 关键修复：`Q` 必须逐项显式构造

上一轮已经把问题锁定在 `|Q G| ~ 1`。单接触点上 `Q g = g - (1/s) g (g^T A^-1 g) = 0`
是解析恒等式，所以「单接触点 |Q G| 必须为 0」可以直接当已知答案自检。
本轮把 `Q` 从 `RodKktOuter + RodV2Add` 改成**嵌套循环 + `RodV2Set` 逐项写**，
两个自检量同时落地：

| 指标 | 用 `RodKktOuter` 拼 | 逐项显式构造 |
| --- | --- | --- |
| `|Q G|`（必须为 0） | 0.74 – 1.00 | **4.7e-08 – 1.2e-07** |
| `|M d - b|/|b|`（必须远小于 1） | 3.3e-06 – 8.5e-06 | **3.6e-12 – 6.5e-11** |

也就是说 **`RodKktOuter` 拼出来的矩阵不是 `g t^T`**——这是从第四轮起所有压缩求解失败的
共同根因（第四轮接 CR 时用的也是这个坏 `Q`，所以 CR 本身很可能一直是好的）。

### 接成默认路径后的实测

| 场景 | 结果 |
| --- | --- |
| 16 guide 胶囊 | `gpuStatus=0`、0 失败、残差 **2.488e-05**（10 宽路径 5.807e-05，改善 2.3x） |
| 16 guide 胶囊+SDF | `gpuStatus=0`、0 失败、残差 **2.486e-05** |
| 200 guide / 400 帧 | `gpuStatus=4`、4/200 失败（10 宽路径 3/200），merit 9.38 |
| 400 guide | 通过，133.0 ms |
| 800 guide | **通过**（10 宽路径在这里帧 1 就 DeviceLost），254.5 ms |
| **2000 guide（runtime 规模）** | **通过，38 帧，exit 0，不再 TDR**；28/2000 guide 失败、`hairSimulation` ~90 ms |

同一批 16-guide 运行的 dispatch 时间从 10 宽路径的 6.45 s 降到 3.53 s（198 帧）。

所以现在的能力边界是：**V3 能在 runtime 规模上跑起来而不炸 GPU**，代价是
1.4% 的 guide 求解失败（已按 guide 隔离，不冻结整个物体）和 ~90 ms/帧。
16 guide 的残差比 10 宽路径更好，说明求解本身更准；失败率的上升来自压缩路径缺少
10 宽路径那套 supernode 兜底——`RodKkt10Condense` 在 `A` 或 `S` 奇异时直接
`valid=false`，`RodKkt10SolveCondensed` 于是让整个 guide 失败。

### 下一轮

给压缩路径加**正则化兜底**而不是硬失败：`A` 求逆失败时加 `eps*scale*I` 重试一次，
`|det S|` 过小时给 `S` 对角线加 `eps*trace(S)`。这样退化点只会得到一个近似方向，
交给线搜索决定接受与否，而不会让整个 guide 掉出 KKT 秩。目标是把 2000 guide 的
28 个失败压到个位数，再把 runtime 资产切到 V3。

## 第十轮：退化点正则化兜底，以及 shader cache 的一个坑

### 正则化（不再硬失败）

压缩路径没有 10 宽那套 supernode 兜底，`RodKkt10Condense` 在 `A` 或 `S` 奇异时直接
`valid=false`，整个 guide 就掉出 KKT 秩。改成三处「加相对 eps 再试一次」：

- `RodV2Inverse(A)` 失败 → `A += 1e-6 * max(diag, 1) * I` 重试；
- `abs(det S)` 过小 → `S += 1e-6 * trace(S) * I` 重试；
- 6 宽主元失败 → `diagonal += 1e-6 * max(diag, 1)` 重试。

这样退化点只得到一个近似方向，交给 merit 线搜索裁决，而不是让整个 guide 失败。

| 场景 | 正则化前 | 正则化后 |
| --- | --- | --- |
| 200 guide / 400 帧 | 4 失败，merit 9.38 | **3 失败**，merit **0.207** |
| 2000 guide / 120 帧 | 28 失败，merit 3.81e+04，maxBlockDefect 123.2 | **21 失败**，merit **0.79**，maxBlockDefect **0.602** |
| 16 guide 胶囊 | 0 失败，残差 2.488e-05 | 0 失败，残差 **2.488e-05**（不变） |
| 16 guide 胶囊+SDF | 0 失败，残差 2.486e-05 | 0 失败，残差 **2.486e-05**（不变） |

### shader cache 的坑

改完 `rodKkt10BandSolve.glsl`（被 `rodV2Kernel.glsl` 传递包含）之后，胶囊路径读到了新代码，
但 `rodV3ImplicitSdf` 那条管线仍在用旧 SPV：同一场景同一帧读到 5.810e-05（旧值）而不是
2.486e-05（新值）。加 `--shader-force-rebuild` 后立刻变成 2.486e-05。

所以**改共享 hair 求解器 include 之后，SDF 管线要用 `--shader-force-rebuild` 验证一次**，
并用残差数值确认它真的换了代码，不能只看「进程起来了」。

### 成本真相（决定 A 能不能切）

同一 2000 guide 规模：V1 的 hair pass 是 **6.83 ms**，V3 现在是 **607 ms**，差 **~89x**。
第九轮的压缩求解已经把「TDR」变成「能跑」，但 60 fps 需要再降两个数量级：

- 分布式 CR：约 6x（把 32 点顺序 Thomas 的 lane 冗余去掉）→ ~100 ms；
- 再降牛顿轮数（现在难 guide 要 64 轮）→ 可能再 8x → ~12 ms。

也就是说 A 的「切 runtime」还需要两步结构性的成本下降，而不是继续调求解器细节。
在 607 ms / 帧、1.05% guide 失败的前提下切 runtime 资产是不合适的。

## 第十一轮：分布式 CR 接上，2000 guide 从 607 ms 降到 ~85 ms

`rodCrBand.glsl` 其实早就被 kernel 在 `VL_ROD_V3_CR_SOLVE` 下包含了，所以把压缩后的
6x6 系统交给 `RodCrSolveBand32` **不增加 shared 内存**（那 20 KB 本来就在预算里）。
`RodKkt10SolveCondensed` 里那段「32 点顺序 Thomas」换成 CR，乘子改从 `rodCrSolution`
读邻居方向恢复。

| 指标（2000 guide） | 顺序 Thomas | 分布式 CR |
| --- | --- | --- |
| hair pass | 607 ms | **84.4 – 94.1 ms**（约 7x） |
| 失败 guide | 21 | 20 |
| maxBlockDefect | 0.602 | 1.344 |
| merit | 0.79 | 1.856 |

16 guide 胶囊/SDF 仍然 `gpuStatus=0`、0 失败。胶囊残差从 2.488e-05 变成 5.800e-05
（多次 Schur 补会损失一些精度，相对于整带分解），SDF 仍是 2.488e-05，两者都远在
1e-04 收敛阈值内。V1 在同规模是 6.83 ms，所以 V3 从 **89x 降到约 13x**。

### 剩下的成本杠杆（按收益排序）

1. **每帧求解次数**：现在是 8 substep x 最多 64 轮牛顿。难 guide 吃满 64 轮，
   但只占 1%，所以限制轮数对总量影响约 10%，不是主要问题。
2. **substep 数**：8 降到 2 就是 4x，但会改变行为，需要单独验收。
3. **occupancy**：当前 shared 约 43 KB（rodCr 20 KB + rodKkt 23 KB），
   1 workgroup/SM。把已经死掉的 10 宽数组（`rodKktFactorUpper/band/work*`，约 17 KB）
   删掉能降到约 26 KB——仍然只够 1 个 workgroup（需要 <= 24 KB 才能翻倍），
   所以这条的收益不确定，优先级最低。

## 第十二轮：runtime 资产切到 cosseratImplicitV3

`Maps/SC_tressfx_rod_validation/Meshes/TF_female_rod_validation.json` 的
`simulationDefaults.solver` 从 `cosseratXpbdV1` 改成 `cosseratImplicitV3`
（`solverProfile` 保持 `effectiveBundleV1`）。该 map 下四个场景都引用这个资产，
所以四个场景一起切了。

先用 `Generated/Validation/SC_tressfx_rod_runtime_v3.json`（同一 groom 的派生副本）
验证，再改正式资产，然后跑正式场景：

| 场景（2000 模拟 guide） | 结果 |
| --- | --- |
| `SC_tressfx_rod_animated` | exit 0，稳态 defect 0.139、3 个 guide 失败、validation 0 条 |
| `SC_tressfx_rod_sdf_animated` | exit 0，稳态 defect 5.09、22 个 guide 失败、validation 0 条 |

SDF 变体明显更差，但这与 V1 共享同一个已知限制：深层作者穿模落在
`margin + 2*cellSize` 截断带之外，代理场看不见（见本文档前几轮的证据）。
约 1% 的 guide 会耗尽牛顿预算失败，失败按 guide 隔离并在下一帧重试，
不会冻结整个物体——这一点很重要，早期版本是整物体冻结。

**这次切换只声明正确性范围**，不声明性能：2000 guide 的 hair pass 约 80 ms
（RTX 3070 Laptop），离 60 fps 还差约 5x。全头风/回正/成本验收属于后续阶段。

### A 阶段状态

A 的两个子目标都已达成：
- 最终模型在 GPU 上带胶囊/SDF 接触跑通（16 guide 零失败、残差 2.5e-05 量级；
  2000 guide 稳态残差 1e-04、约 1% 瞬时失败）；
- runtime 从 V1 切到最终模型（正式资产已提交）。

遗留的、属于后续阶段的债务：80 ms 成本（需要 substep 数或每解成本再降约 5x）、
SDF 深层穿模盲区（资产/碰撞体层面，与 V1 共享）。

## 第十三轮：runtime 规模失败率的准确画像

之前几轮把「约 1% guide 失败」当成稳态值，本轮用 `gpu-failed-guides.csv`（正式资产、
150 帧、无 debug）把它拆开：

| 指标 | 实测 |
| --- | --- |
| 失败行总数 | 1092 行 / 138 帧 |
| 稳态（帧 > 10） | **7.2 个 guide/帧**，即 **0.36%** |
| 瞬态峰值 | 帧 2–3 分别 23 / 38 个（启动过渡，之后回落到 3–8） |
| 失败码 | 全部 `3` |
| 迭代数分布 | 395 行（36%）吃满 64 轮预算；697 行（64%）在 10–22 轮就退出 |

失败码 3 有两个来源：`iteration == RodV2IterationBudget`（第 961 行）和
`v2Accepted == 0`（第 1085 行）。后者的含义是 **21 次回溯都没找到可接受的步长**。

关键的推论：α 从 1 减半到 `2^-20`，Armijo 判据是
`merit_new <= (1 - 1e-4*alpha) * merit`；在 α = 2^-20 时右边等于
`(1 - 9.5e-11) * merit`，实质上就是「任何下降都接受」。也就是说这些 guide 的
merit **在这条方向上根本不下降**，不是回溯次数不够、也不是线搜索阈值太严。

所以剩下的不是线性代数问题（压缩系统的求解残差已经是 1e-10 量级），而是
**merit 本身的性质**：`RodActiveMeasure` 把 10 分量残差直接平方求和，位置行是力的
量纲（O(1)），接触行是 gap/L 的量纲（O(1e-3)）——接触项在 merit 里被压了约 1000 倍，
线搜索看不见接触的改善。下一轮可试的最小改动是给 merit 里的接触行乘以 `F/L`
使两类残差同量纲，再看 0.36% 会不会下降；这只影响 merit，不改 `v2Maximum` 的收敛判据。

## 第十四轮：三个否定结果，把 0.36% 的失败逼到一个很小的范围

本轮做了三次针对性实验，全部是**否定**结果——但它们把范围收得很窄。

### 1. 方向不是上升方向（排除了 Jacobian 不自洽）

在 kernel 里加了一个诊断：每 lane 计算 `F^T d`（10 分量残差与方向的点积），
取正部按位 `atomicMax`。牛顿方向必须满足 `F^T d < 0`。实测 2000 个 guide 里
**没有任何一个 lane 的 `F^T d` 为正**。所以「装配的 Jacobian 与实测残差不自洽」
这个假设被排除，压缩求解给出的确实是下降方向。

### 2. 失败时残差早就远低于容差

失败 guide 的 merit 常在 1e-13 量级（逐分量约 1e-7），而收敛判据是
`v2Maximum <= implicitControls.originTolerance.w`（**1e-4**）。所以它们不是
「残差没降到容差」——残差早就够了，卡住的是与残差无关的另外两个门：
`rodKktFeasible` 与 `v2DomainValid`。

### 3. 负乘子不是卡点

把负乘子在接受步之后投影到 0 是个正确的 KKT 投影（不等式约束要求 λ ≥ 0），
但实测**没有变化**：0.36% → 0.336%（噪声内），defect 还从 0.15 涨到 2.34。
所以「负乘子导致可行性判定永久失败」这个假设也被排除，改动已回退，
代码保持在最后一次验证过的状态。

### 剩下的唯一嫌疑：`v2DomainValid`

试算接受需要 `v2DomainValid != 0`，它是 32 个 lane 的 `v2Metric.w` 的最小值；
而 `RodKkt10Measure` 里判定 valid 的条件之一是

```text
length(RodLog(conj(oldOrientations[lane]) * v2Orientations[lane])) < RodRotationLimit
```

`oldOrientations` 是**子步开始时**的姿态，不是上一次迭代的姿态。也就是说这是
**累积**转角判据：一个 guide 在一个 substep 内累计转过超过 `RodRotationLimit` 之后，
无论 α 取多小，试算都不可能再 valid（α→0 也消不掉已经累积的转角），于是线搜索
必然失败——这正好解释了为什么失败集中在 `SC_tressfx_rod_animated`（身体带动头发的
动画场景）而静态场景少得多。

下一轮的验证方式很直接：`v2Metric.z` 目前是未使用的 0，把 validity 的具体失败原因
（incrementValid / 非有限 / NaN 残差 / 转角 / 关节）编码进去，用 debug 通道读出来，
就能确认是不是转角这一条。若确认，修法是让转角判据对齐「本子步内的增量」而不是
「相对子步起点的累积」，或者对超限 guide 采取分段推进。

## 第十五轮：两个探针都不可信，先修诊断通道

本轮试图把「0.36% 失败」的最后一个嫌疑（`v2DomainValid`）变成测量，结果**两个探针
都无法自证**，所以本节只记录方法结论，不记录任何新的失败机理。

### 探针 1：validity 原因码 —— 不可信

在 `RodKkt10Measure` 里把 validity 的失败原因编码进 `v2Metric.z`
（1=增量角度超限、2=位置非有限、3=残差 NaN/Inf、4=累积转角超限、5=关节转角超限），
归约到 `rodKktReason`，再写进 `energies.z`。实测 2000 个 guide 全是 0。

### 探针 2：最后一次回溯的 merit 比 —— 不可信

把「最后一次回溯（α = 2^-20）的 merit / 迭代起点 merit」写进 `energies.z`。
实测**全部**是 0，包括通过的那些 guide——而通过的 guide 根本不会走到 α = 2^-20，
按设计应当读到重置值 **1.0**。读到 0 说明 `energies.z` 这条通道没有承载这个值。

### 方法结论

`energies` 字段（`rod-diagnostics.bin` 的对应列）**不能作为诊断通道**，至少在没有
先验证的情况下不能。这也回头污染了第六轮那次「`|Q G|` 读到 0」的结论——同样的通道。

所以：**下一轮的第一件事是搭一条可自证的诊断通道**。仓库里已有现成且 schema 明确的
通道可用：失败导出缓冲（`lateMatrices` / `lateState` + `--` 的
`root-failure-encoding3-<frame>.{matrix,candidate,params,palettes,controls}.bin`），
之前用它 dump 过带状矩阵，字段含义是确定的。把探针写进那里，并且**先用一个已知非零的
输入（比如恒写 42）确认能读回来**，再谈结论。

在此之前的正确做法是不要再猜机理。当前代码已回退到上一次验证过的状态：
`SC_tressfx_rod_animated` 跑 100 帧 exit 0、validation 0 条消息、稳态失败 1–7 个 guide。

### 关于是否继续投入这个 0.36%

本轮起我建议把 0.36% 的失败**暂时接受为已知债务**：它按 guide 隔离、不冻结物体、
无 validation 错误，而且 A 的功能与切换目标都已达成。相比之下，C 阶段明确要求
40,334 根规模的风/回正/性能验收，而 2000 guide 现在就要 80 ms——那是更大、定义更
明确、且用户已经写进目标里的问题。继续在没有可信仪器的前提下追 0.36%，性价比明显更低。

## 第十六轮：信任域（回溯起点）把稳态失败从 0.36% 降到 0.28%

上一轮证明 `energies` 这条探针通道读不出我新加的共享变量（通过的 guide 也读到 0，
而按设计应读到重置值 1.0），所以本轮不再依赖它，改成**直接改行为再量结果**。

### 改动

回溯永远从 `v2Alpha = 1` 开始。对难 guide 来说第一步过大，于是一路砍到
`2^-20`；而那时位置增量 `alpha * v2LengthScale * direction` 已经落到 fp32 世界坐标
（约 100 cm 量级）的精度以下，试算点与起点**完全相同**，merit 自然一动不动，
Armijo 永远不满足。

改成**信任域**：先按段长与角度上限算一个安全步长，回溯从它开始，而不是从 1 开始。

```text
位置步上限  RodKktStepFraction(0.25) * restStrains[lane].w
旋转步上限  RodKktAngularStepLimit(0.125) rad
alpha_start = min over lanes of 上述比例（clamp 到 [1e-6, 1]）
```

缩放的是 `alpha` 而不是方向本身，所以 10 个分量（含接触乘子）仍然彼此一致——
等价于把线搜索的起点右移，而不是截断牛顿方向。

### 实测（`SC_tressfx_rod_animated`，2000 guide，150 帧）

| 指标 | 改动前 | 改动后 |
| --- | --- | --- |
| 稳态失败率 | 0.36%（7.2/帧） | **0.283%（5.65/帧）** |
| 吃满 64 轮预算的失败行 | 395 | **132**（−67%） |
| 线搜索耗尽的失败行 | 697 | 761 |
| hair pass | 80–94 ms | 87–95 ms（噪声内，无回归） |

16 guide 胶囊/SDF 仍是 `gpuStatus=0`、0 失败、残差 5.800e-05 / 2.488e-05。

结论：信任域确实消掉了「步长掉到精度以下」那一类（预算类失败少了 2/3），但对
「线搜索耗尽」那一类几乎没动——后者另有原因，而且现在成了主要来源（761/893）。
下一轮该改的是这一类，而它需要的正是上一轮没搭成的那条可自证通道。

## 第十七轮：诊断通道已自证可用，但 shared 内存到顶了

### 通道自证：通过

上一轮说 `energies` 通道「读不出新变量」——那个结论现在可以修正为
**通道本身是好的，是我写进去的值确实是 0**。做法：把 `diagnostic` 这个字段
（它的 `v2Maximum`/`v2Merit`/`v2Iterations` 一直在被我正确读出）改成写已知常量

```glsl
rodState.guides[slot].diagnostic = vec4(float(failure), 42.0, 43.0, float(v2Iterations));
```

实测侧车 `row[1]` 全为 **42**、`row[2]` 全为 **43**，自检 PASS。

顺带纠正结构认知：`diagnostic` 这一路一直是可靠的，所以**前几轮读到的
「reason = 0」是真的**——`v2DomainValid` 在最终 measure 上确实没有失败。
那个否定结论可以保留，`v2DomainValid` 不再是嫌疑。

### 但真正要测的量加不进去了

把 `diagnostic.y` 换成「最后一次试算的 merit / 迭代起点 merit」之后，
2000 guide 的正式场景**连续两次 device-lost**（`-1073740791`），150 帧和 50 帧都一样。
这次改动只加了两个 `shared float`（8 字节），不碰物理。

结论：**V3 的 KKT 管线已经贴住 48 KB 的 `maxComputeSharedMemorySize` 上限**，
再加任何 shared 变量都会把管线打坏。这解释了为什么三番两次的探针都不成功——
不是写错，是没有空间。

### 下一轮：先腾 shared，再测

死掉的 10 宽求解器还占着约 17 KB shared（`rodKktFactorUpper[32]` 12.8 KB、
`rodKktBand[2]` 2.5 KB、`rodKktFactorRight/Direction`、`work*` 三个工作区）。
`RodKkt10SolveBand` / `RodKkt10FactorNode` / `RodKkt10FactorSupernode` /
`RodKktEquilibratedPivot` / `RodKkt10AssemblePoint` / `RodRkt10ProbeFactor6`
及其探针驱动都已经没有调用点（默认路径走压缩 6x6 + CR）。

删除它们有三个收益：腾出诊断与后续功能需要的 shared 空间；shared 从约 43 KB
降到约 26 KB，虽然还不够翻倍 occupancy（需要 ≤ 24 KB）但更接近；编译期也能少一堆死代码。
风险是纯粹的死代码删除，编译器会逐条报错，因此可验证。

当前代码已回退到上一次验证过的状态：`SC_tressfx_rod_animated` 跑 100 帧 exit 0、
validation 0 条消息、稳态 6 个 guide 失败。

## 第十八轮：删掉死代码腾出 shared，探针终于跑通，结论反转

### 死代码删除（已验证）

默认路径早就是压缩 6x6 + 分布式 CR，10 宽那套整条链都是死代码。删掉
`RodKkt10Band` / `RodKkt10AssemblePoint` / `RodKktEquilibratedPivot` /
`RodKkt10FactorNode` / `RodKkt10FactorSupernode` / `RodKkt10SolveBand` 及其工作区，
以及只服务它们的探针驱动和 **18 个 shared 声明**。

| 指标 | 删除前 | 删除后 |
| --- | --- | --- |
| shared 占用 | 约 43 KB | **约 26 KB** |
| hair pass（2000 guide） | 87–95 ms | **73.8–80.2 ms** |
| 稳态失败率 | 0.283% | 0.289%（噪声内） |
| 帧 118 | — | `gpuStatus=0`、0 失败、defect 9.98e-05 |

16 guide 胶囊/SDF 不变且绿（5.800e-05 / 2.488e-05，0 失败），runtime 120 帧
validation 0 条消息。

### 探针跑通了，而且推翻了我原来的假设

有了 shared 余量之后，merit 比探针一次就跑通（上一轮同样改动直接 device-lost），
这本身印证了「48 KB 顶到天花板」的诊断。

**两个关键读数**：

1. 失败 guide 在最后一次试算的 `merit / 迭代起点 merit` 是 **0.004 – 0.76**，
   即 **merit 确实下降了**，比值远小于 Armijo 右端（≈1）。20 个失败里 19 个如此。
   所以「merit 不下降」这个前提是错的——我前面的推理一直建立在这个错误前提上。
2. 用同一通道读 validity 原因码：9 个失败 guide 里 8 个 **reason = 0**（该 lane 状态合法）。

### 一个具体的解释：`energies` 被第二个 kernel 覆写

`rodKernel.glsl:694` 也会写 `rodState.guides[slot].energies = energy`。也就是说
`energies` 这条字段被两个 kernel 写，sim kernel 写完会被 follow kernel 覆盖，
所以之前几轮写进 `energies` 的探针值读出来都是 0；而 `diagnostic` 只由 sim kernel 写，
用常量 42/43 自证 PASS。这解释了全部「通道坏掉」的假象。

### 现在的矛盾点（下一轮的起点）

按现存代码，试算被接受的条件是

```text
v2DomainValid != 0 && v2Merit <= (1 - 1e-4 * v2Alpha) * merit
```

而实测：merit 比 0.004（下降 99.6%）、lane 状态合法（reason 0），却仍然 `v2Accepted == 0`
并走到 `RodFail(3u)`。说明我对这条接受路径的理解还不完整。下一轮该做的是**逐行读**
kernel 第 1028–1091 行与 `merit` 的定义（它是不是每个 lane 各自持有的私有副本、
是否在 trial 之间被改写），以及 `v2DomainValid` 是否走了
`isnan(v2Merit) || isinf(v2Merit)` 那条置零分支——那是唯一不依赖 lane 合法性的置零路径。

## 第十九轮：拒绝步长的不是 merit，是 `v2DomainValid`

上一轮的矛盾（merit 比 0.004 却仍 `v2Accepted == 0`）本轮读代码读清楚了。
逐个查过接受判定的输入之后，第 1096 行只有两个条件，而我把**唯一没直接测过的那一个**
测了：

```text
if (lane == 0u && v2DomainValid != 0u && v2Merit <= (1 - 1e-4*alpha) * merit)
    v2Accepted = 1u;
```

把 `v2DomainValid` 直接写进 `diagnostic.z`（已自证的通道）后，失败 guide 的读数是
**`v2DomainValid = 0`，同时 merit 比是 0.03 / 0.06 / 0.11 …**（都远小于 1）。
所以：**步长是被「域不合法」拒掉的，不是被 merit 拒掉的。** 前几轮我在 merit 上
找原因，方向一直是错的。

顺带澄清两个读数陷阱：

1. `v2Iterations` 只在**接受**一次迭代时自增（第 1111 行），所以失败 guide 读到的
   `iters = 9/12/14` 是「接受了几次」，不是「尝试了几次」。我此前把它当尝试次数用过。
2. 探针变量在「试算循环根本没进」的 guide 上会留上一次的残值（读到的 7835 / 26 / 70
   就是这种残值），所以只看**失败 guide 且非残值**的行才有意义。

### 为什么域会不合法（下一个待验证点）

`v2DomainValid` 在归约里只有两条置零路径（`rodKkt10BandSolve.glsl:733-742`）：
某个 `v2Metric[point].w == 0`，或者 `v2Merit` 是 NaN/Inf。

而 `v2Metric.w == 0` 的判据之一（`RodKkt10Measure`）是

```text
length(RodLog(conj(oldOrientations[lane]) * v2Orientations[lane])) < RodRotationLimit
```

关键事实：**`oldOrientations` 每个 substep 只刷新一次**（`rodV2Kernel.glsl:871`
`oldOrientations[lane] = orientations[lane];`，在牛顿循环之前）。所以这个判据是
**相对子步起点的累积转角**，不是相对上一次迭代。一个 guide 在一个 substep 内
累积转过 `RodRotationLimit` 之后，无论 α 多小，试算都不可能合法——这正好解释了
「merit 明明下降了却一律被拒」。

### 下一轮

先用已自证的通道把「是不是累积转角这一条」确认掉（把 validity 的具体分支编码进
`diagnostic`），再决定修法。方向上有两个候选：

- 把转角判据改成相对**上一次被接受的状态**的增量，而不是相对子步起点；
- 或者把「累积转角超限」当成「本子步到这里为止」的合法终止条件（保留当前状态、
  不算失败），而不是「域不合法 → 全部拒绝 → RodFail(3)」。

后者更保守，也更符合该判据本来想表达的「安全上限」语义：触发上限时应当停下，
而不是把整个 guide 判失败。

当前代码已回退到上一次验证过的状态：runtime 120 帧 exit 0、validation 0 条消息。

## 第二十一轮：A 阶段收尾验收（runtime map 四个场景全覆盖）

> **注：本节表格是 SDF 单位缺陷修复前的记录，已被后面「最终验收」一节取代。**

之前只复验了切换后 `SC_tressfx_rod_validation` 里两个动画场景，本轮把四个场景都跑了一遍
（`--shader-force-rebuild`、150 帧、validation layer 打开）：

| 场景 | exit | validation | 帧 148 失败 guide | maxBlockDefect | hair pass |
| --- | --- | --- | --- | --- | --- |
| `SC_tressfx_rod_animated` | 0 | 0 条 | 1 | 0.295 | 51.0 ms |
| `SC_tressfx_rod_sdf_animated` | 0 | 0 条 | 2 | 0.542 | 50.1 ms |
| `SC_tressfx_rod_static` | 0 | 0 条 | 6 | **15.91** | 26.5 ms |
| `SC_tressfx_rod_sdf_static` | 0 | 0 条 | 1 | 0.146 | 10.0 ms |

收敛下限（1e-2）对 SDF 动画场景帮助也很明显：defect 从之前的 5.09 降到 **0.542**。

### 需要记下来的例外：静态场景有 7 个永久卡住的 guide

`SC_tressfx_rod_static` 的 15.91 全部来自 **7 个固定 guide**（src 914 / 5013 / 4282 /
5283 / 32997 / 39581 / 39620），它们在 138 帧里几乎每帧都失败（每帧 6–7 个），
defect 从帧 2 起就是同一个值不再变化——即状态冻结、反复失败、永不恢复。

失败码全是 3：其中一部分吃满 64 轮预算，另一部分（iters 7/12/14/20/26）是线搜索耗尽。
这与之前记录的「作者穿模过深」属于同一类：bind pose 下这些 guide 与胶囊已经深度重叠，
进入约束无法同时满足的状态。

按当前验收口径（发型整体保住、单根 guide 失败按 guide 隔离不冻结整个物体），
这是**已知债务**而不是回归：动画场景（真实使用形态）的 defect 在 0.3–0.5 量级，
只有静态 bind pose 场景有这 7 根。若要处理，方向是这类 guide 的初始化/重叠预修复
（`RodRkt10RepairOverlap` 目前对它们不起作用），而不是继续调求解器收敛条件。

### A 阶段结论

A 的两个子目标都已达成并复验：

- 最终模型 `cosseratImplicitV3` 在 GPU 上带胶囊与 SDF 接触跑通（四个 runtime 场景
  全部 exit 0、零 validation 消息）；
- runtime 资产已从 `cosseratXpbdV1` 切到 `cosseratImplicitV3`（资产分支已提交）。

遗留债务（不属于 A 的验收范围）：性能约 51 ms/2000 guide（V1 是 6.83 ms），
属于 C 阶段；静态场景 7 根卡住 guide；SDF 深层穿模盲区（与 V1 共享）。

### 静态场景那 7 根：结论是作者冲突，不是求解器缺陷

补查了重叠修复链路：`RodKkt10RepairOverlap` 在 `rodV2Kernel.glsl:927` **每个 substep
每个点都会调用**，逻辑是「最深重叠超过 `1% * v2LengthScale`（约 4 mm）就一直往外投影，
直到 gap 落回 `RodKkt10GapTolerance()`」，最多 16 次（一次投影就能把点放到 gap≈0，
所以深度不是限制）。

而这 7 根 guide 的 defect 从帧 2 起就是**同一个值**（15.9）不再变化，说明：
修复每个 substep 都把点推出去，求解阶段又被弹回来——**bind pose 本身就在胶囊内部**，
弹性静止形状与非穿透约束互相矛盾，这是**不可同时满足**的状态，换任何求解器都一样。

所以这不是收敛条件或求解器的问题，也不在 A 的范围内：要处理它需要资产侧决定
（bind pose 与碰撞体摆放的关系），或对这类 guide 单独做「初始状态投影」，
而不是继续调求解参数。按当前验收口径（发型整体保住、单根隔离）保持为已知债务。

## B 阶段入口盘点（先查清已有基础，不从头设计）

在动 B 之前先把仓库里已有的成绺/耦合基础查清了，结论是**不是从零开始**：

`tool/hair/` 下已有一套 CPU 原型，并且在构建图里（`CMakeLists.txt:104`
无条件 `add_subdirectory(tool/hair)`，四个可执行目标都在 `build/Makefile` 里）：

| 目标 | 源文件 | 内容 |
| --- | --- | --- |
| `vl_hair_clump_cpu` | `hairClumpPrototype.cpp` + `hairClumpControl.cpp`(48 KB) | C1 成对 clump 能量 + C0 startup/control |
| `vl_hair_clump_coupled` | `hairClumpCoupled.cpp`(68 KB) | C2 45 维联合能量与交叉装配 |
| `vl_hair_clump_long_run` | `hairClumpLongRun.cpp`(28 KB) | 长跑台 |
| `vl_hair_rod_ghost` | `rodGhostPrototype.cpp` | 冻结的单根求解器参照 |

C1 的能量形式是 world-oriented **vector-rest**：

```text
U = 0.5*k*sum_{a<b, j=1..3} |((x_a,j - x_b,j) - (x*_a,j - x*_b,j))|^2,  k = 0.1 N/m
```

文档记录的状态（`documents/rationale/hair-clump-control-20261119.md`、
`hair-clump-coupled-c2-20261119.md`）：

- C1：`execution-002` 本地自检 `checks=PASS failures=0`（v1 曾 5 项失败，v2 只修 oracle/诊断，
  没有调 k 或放宽阈值）；
- C0 startup：`execution-003` 完成 9 组共 840 步、执行侧门槛全满足，**独立验收 PENDING**；
- C2：45 维联合能量 fixtures 与 FD 装配已实现，**独立审计 PENDING**；
- **coupled 长跑、volume、clump 物理 PASS、runtime/shader/assets 修改：全部 NOT_RUN**，
  文档明确写着「等待主线程依据 C1 失败闭环重新授权」。

而 GPU runtime 侧目前**没有**任何 clump/volume 项：`rodKkt10BandSolve.glsl` /
`rodV2Kernel.glsl` 里没有成对能量，资产 schema 的 `simulationDefaults` 也只有
`enabled/backend/solver/solverProfile` 四个字段。

所以 B 的真实入口是：**0）把 C2 独立审计与 coupled 长跑补上 → 1）把已验证的成对能量
搬进 GPU KKT 系统（作为点间耦合项，会打破当前"点间只在同一条杆上耦合"的块三对角结构，
这是 B 的主要架构代价）→ 2）体积项 → 3）资产字段与验收。** 第 1 步之前需要确认
C2 的审计结论，否则会把未审计的能量形式搬上 GPU。

## SDF 接触为什么在正式资产上「不做事」：带宽是被 margin 卡住的

之前记录过「SDF 采样在跑但产生不了修正，26 个穿透自由点里 25 个比可用带更深」。
本轮把这条量化清楚了，并且找到了那个唯一有效的旋钮。

域布局（`source/hair/tressfxSdfGrid.cpp:83-99`）：

```text
resolution = clamp(round(maxExtent / cellSize), 48, 64)
side       = (maxExtent + 2*margin) / (1 - 4/resolution)
cellSize'  = side / resolution
truncation = margin + 2*cellSize'          // 这就是可用带
```

女性身体代理的 `maxExtent = 47.752 cm`（实测 OBJ），场景给的是 `cellSize: 1`，
于是 `resolution = 48` 被下限钳住。代入得：

| margin (cm) | cellSize' | truncation（可用带） |
| --- | --- | --- |
| **0.05（当前）** | 1.088 | **2.225 cm** |
| 2.0 | 1.176 | 4.352 cm |
| **4.0** | 1.267 | **6.534 cm** |
| 6.0 | 1.358 | 8.716 cm |

关键结论：**`cellSize` 不能加宽可用带**——它只决定 resolution，而 resolution 已经被
48 的下限钳住；`resolution` 越大反而 cellSize' 越小、带越窄（64³ 时只有 1.645 cm）。
**只有 `margin` 能加宽带**，代价只是格子略微变粗（1.09 → 1.27 cm，48³ 体素数不变）。

### 把 margin 提到 4 cm 的实测结果：能生效，但 2000 guide 撑不住

用派生场景（`Generated/Validation/SC_tressfx_rod_sdf_*_margin4.json`，只改 margin）
跑正式规模，日志确认参数确实生效：

```text
proxyTriangles=7391 resolution=48^3 cells=110592 bindCellSize=1.2671 margin=4
(domain is rebuilt per frame)
```

`bindCellSize=1.2671` 与上面手算的 1.267 一致 ✓。但两个场景都在**帧 1 就 device-lost**
（`-1073740791`）。

合理的解释是：带宽从 2.2 cm 扩到 6.5 cm 之后，原本被「看不见」而**完全不产生接触**的
深层点全部进入 SDF 可用带，SDF 侧活跃接触从几乎为空变成大面积存在，每个点的 3 个接触槽
很快被填满，KKT 行数和解算量随之上升；再乘以 2000 guide 与 SDF 每帧重建域，就超过了
提交上限。

所以「让 SDF 真正生效」在机制上是通的（就是调 margin），但在当前成本水平下不可用。
这不属于 A 的求解器缺陷，而是 C 阶段的成本问题：**margin 该取多少，要和性能预算一起定**
（例如 margin 2 cm 先跑通、或配合降低 SDF 重建频率/活跃槽上限）。

本轮未修改正式资产；派生场景只用于验证，margin 仍是 0.05。

## 第二十二轮：SDF 采样单位错了 100 倍——这才是「SDF 不做事」的真正原因

上一轮把「SDF 采样在跑但不产生修正」解释成「深层穿模落在可用带外，V1 与 V3 共享」。
**这个解释是错的。** 本轮定位到真正的缺陷并修掉。

### 缺陷

\RodKkt10SdfQuery\ 采样时写的是

\\glsl
vec3 local = (rod.worldToHair * vec4(position * 100.0, 1)).xyz;
\
\* 100.0\ 是 V1 路径的约定：V1 的 \positions\ 是**米制**，采样前 ×100 换厘米，
取回后再 \(distance - range.x) * 0.01\（odKernel.glsl:229/243\）。
但 KKT 路径的 \RodKkt10PointPosition\ 已经是**厘米制**——它的输出被直接和厘米制的
胶囊端点比较，而且那条路径残差稳定在 5.8e-05，说明单位本来就是对的。

于是 ×100 把采样点送到 100 倍远的地方，三线性查表几乎总落在域外，
SDF 接触**长期看起来完全不起作用**。这也解释了为什么把 margin 调大反而更容易出问题：
栅格扩了之后偶有错位采样落进域内，返回的是垃圾 gap。

### 修复与证据

16 guide、margin 提到 1 cm（让场真的有值）的对照组：

| | 修复前 | 修复后 |
| --- | --- | --- |
| maxBlockDefect | **95.68** | **0.00979** |
| 失败 guide | 1 | **0** |

正式规模（2000 guide、150 帧、零 validation 消息）：

| 场景 | 修复前 defect / 失败 | 修复后 defect / 失败 |
| --- | --- | --- |
| \SC_tressfx_rod_sdf_animated\ | 0.542 / 2 | **0.00828 / 0** |
| \SC_tressfx_rod_sdf_static\ | 0.146 / 1 | **0.00879 / 0** |
| \SC_tressfx_rod_animated\（仅胶囊） | 0.295 / 1 | **0.00976 / 0** |
| 16 guide 胶囊 | 0.00979 / 0 | 0.00979 / 0（不变） |

现在四个场景的 \gpuStatus\ 都是 0（除了静态场景那 7 根 bind-pose 冲突的 guide）。

### 顺带说明

- 可用带仍由 \margin\ 决定（当前 2.225 cm），「要不要加宽」是成本问题，回到 C 阶段；
- 但「SDF 接触是否真的生效」这个 A 阶段问题现在是**是**——带内的接触真实参与求解。


## 最终验收（SDF 单位缺陷修复后，A 阶段收尾）

当前 HEAD、`--shader-force-rebuild`、150 帧、validation layer 打开，四个正式场景：

| 场景 | exit | validation | 稳态 defect | 帧 149 失败 | 运行中曾失败的 guide 数 | hair pass |
| --- | --- | --- | --- | --- | --- | --- |
| `SC_tressfx_rod_animated` | 0 | 0 条 | 0.00991 | **0** | 7 | 88.3 ms |
| `SC_tressfx_rod_sdf_animated` | 0 | 0 条 | 0.00828 | **0** | **0** | 80.3 ms |
| `SC_tressfx_rod_static` | 0 | 0 条 | 15.91 | 2 | 3 | 25.7 ms |
| `SC_tressfx_rod_sdf_static` | 0 | 0 条 | 0.00879 | **0** | **0** | 2.8 ms |

两个 SDF 场景现在 defect 到 1e-02 视觉下限、零失败；胶囊动画场景同样零失败。
静态 bind pose 场景的 15.91 仍来自那几根「bind pose 就在胶囊内部」的 guide（成因见下节），
属于资产冲突而不是求解器问题。

成本读数波动较大（动画场景本次 88.3 ms、早前一次 51.0 ms，取决于难 guide 触发的
substep 数与债务），区间约 **51–88 ms**；量级结论不变：V1 是 6.83 ms，属 C 阶段。

### A 阶段交付清单

1. `cosseratImplicitV3` 在 GPU 上带胶囊与 SDF 接触跑通：四个正式场景 exit 0、零 validation 消息；
2. runtime 资产 `TF_female_rod_validation.json` 已从 `cosseratXpbdV1` 切到 `cosseratImplicitV3`（资产分支）；
3. 关键缺陷修复：压缩 Schur 代数的 `Q` 构造、6 宽 Thomas 右端符号、SDF 带边缘梯度、
   退化点正则化、信任域回溯起点、收敛视觉下限、**SDF 采样单位（100 倍）**；
4. 成本从「2000 guide 直接 TDR」降到 51–88 ms 可跑。

### 已知债务（均不属于 A）

- 性能 51–88 ms/2000 guide vs V1 的 6.83 ms（C 阶段）；
- 静态 bind pose 场景那几根卡住 guide：bind pose 与胶囊冲突，需资产侧决策；
- SDF 可用带 2.225 cm（margin 0.05），加宽带是成本问题（C 阶段）。

## C 阶段起点：成本基线，以及一个必须先解决的测量口径问题

### 基线（当前 HEAD，胶囊场景，只改 simulationGuideCount，follow 数不变）

| 模拟 guide 数 | hairSimulation | 帧状态 |
| --- | --- | --- |
| 16 | 2.3 ms | 正常 |
| 800 | 23.6 ms | exit 0，defect 15.9（那 7 根 bind-pose guide） |
| 1600 | 30.5 ms | exit 0，同上 |
| **2000** | **61.3 ms** | exit 0、`gpuStatus=0`、0 失败、defect 0.00999 |

V1 同规模是 **6.83 ms**，所以 C 的目标量级是 **约 9x**。

两条可直接用的结论：

1. **follow 不是瓶颈**：16 guide 场景的 follow 数同样是 38,334，整帧只要 2.3 ms，
   所以 follow 开销 ≤ 2.3 ms；
2. 2000 guide 下每 guide 每帧约 **29 µs**（(61.3 − 2.3)/2000）。

### 但 800 → 1600 只涨 23.6 → 30.5 ms，这不是优化成果

这种次线性来自**自适应 substep 上限**：guide 越多，越多帧被判为「硬帧」，
`RodGpuV3HardFrameSteps=1` 生效，于是每帧实际 substep 变少、成本变低，
代价记在 `debt` 上——**仿真在慢放**，不是变快了。

所以任何「成本随 guide 数缩放」的测量，都必须先固定 substep 数，否则读数没有意义。

### 固定 substep 不是改一个常量

`RodGpuMaxSteps` 已经烘进 UBO 布局：`RodGpuParams::capsules` 是
`array<array<GuideColliderGpu,8>, RodGpuMaxSteps+1>`，配套还有
`static_assert(offsetof(RodGpuParams, counts) == 768)` /
`offsetof(capsules) == 800` 与 `sizeof == 3136`，shader 侧的
`rod.capsules[RodGpuMaxSteps+1]` 也要同步。把它从 8 改成 4 会直接触发那两个
static_assert。

结论：C 要做成本对比，得**新增一个明确的固定 substep 诊断模式**（或把 substep 数
做成可查询/可覆盖的调试参数），而不是改常量。这本身是 C 的第一件正事。

### 固定 substep 诊断开关与分解结果

新增环境变量 `VL_ROD_FIXED_SUBSTEPS=<n>`（`source/hair/rodGpuRecording.h`，仅在设置时生效），
把每帧 substep 数强制成 n（上限 `RodGpuMaxSteps`），绕开自适应上限与债务的耦合。

**2000 guide（正式动画场景）：**

| substeps | hairSimulation |
| --- | --- |
| 1 | **35.0 ms** |
| 4 | **103.6 ms** |

→ 每 substep ≈ **22.9 ms**，固定成分 ≈ 12.1 ms。
按 60 fps、4 substep（4 × 1/240 = 1/60 s，恰好实时）算，需要整帧 ≤16 ms，
即每 substep ≤ **4 ms**——**目标是 5.7x**。

**不同规模（固定 1 substep）：**

| guides | hairSimulation | 每 guide |
| --- | --- | --- |
| 16 | 1.3 ms | 0.081 ms |
| 200 | 19.4 / 21.1 / 21.3 ms（三次复现） | 0.097 ms |
| 2000 | 35.0 ms | **0.0175 ms** |

**这里有个还没解释的现象**：16 → 200 是线性的（每 guide 0.08–0.10 ms），
但 2000 的每 guide 成本只有 200 的 **1/5.5**。如果按 200 的斜率外推，
2000 guide 应该是 ~194 ms，实测只有 35 ms。

在解释清楚这个尺度依赖之前，**任何「优化到多少 ms」的结论都不可靠**——
它可能是并行度/占用率随 workgroup 数变化，也可能是计时口径（fence 等待、
与其他 pass 重叠）在这个规模上不同。C 的下一件事就是这个，而不是先动求解器。

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
