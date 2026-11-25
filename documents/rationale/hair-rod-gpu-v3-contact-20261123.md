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

### 尺度反常已解释：不是「每 guide 成本」，是「每次迭代成本 × 最大迭代数」

先纠正我自己的两个错误读数：

1. 之前把 `received/dispatched` 当性能指标——它们其实是**仿真时间**。
   `dispatched/frame = 4.2 ms` 在任何规模下都恒定，因为那就是 1 substep × 1/240 s
   = 4.167 ms。唯一真实计时是 perf jsonl 的 `hairSimulation`。
2. 所以上一节「200 guide 每 guide 成本 0.097 ms」的换算毫无意义。

用 `hair snapshot` 读每个 guide 的实际迭代数（固定 1 substep，70 帧）：

| guides | hairSimulation | maxIters | meanIters |
| --- | --- | --- | --- |
| 16 | 1.2 ms | **1** | 0.2 |
| 200 | 19.2 ms | **64** | 0.6 |
| 2000 | 33.5 ms | **3** | 1.1 |

结论很清楚：**帧时间由「最慢的那批 workgroup 的迭代数」决定**。
200 guide 那 19.2 ms 完全来自**单个 guide 跑满 64 轮**；16 guide 只迭代 1 次所以 1.2 ms；
2000 guide 平均 1.1 次、最大 3 次，33.5 ms 是「2000 个 workgroup × 约 1.1 次迭代」。

于是单位成本可以定出来：2000 guide、1 substep、约 1.1 次迭代 = 33.5 ms
（其中固定成分约 12 ms），即 **每 guide 每次迭代约 9.5 µs**。

**60 fps 的预算**：4 substep × 约 1.1 次迭代 × 2000 guide = 8,800 次 guide-iteration
要压进 16 ms → **每次 guide-iteration 需 ~1.8 µs，即 5.3x**。

### 这 5.3x 大概率在占用率，不在指令数

一次 guide-iteration 是一个 warp 跑完 32 点：压缩约 1,300 条指令 + CR 约 2,900 条
≈ 4,200 条 warp 指令。1.8 µs @1.8 GHz = 3,240 cycle，也就是「接近 1 IPC」就够——
**指令数本身已经接近这个目标**。实测 9.5 µs ≈ 17,000 cycle，是理论值的 5 倍，
差在延迟/占用率而不是算法量。

而占用率被 shared 钉死在 **1 workgroup/SM**：`rodCr` 四个矩阵数组
（32 × 144 B = 18,432）+ 两个向量数组（32 × 24 B = 1,536）≈ **20 KB**。
shared 要 ≤24 KB 才有 2 个 workgroup、≤12 KB 才有 4 个。

### C 的方向：把 CR 从 shared 换成 subgroup shuffle

`local_size_x = 32` 正好是一个 subgroup，块循环约化天然适合
`subgroupShuffle` / `subgroupShuffleXor`。换成 shuffle 版 CR 可以把那 ~20 KB shared
全部让出来，占用率 1 → 3~4，正好覆盖 5.3x 的目标量级。

**前提已验证通过**：

- 仓库本来就以 `shaderc_env_version_vulkan_1_4` 编译着色器
  （`source/shaderCompiler.cpp:439-441`，缓存键里的 `target=Vulkan1.4` 就是它），
  所以**不需要改动编译目标**，只要在着色器里加
  `#extension GL_KHR_shader_subgroup_shuffle : require`；
- 实测：`subgroupShuffle` + `subgroupShuffleXor` 在 `--target-env=vulkan1.3` 下编译通过
  （856 字节 SPIR-V）；**默认的 vulkan1.0 目标会报 `requires SPIR-V 1.3`**——
  这解释了为什么必须走仓库自己的编译路径，而不能用默认 glslc；
- 设备是 `VK_API_VERSION_1_3`，`local_size_x = 32` 与 NVIDIA 的 subgroupSize=32 一致，
  shuffle 语义可直接对应到当前「每个 lane 一个点」的布局。

所以 C 的下一个实现步骤很明确：写一个不依赖 shared 的 shuffle 版块循环约化，
替换 `rodCrBand.glsl` 里的 `RodCrSolveBand32`，然后实测 shared 占用与
每 guide-iteration 成本。若 5.3x 里能拿到 3x 以上，2000 guide 就落在 60 fps 附近。

### 测量口径确认：`hairSimulation` 就是墙钟，且瓶颈是单 warp 延迟

先排除一个怀疑：`--frame-limit 60` 的节流等待是否被算进 `hairSimulation`。
用 `--frame-limit 1000`（不节流）复测，读数几乎不变：

| guides | 节流 60 | 不节流 1000 |
| --- | --- | --- |
| 200 | 19.2 ms | 19.2 ms |
| 800 | 22.0 ms | 22.1 ms |
| 2000 | 33.5 ms | 36.3 ms |

所以那是真实墙钟，不是被 pacing 稀释的数字。

**关键一致性检查**：200 guide 场景 meanIters 只有 0.6，但 maxIters = 64，
整帧 19.2 ms 完全由那**一个** guide 决定（其余几乎不迭代）。也就是

```text
64 次迭代 ≈ 19 ms  →  单 warp 一次迭代 ≈ 300 µs ≈ 540,000 cycle
```

而一次迭代的理论指令量只有约 4,200 条（压缩 1,300 + CR 2,900）。
540,000 / 4,200 ≈ **128 cycle/指令**——这正是「**每个 SM 只驻留一个 warp、
完全无法隐藏访存延迟**」的典型特征（一次依赖访存就是数百 cycle）。

所以占用率假设得到实测支持：**不是指令多，是只有一个 warp 在跑、每次访存都裸露延迟**。
shared 被 `rodCr`（约 20 KB）钉在 1 workgroup/SM 就是根因，把它让出来
（1 → 3~4 warp/SM）正好是 3~4x 的延迟隐藏收益。

补充：静态累加 shared 声明没有意义——声明里还有 `rodV2BandStorage[1728]`（27,648 B）
这类 KKT 路径根本不用的数组，合计远超 48 KB 却仍能创建管线，说明驱动做了死代码消除，
真实分配只能靠实测（本次用「单 warp 迭代成本」反推）。

### 尝试把压缩代数也改成块形式：公式对、GPU 结果不对，已回退

`RodKkt10Condense` 里 S 的 3x3 与 Q 的 6x6 都是 `for (row<3) for (column<3)` 里
用 `RodV2Entry/Set` 逐元素写的（9 + 81 次动态索引），按同一类问题（local memory）
本该是下一个大收益点。于是改用 mat3 块：

```text
S[k][l] = g_k · a_l      →  s = transpose(gLinear) * aLinear
Q = [[I - G S^-1 T_lin^T,  -G S^-1 T_ang^T], [0, I]]
extra = G S^-1 h, h = -gapRow
```

**公式先在 Python 里逐项对过**（A 随机、g_k 角向为 0）：

| 量 | 块形式 vs 逐元素 |
| --- | --- |
| Q | **0.0**（完全一致） |
| extra | 1.1e-12 |
| S | 1.4e-17 |

但搬到 GLSL 之后结果明显错：16 guide 的 defect 从 0.00979 变成 **3.42**，
2000 guide 261 ms、全部 guide 失败。已 `git checkout` 回退，当前状态仍是
16 guide defect 0.00979、0 失败。

结论：公式没错，错在**代码里的块/元素约定**（`RodV2Matrix` 的 4 个 mat3 块顺序、
`mat3(a,b,c)` 是列还是行、`transpose` 与 `RodV2Entry` 的行列对应）——这类问题 Python
验不出来，只能靠 GPU 上逐步二分。下一轮的正确做法是**分别**替换 S、Q、extra 三段，
每段单独验证 defect 保持 0.00979，而不是一起换。

本轮净收益为 0（失败并回退），但也确认了一件事：这条路线本身仍有价值，
只是必须逐段验证。

### 逐段二分成功：但收益只有 3%，因为常量循环本来就会被展开

上一轮三段一起换导致 GPU 结果错，本轮按三分段重做，**每段单独验证**：

| 步骤 | 16 guide defect | 结论 |
| --- | --- | --- |
| 基线 | 0.009791187942 | — |
| 改 `extra`（三次加权和） | **0.009791187942** | 完全一致 |
| 再改 `S`（9 个常量下标赋值） | **0.009791187942** | 完全一致 |
| 再改 `Q`（9 个 outerProduct 项） | **0.009791187942** | 完全一致 |

所以上一轮的错误就在 Q/S 的块形式本身（`transpose` 与块顺序的约定），二分法一次就把它隔离出来了。

**但性能只有 27.8 → 26.9 / 27.1 ms（约 3%，在噪声内）。**

原因清楚了：`for (i<6) for (j<6)` 这类循环**次数是常量**，编译器本来就会完全展开，
下标因此变成常量，根本不进 local memory。真正触发 local memory 的是
`RodV2Inverse` 里那个**数据相关**的索引 `augmented[selected * 12u + pivot]`——
那才是 1.26x 的来源。

**结论：动态索引已经不是剩余成本（26.9 ms vs 目标 4 ms）的解释了。**

### 下一个假设：一个 workgroup 只有一个 warp，而它要占整块 shared

`local_size_x = 32`，所以**一个 workgroup 就是一个 warp**。而每个 guide 的求解状态
（`positions/velocities/orientations/...` 约 30 个 32 元素数组，粗算 10–15 KB）
是 per-workgroup 的 shared。这意味着每个 SM 只能驻留 1–2 个 workgroup，
也就是 **1–2 个 warp / SM，而一个 SM 本可容纳几十个**——占用率在个位数百分比。

这也解释了为什么之前「让出 rodCr 的 20 KB」没有效果：剩下的 per-guide 状态
自己就还有十几 KB，仍然只够 1–2 个 workgroup。

下一轮的正路是**同一套变换再做一遍**：把 per-point 状态（positions/velocities/
orientations 等）从 shared 挪到**寄存器**，邻点访问统统走 `subgroupShuffle`——
这正是 CR 已经验证可行的模式。shared 降到接近 0 之后，占用率才可能从个位数
升到几十个百分点，那才是 7x 的量级来源。

验证方式：先做一个「加 dummy shared 观察性能悬崖」的标定，确认当前确实卡在
1–2 workgroup/SM，再动手搬状态。

### 占用率假设被自己的标定否掉（在投入大改之前）

上一节提出「一个 workgroup 只有一个 warp、每 guide 十几 KB shared → 1–2 warp/SM」，
并打算把 per-point 状态搬进寄存器。动手前先做了标定：加了
`VL_ROD_SHARED_PAD_BYTES` 这个可配置的 dummy shared 数组（宏未定义时零影响，
用「运行期永假、编译期不可知」的条件引用它以免被 DCE），逐步加大并测同一场景。

| 额外 shared | hair（2000 guide，固定 1 substep） |
| --- | --- |
| 0 | 27.4 ms |
| 8 KB | 28.3 ms |
| 16 KB | 30.8 ms |
| 24 KB | 28.2 ms |

**几乎完全平**。如果原本是 2 个 workgroup/SM，加 24 KB 会让它掉到 1 个、
性能应该掉一大截；没有掉，说明**当前就已经是 1 个 workgroup/SM，而且它不是瓶颈**
（否则加 shared 也不该完全没反应——或者说，占用率被别的因素限制，不是 shared）。

所以「把 per-point 状态搬进寄存器」这个大改造**被否定掉了**，省下了一轮高风险重写。
这正是先做标定的价值：前两次（占用率、寄存器溢出）都是在假设上直接改代码，
各花了一轮才发现无效。

### 重新估成本：现在看是指令量，而不是占用率或延迟

按当前实现粗算每次 guide-iteration 的 warp 指令量：
压缩（两次 `RodV2Inverse` 各约 500 条 + 3 次 6x6 乘约 650 条 + Q 的 9 项外积 + 残差）
约 2,000 条；CR 每层 4 次 6x6 乘 + 邻居 shuffle（约 800–900 条）× 5 层约 4,000 条。
合计约 **6,000–10,000 条 warp 指令**，而实测每次约 540,000 cycle，
即 **约 60–90 cycle/指令**。

即便把延迟完全藏掉（1 IPC），也只值 2–3x，离 7x 仍有距离——所以**7x 必须来自
减少指令数**，而不是继续找延迟。可动的方向：

- CR 的每层 4 次 6x6 乘（共 20 次）是最大的一块，可以考虑把「系数 = L·inv」
  与后续乘积合并，或改成 6x3 的窄右端形式减少一半工作量；
- 压缩里的 `RodV2Inverse(A)` 每点每迭代一次（约 500 条），可考虑复用上一次迭代的
  A⁻¹ 作为预热（A 变化不大），代价是精度；
- 残差/装配本身（每点读 10 分量残差 + 接触几何）也要算进来。

### 实测：CR 只占 12%，不是瓶颈（含一次假阴性）

用「同一段工作做两遍、丢掉第一遍结果」来量某一段的占比。第一个被测的是 CR：

| 变体 | hair（2000 guide，固定 1 substep） |
| --- | --- |
| 单次 CR | 27.9 ms |
| CR 跑两遍（结果未被引用） | 27.9 ms ← **假阴性** |
| CR 跑两遍（结果被不透明条件引用） | 30.8 ms |
| 同一构建的单次基线（对照上面一行） | 27.5 ms |

第一次的「没有变化」是**编译器把没被引用的第二次求解整个消掉了**——这就是我前面
在探针上栽过的同一个坑。加了「运行期永假、编译期不可知」的引用之后才拿到真值：

**一次 CR ≈ 3.3 ms，占 27.5 ms 的约 12%。**

所以 CR 不是瓶颈，和之前按指令量的估计相反。剩下的约 24 ms / substep
（2000 guide，约 12 µs 每 guide-substep）在别处：

- 每 substep 把 per-point 状态装进 shared（几十个数组）；
- predictor；
- 每迭代的 `RodV2Tangent` 三段切向装配；
- 残差与接触几何；
- 结果写回。

下一轮用同一个「做两遍 + 不透明引用」的判别法逐项量这几段，找出那 24 ms 落在哪。
注意这条方法本身要**先在已知为零的地方做阴性对照**（例如重复一个无副作用的空段），
否则无法分辨「真的免费」和「被消除了」。

### 定位到了：27 ms 里几乎全部是接触几何求值

用四次独立的切分把 2000 guide、单 substep 的成本拆开，结论推翻了前几轮优化的方向：

| 切分方式 | 结果 | 占比 |
| --- | --- | --- |
| 双跑 CR（带不透明引用） | 30.8 vs 27.5 ms → **3.3 ms** | 12% |
| 双跑整段 `RodKkt10Condense`（含切向装配 + 接触几何 + 残差 + 压缩代数） | 29.2 vs 27.5 ms → **1.7 ms** | 6% |
| 可覆盖迭代预算 0 / 1 / 2 | 26.0 / 26.5 / 26.3 ms → **几乎不变** | 迭代≈免费 |
| **去掉胶囊与 SDF 碰撞体**（同 2000 guide、同 1 substep） | **27.2 → 0.8 ms** | **≈96%** |

也就是说：**求解器、切向装配、写回合计只有 0.8 ms**；27 ms 几乎全部花在
**接触几何求值**上——`RodKkt10CapsuleDistance`（最多 8 个碰撞体）加
`RodKkt10SdfQuery`（7 次三线性采样），而且每个点每个 substep 被**重复求值多次**：

- `RodKkt10UpdateActiveSet`（每迭代一次）；
- `RodKkt10BuildContacts`（压缩、残差、measure 各自都会调）；
- `RodKkt10DeepestOverlap` / `RodKkt10RepairOverlap`（每 substep）。

这就把 C 阶段的结论彻底改了：**4 ms 目标不再取决于求解器，而取决于接触几何**。
只要把「每个点每个位置求一次、然后复用」做到，并把 SDF 查询压下来，
0.8 ms 的底子已经在目标之内。

另外记一笔：无碰撞体的派生场景 defect 是 0.00879，仍然是一个合法的计时夹具。

### 方法教训（本轮第二次踩到）

双跑判别法的第一次读数（27.9 vs 29.0）是**假阴性**——没被引用的第二次求解被编译器
整个消掉了。之后所有这类探针都必须带「运行期永假、编译期不可知」的引用，
并且先用已知为空的段做阴性对照。

### 撤回：之前的阶段占比都是噪声，稳态是 17.22 ms

上一节那些「CR 12%、压缩 6%、修复 3.1 ms」的读数，用的都是 **`--exit-after-frames 60`
+ 跨帧取最大值**，那取到的是**瞬态峰值**，再加上约 15% 的跑间波动，才凑出了那些数字。

我写了一个稳定测量脚本（`artifacts/tmp/bench.py`：3 次重复、200 帧、丢掉预热的前 20%、
取中位数、固定 1 substep、不节流），跑间极差约 1%。用它重测：

| 变体 | 稳态 hair |
| --- | --- |
| 基线 | **17.22 ms** |
| CR 双跑 | 17.32 ms |
| 压缩双跑 | 17.18 ms |
| measure 双跑 | 17.26 ms |
| 跳过重叠修复 | 17.51 ms |
| 激活集双跑 | 17.23 ms |
| 迭代预算 0 / 1 / 4 / 64 | 17.27 / 17.25 / 17.33 / 17.39 ms |

**全部落在噪声内。** 也就是说：CR、压缩代数、measure、重叠修复、激活集、以及整个牛顿
迭代循环，**在稳态里都低于噪声底**。上一节那些百分比是在瞬态峰值上量的，**撤回**。

### 现在确定的边界

| 场景（2000 guide，1 substep，稳态中位数） | hair |
| --- | --- |
| 无碰撞体 | **0.69 ms** |
| 1 胶囊 | 3.3 ms |
| 2 胶囊 | **2.92 ms** |
| 4 胶囊 | **17.22 ms** |

- 成本是**每 guide** 的（16 guide 同 4 胶囊只有 1.2 ms，2000 guide 是 17.22 ms）；
- 成本随**碰撞体数**是**台阶**而不是斜率（2 → 4 跳 5.9 倍）；
- 不是 CPU 侧串行（`--worker-thread-count 2` 是 17.39 vs 1 的 17.02）；
- `--gpu-stats` 只打印 UI 重建计数，没有 GPU 分段时间，所以 `hairSimulation` 是
  CPU 括号（含 fence 等待），无法直接区分「GPU 执行」与「队列延迟」。

### 剩下的问题（下一步）

「每 guide、接触相关、但所有可双跑的 shader 阶段都免费」这三件事要同时成立，
只剩两种可能：

1. **长尾**：少数 guide 进入病态循环（`RodKkt10RepairOverlap` 最多 16 轮、
   线搜索失败重试），整次 dispatch 等最慢的那个 workgroup，于是加「平均工作量」
   的探针看不出变化；
2. 成本在**尚未探测的段落**：substep 的 prologue/epilogue（约 30 个 per-point
   数组的装载与写回）、`RodKkt10UpdateAxial`，或 follow kernel。

区分办法：先用 `RodKkt10RepairOverlap` 的**触发次数**与单 guide 迭代分布确认长尾是否存在
（如果只有极少数 guide 差很多，就是长尾）；否则按「prologue 双跑」和「UpdateAxial
双跑」继续切。注意长尾情形下，优化平均路径没有意义，必须削掉病态分支。

### GPU 时间戳：14.38 ms 是真实 GPU 执行，而且这个 kernel 是延迟受限

用仓库现成的 `GpuPassTimer`（`renderFrameRecorder.cpp` 里已经有
`BeginPass(commandBuffer, "hairSimulation")`，控制台命令 `gpustats`）直接读 GPU 分段，
不再依赖 CPU 括号：

| 场景（2000 guide，固定 1 substep） | GPU hairSimulation | CPU 括号 |
| --- | --- | --- |
| 有碰撞体 | **14.38 ms** | 17.22 ms |
| 无碰撞体 | **0.63 ms** | 0.69 ms |

所以 17.22 ms 里约 **14.4 ms 是真实 GPU 执行**，其余约 2.8 ms 是 CPU/队列开销。
而无碰撞体时同样是 2000 个 workgroup、同样的状态装载与写回，只要 **0.63 ms**——
**差别完全在接触相关的代码路径上**。

把它和「所有可双跑的 kernel 阶段都免费」放在一起，这个 kernel 的性质就确定了：
**它是延迟受限（latency-bound），不是吞吐受限**。加 ALU 工作量会被访存停顿的空档吃掉，
所以双跑任何阶段都测不出差别；而接触路径一旦开启，多出来的 shared/global 访问
（接触槽、残差缓冲、接触数组）在**每个 SM 只驻留极少 warp** 时全部裸露延迟。

这也调和了前面两个否证实验：加 24 KB shared 不变（shared 不是把占用率压到 1 的原因）、
删 36 个 float 不变（寄存器压力也不是主因）——但**占用率低**这个事实仍成立，
只是限制它的既不是 shared 也不是那 36 个 float。

### 下一步：提高并发，而不是继续减指令

1. **改 workgroup 布局**：把 `local_size_x` 从 32 提到 128，让一个 workgroup 含 4 个 warp、
   每个 warp 处理一个 guide（per-guide 共享状态变成 4 份并存）。若 GPU 能同时驻留多个
   workgroup，这就直接给出 4x 的延迟隐藏；若 shared 装不下 4 份，退到 64（2 个 warp）。
2. **把接触路径的 shared 访问搬进寄存器**（`subgroupShuffle` 已验证可行），
   缩短必须串行等待的依赖链。

两者都是结构性改动（`gl_WorkGroupID`/`gl_LocalInvocationID` 到 guide 的映射、
`barrier()` 范围、per-guide 共享数组索引），必须先锁住 16 guide 的 defect 0.00979
再谈性能。

### 把「接触」再拆开：SDF 查询占 14.4 ms 里的约 10 ms

上一节「有碰撞体 14.38 ms / 无碰撞体 0.63 ms」把**胶囊和 SDF 混在一起**了。
派生场景把两者分开后（2000 guide、1 substep、GPU 时间戳）：

| 场景 | GPU hairSimulation |
| --- | --- |
| 4 胶囊 + SDF 网格 | **14.38 ms** |
| 只有 4 胶囊（删掉 `collisionMesh`） | **3.52 / 4.41 ms**（两次运行） |
| 只有 SDF 网格（删掉 `capsules`） | 0.63 ms，但 `sdfObjects=0` —— **SDF 没被启用** |

顺带记一个引擎现状：只声明 `hairCollision.collisionMesh` 而不带 `capsules` 时，
SDF 根本不会被建起来（`sdfObjects=0`），所以「SDF 单独成本」无法用这种方式隔离。
但对照已经足够：**胶囊约 3.5–4.4 ms，SDF 路径约 10 ms**。

### 为什么 SDF 查询这么贵

`SdfTrilinear`（`tressfxSdfCommon.glsl`）是**手写三线性**：每次采样要读 8 个
`SdfCellIndex` 单元、做 `SdfInitialBits` 校验、再解 `SdfIFloatFlip`。
`RodKkt10SdfQuery` 用 1 次中心采样 + 6 次 ±0.5·cell 偏移做中心差分求梯度，
一次查询就是 **7 × 8 = 56 次全局缓冲读取**（中心采样越界时会短路，所以远处点只花 1 次）。

在「每个 SM 只驻留极少 warp」的前提下，这 56 次读取的延迟基本裸露，
这与「加 ALU 免费、加访存变慢」的实测性质一致。

### 下一步（按收益/风险排序，都不需要动 workgroup 布局）

1. **把 SDF 网格改成 `sampler3D` + 硬件三线性**：一次采样 1 个纹理取指，
   7 次采样取代 56 次缓冲读取，理论上是 8x 的访存削减。代价是要新增 3D 图像
   （R32F、48³ ≈ 442 KB）、采样器与描述符，并且 splat 要么直接写图像、
   要么每帧从缓冲上传。
2. **把 7 次采样减到 4 次**（前向差分：中心 + 三个轴向），立省 43%，
   但梯度精度从二阶降为一阶，必须重新验 defect 与失败数。
3. 中心采样加**带宽早退**：目前 `distanceValue >= SdfOutOfRangeDistance` 只跳过偏移采样，
   距离表面较远但仍在带内的点依然付 56 次读取；可加「距表面 > k·cellSize 直接返回」
   的阈值。

这三条都先锁 16 guide defect 0.00979 和四场景 0 失败，再谈性能。SDF 单位那条
历史坑（`position * 100.0`）已经修过，改这段必须重新确认量纲不变。

### 已实测：带宽早退没有收益（第 3 条被否掉）

先做了最便宜的第 3 条：中心采样后若 `(distanceValue - sdf.range.x) * 0.01 > 0.5`（厘米）
就直接拒绝、不再做 6 次偏移采样。GPU 时间戳结果 **15.11 ms，对比基线 14.38 ms**——
没有收益（在噪声内偏慢），已回退。

原因很直接：头发本来就贴在头皮上，**绝大多数被查询的点都在表面 0.5 cm 以内**，
所以这个早退几乎不跳过任何东西。这条否掉之后，剩下的只有「减少每次查询的读取次数」：

- 第 1 条（`sampler3D` + 硬件三线性，56 次缓冲读取 → 7 次纹理取指）是唯一有量级收益的路；
- 第 2 条（7 次采样减到 4 次）约 1.75x，代价是一阶梯度，需要重新验 numerics。

第 1 条要动引擎：新增 R32F 3D 图像（48³ ≈ 442 KB）、采样器、描述符，
并决定 splat 直接写图像还是每帧从缓冲上传。这是一整轮的活，且必须重跑
SDF 单位核对（历史上这里错过一次 100 倍）。

### 把 SDF 链逐环摘掉：四环全部无罪，10.7 ms 仍无归属

上一节把「SDF ≈10 ms」归到查询上，并打算改成 `sampler3D`。**先做了一个更便宜的判决**：
把 SDF 查询整个短路（`VL_ROD_SDF_SKIP_QUERY`，直接 `return false`）。

| 变体（2000 guide、1 substep、GPU 时间戳） | GPU hair |
| --- | --- |
| 基线（中心差分 7 次采样） | 15.13 ms |
| **SDF 查询整个跳过** | **15.12 ms** |
| 前向差分 4 次采样（少 43% 读取） | 15.32 ms，defect 从 0.00851 变差到 0.00994 |
| 跳过 `SdfSplat` | 15.92 ms（对照 15.95） |
| 跳过 `SdfCollide` | 15.29 ms（对照 15.21） |
| 跳过 `SdfClear` | 15.77 ms（对照 15.53） |

也就是说：**查询、splat、collide、clear 逐个摘掉都没有任何变化**，但「声明了
`collisionMesh`」这件事本身仍然值约 10.7 ms（caps_only 3.5–4.4 ms vs 15.1 ms）。
前向差分那条已回退（没提速、defect 还变差），也说明**梯度采样不是成本来源，
原本要做的 `sampler3D` 改造会是无用功**——这是本轮最有价值的一次「先量再做」。

四环都无罪、总量却存在，说明成本落在「SDF 存在才会发生、但不在这些 dispatch 里」的地方：

- 有 SDF 时 guide 求解走的是**另一条 pipeline**（`rodV3ImplicitSdf.comp`，带 `VL_ROD_SDF`），
  它比 `rodV3ImplicitCrCompensated.comp` 慢的原因可能不是查询，而是别的东西
  （更宽的描述符集、额外的屏障、`rodKktDebug` 写入等）；
- 或者 `hairSimulationPassTrace.cpp` 里那段「所有 dispatch 写完后、transfer 前」的
  **copy + 两侧显式屏障**在 SDF 路径上多走了一次；
- 或者代理网格每帧的**顶点/索引流上传**在 SDF 路径上才有。

### 下一步：先给每个 dispatch 加 GPU 分段，别再猜

`GpuPassTimer` 目前只把整条 hair 链当一个 `hairSimulation` 段。下一步是给
`SdfClear` / `SdfSplat` / `SdfCollide` / guide solve / follow 各自加 `BeginPass`
（或在 `hairSimulationPassTrace` 里按 dispatch 计数），让 GPU 表直接给出每一环的毫秒，
再决定改哪里。**这是纯诊断、无行为风险**，且能一次性结束这轮反复出现的
「摘掉一环却毫无变化」的僵局。

### 细粒度 GPU 分段：14 ms 根本不是 GPU 在算，是 GPU 在空等

上一节的疑惑（四个 SDF 环节逐个摘掉都无变化）用细粒度分段一刀解开了。
做法：给每个 tressfx pass 和 rod 求解各自开一个 `GpuPassTimer` 分段
（`HairSimulationPass::SetGpuStageTimer` + `RecordPass` 内 `BeginPass/EndPass`），
并用 `VL_ROD_GPU_STAGE_TIMING=1` 让上层**不再**包一个整链分段。
未设置时行为与之前完全一致。

结果（2000 guide、固定 1 substep、SDF 场景）：

```text
[GPU] frame avg over 120: hair.rodSolve 0.00 ms | hair.sdfClear 0.00 ms |
      hair.sdfSplat 0.20 ms | skinnedDeform 1.24 ms | ... | total 1.73 ms
```

**`hair.rodSolve` 是 0.00 ms，整条 hair 链约 0.2 ms，而整帧 GPU 总量从
之前的 15.91 ms 掉到 1.73 ms。** 同一份场景、同一个构建，只是去掉了
那个外层分段——如果 GPU 真在计算 14 ms，去掉分段不可能让它消失。

所以那 14 ms 是**计时区间内 GPU 空等**：起始时间戳写入后，GPU 一直在等 CPU
把剩下的命令录完。换句话说：**hair pass 是 CPU 录制嵌颈，不是 GPU 嵌颈**。

这一个结论一次性解释了前面所有反常现象：

- 双跑任何 shader 阶段都无变化（GPU 本来就没在跑）；
- 摘掉 SDF 的 clear/splat/collide/查询都无变化（它们合起来只有 0.2 ms）；
- 声明 `collisionMesh` 却多出 ~10 ms（SDF 路径多了 CPU 侧的参数写入/上传，
  多出的是**空等时间**）；
- `--worker-thread-count 2` 不变（录制仍在 GT）；
- 成本随 guide 数增长（CPU 侧每帧要处理的对象/参数随规模变多）。

因此 C 阶段的优化目标从「GPU kernel」彻底改为「**CPU 录制路径**」。
下一步就是用 Tracy/NVTX 分段或直接在 `HairSimulationPass::Record` 里手工计时，
定位到底是描述符写入、参数 UBO 上传、还是逐对象循环里的哪一步。

### 结论推翻：那些 17 / 27 ms 不是 hair 的成本

把 CPU 侧和 GPU 侧都补齐分段后，三个口径对不上：

| 口径（2000 guide、固定 1 substep、SDF 场景） | 数值 |
| --- | --- |
| GPU：hair 链各分段之和（snapshot 0.00 + skinning 0.00 + rodSolve 0.00 + sdfClear 0.01 + sdfSplat 0.20） | **0.21 ms** |
| GPU：整帧所有 pass 总和（avg over 120） | **1.71 ms** |
| CPU：`HairSimulationPass::Record` 全函数 | **2.35 ms**（其中 rod 录制 2.5 ms/frame） |
| perf jsonl 里的 `hairSimulation` | **15.29 ms**（中位数） |

`GpuPassTimer` 的 Begin/End 都用 `eBottomOfPipe`（`gpuPassTimer.cpp` 里有注释）。
实测结果是：**同一个构建、同一个场景，分段之和 0.21 ms，而整条链一个分段时
读数是 15.29 ms**。这个差异既不能用 GPU 工作解释（分段证明它只有 0.2 ms），
也不能用 CPU 录制解释（全函数只有 2.35 ms）。

结论：**perf jsonl 里的 `hairSimulation` 不是 hair 的成本**，它是一个包含
空等的时间戳区间。这意味着从 C 阶段开始到现在，所有「27 ms / 17 ms」
的读数都不是可比的成本量，包括那些基于它做出的「瓶颈在 X」结论。

已经可以确定的是：**hair pass 的真实 GPU 工作是 0.2 ms 量级，CPU 录制是
2.4 ms 量级（其中 rod 录制占大头）**。这与「真实时」目标已经同阶，所以 C 阶段
的性能结论必须**重新基准**。

### 下一步：重新建立可信的 C 基准

1. 不再用 `hairSimulation` 这个名字做判据；改用：fps / 帧时间、
   `VL_ROD_GPU_STAGE_TIMING` 下的 GPU 总和、以及 `VL_ROD_CPU_STAGE_TIMING` 下的 CPU 录制时间；
2. 在这三个口径下重测：1 / 4 substep、2000 guide、以及开关 SDF、开关碰撞体；
3. 顺便查 `GpuPassTimer` 为何让一个包含多个 dispatch 的分段多出 15 ms（已知分段之和
   才 0.21 ms）——这个工具得先可信，否则后面还会被它带偏。

### `GpuPassTimer` 本身不可信，先别拿它做判据

上一节之后又做了一次对照：perf jsonl 里只有 15 个受监控阶段，
2000 guide 那一行把 **76.44 ms** 算在 `hairSimulation` 名下（16 guide 同场景只有 0.81 ms）。
但同一构建实测：CPU `HairSimulationPass::Record` **2.5 ms**（含 rod 2.45 ms），
GPU 分段之和 **0.2–0.8 ms**。

三个口径又一次不可能同时成立，而这次矛盾指向**工具本身**：
`GpuPassTimer` 的 Begin/End 两端都在 `eBottomOfPipe`，这个位置上写入的时间戳
不保证两次写入之间只有本范围的命令：如果有其他队列的提交或
GPU 调度切换插在中间，它们的耗时会被计入这个分段。已知事实：
一个包多个 dispatch 的分段读到 15–76 ms，而同一批 dispatch 各自分段相加只有 0.2 ms。

**所以在搞清 `GpuPassTimer` 为什么会吮到外部耗时之前，不能再用它判定
「哪一段贵」。** 这也是这几轮反复出错的根源：工具本身没有先被验证。

### 下一步：先验证工具，再测性能

1. **验证 `GpuPassTimer`**：在一个已知耗时的 compute 前后写时间戳
   （例如人为循环一定次数），对比时间戳差与 CPU 侧壁钟；若不一致，
   就把 Begin/End 改成 `eComputeShader`→`eComputeShader` 而非 `eBottomOfPipe`；
2. **不依赖它重建一个可信基准**：fps / 帧时间 + `HairSimulationPass::Record` 的 CPU 壁钟（已有
   `VL_ROD_CPU_STAGE_TIMING`）；这两个至少自洽。
3. 然后才能回答「2000 guide 为什么 10 fps」——目前知道的只是：
   模拟链路 CPU 2.5 ms / GPU 0.2–0.8 ms，所以 92 ms 在别处，而现有仪表把它错记到了
   `hairSimulation` 头上。

### 唯一可信的信号：fps 与规模的关系，以及 800→2000 的断崖

既然时间戳工具不可信，就只用壁钟量下去。同一构建、同一并发设置（`--frame-limit 1000`
不节流、`--worker-thread-count 1`、默认 substep），200 帧取 fps 中位数：

| 模拟 guide 数 | fps | 帧时间 |
| --- | --- | --- |
| 16 | **111.6** | 9.0 ms |
| 200 | **42.9** | 23.3 ms |
| 800 | **39.0** | 25.6 ms |
| 2000 | **10.0** | 100 ms |

两个特征：

1. 200 → 800（guide 数 ×4）帧时间几乎不变（23.3 → 25.6 ms）；
2. 800 → 2000（×2.5）却跳到 100 ms（×4）。

所以不是线性的每-guide 成本，而是**在 800–2000 之间有一个断崖**。
这与更早那条「GPU 时间 200/800/2000 = 1.0/2.1/14.2 ms」的形状一致（那条虽然取自
不可信的工具，但形状与 fps 自洽）。

**下一步：直接针对这个断崖。** 候选机制都是「超过某个容量才会发生」的类型：

- 每帧诊断回读（`rodState` 逐 guide 16 个 float）带来的 GPU→CPU 副本与等待；
- 失败证据 CSV 写入的触发（2000 guide 时才有失败）；
- guide→strand 映射/上传缓存的容量阈值（如分块或队列上限）；
- 硬 guide 触发的 `RodGpuV3HardFrameIterations` 路径。

验证方式：在 800–2000 之间取几个点（1000/1200/1600）找出断崖的位置，再对着该位置
用 Tracy/NVTX 或手工计时看是哪一步。断崖位置本身就能排除一大半候选。

### C 的性能真相：不是引擎代码慢，是求解器在「追赶模式」里跑太多子步

上一节的断崖被证实**不是 guide 数**。一层一层剥下来：

| 隔离实验（2000 guide、不节流、单 worker） | fps |
| --- | --- |
| 派生场景（bind pose 角色、无 SDF） | 35.7 |
| 派生场景 + 声明 SDF 碰撞网格 | **35.1**（SDF 无影响） |
| 正式场景 | 10.0 |
| 正式场景 − 碰撞网格 | **10.0**（删掉也不快） |
| 派生场景，仅把角色换成**带动画的蒙皮网格** | **10.0** |
| 派生场景，角色保持 bind pose | **35.5** |

所以：**同一份头发资产、同样 2000 guide、同样胶囊，只把角色从 bind pose
换成带动画的蒙皮网格，帧率就从 35.5 掉到 10.0。** `hairSimulation` 也相应从
21.12 ms 涨到 58.96 ms，而两边的蒙皮工作量完全相同（1377429 顶点 / 21523 组）。

原因在每帧的子步数（诊断 CSV 的 `dispatched` 列，以 1/240 s 为单位）：

| 场景 | 每帧推进的仿真时间 | 等效子步数 |
| --- | --- | --- |
| bind pose | 404 ms | ~97 |
| 动画角色 | 929 ms | ~223 |

也就是说：头发求解器每帧推进的**仿真时间远超实际帧时间**（帧时间 28–100 ms，
而它每帧推进 0.4–0.9 s），即「追赶模式」：一旦某几帧变慢，债务积累，后续每帧
用更多子步去追，于是更慢，形成**正反馈循环**。解析形解释：223 次子步 × ~0.4 ms
≈ 90 ms，与实测 100 ms 帧时间对得上。

**所以 C 阶段的性能问题不是「哪个 kernel 慢」，而是「每帧该推多少子步」的策略问题。**
这也一次性解释了为什么双跑任何 GPU 阶段都「免费」、为什么 200→800 guide
帧时间几乎不变（那时还没进入追赶）、为什么 800→2000 突然跳。

### 下一步：处理追赶策略，而不是再优化 kernel

候选（都需要先定义「可接受的模拟时间落后」上限）：

1. 给 **每帧子步数硬上限**（现在实际跑到 97–223，远超 `RodGpuMaxSteps = 8`，
   说明上限没生效或被绕过），并明确它与债务计算的关系；
2. 当债务超过阈值时**主动丢弃模拟时间**（宁可头发比动画慢一点），
   而不是拿帧率去追；
3. 降低每子步的迭代预算（当前 `RodV2IterationBudget = 64`）——子步数与迭代数
   是可以互相换的，需要实测哪一种组合在同一模拟质量下更快。

验收标准仍然是：四场景 defect 与失败数不退化、零 validation，叠上 fps 与
实际模拟时间落后两个新指标。

### 更正：上一节的「每帧 97 / 223 子步」是错的

`receivedSeconds` / `dispatchedSeconds` / `debtSeconds` 是**累计列**，不是每帧值。
直接看原始 CSV 就能看出：

| 场景 | received 首→末 | dispatched 首→末 | debt 末 |
| --- | --- | --- | --- |
| bind pose | 0.12 → 5.38 s | 0.004 → 0.771 s | 4.61 s |
| 动画角色 | 0.12 → 8.95 s | 0.004 → 2.58 s | 6.37 s |

178 帧下换算成每帧：

- bind pose：received ≈ 30 ms/帧，dispatched ≈ 4.3 ms/帧 ≈ **1.0 个子步/帧**；
- 动画角色：received ≈ 50 ms/帧，dispatched ≈ 14.5 ms/帧 ≈ **3.5 个子步/帧**。

所以真实情况是：**每帧只跑 1–3.5 个子步**（`steps = min(8, debt/step)`
确实在生效），而不是 97–223。上一节那个算法把累计值除以 1/240 当成了子步数——
这是本会话第二次把累计列当成每帧列（第一次是 `received/dispatched`
被当成墙钟时间）。**后续任何用这三列的结论，都必须先看首尾两行。**

### 更正后的真正结论

仍然成立的是实验事实：同一头发资产、同样 2000 guide、同样胶囊，
只把角色换成带动画的蒙皮网格，`fps` 从 **35.5 → 10.0**；而 SDF 声明与否
（+mesh 35.1，−mesh 10.0）都不影响。

但**头发每帧只多跑了约 2.5 个子步**，按单子步 ~0.4 ms 算不过几毫秒，
**不可能解释 70 ms 的帧时间差**。同样，两边蒙皮工作量相同
（1377429 顶点 / 21523 组）、SDF 已被排除、CPU 录制 2.5 ms。

而现有 profiler 只监控 **15 个阶段**（hair、蒙皮、阴影），几何/后处理/UI
等阶段**完全不在监控范围内**。所以那 70 ms 很可能在根本没被监控的阶段里，
而不是头发。

### 下一步

1. 把 profiler 的阶段表补齐（至少覆盖 geometry / deferred / postprocess），
   否则「剩下的时间在哪」永远是猜；
2. 在补齐之前，可用一个很便宜的旁证：把动画角色换回 bind pose 但**保留动画骨骼更新**
   或反之，看 fps 变化落在哪一侧；
3. 头发侧的 debt 确实在积累（bind 4.6 s / 动画 6.4 s 于 178 帧），
   说明模拟时间跟不上真实时间，但这是**已知的、与帧率无关的**旧问题（它本身不制造 70 ms），
   应单独作为「模拟质量」课题处理。

### 动画角色差异本身不稳定，得先稳住才能给结论

上一节那组隔离实验里，「bind pose = 35.5–35.8 fps」在三次独立运行里都稳定，
但「动画角色 = 10.0 fps」并不稳：同一场景、同一设置（`--frame-limit 1000`、
`--worker-thread-count 1`、同样 180 帧）有一次跑出 **21.7 fps**，而前三次都是 10.0。
两个工作线程的那次是 10.1。

也就是说：**动画角色那一侧的帧率有 ~2x 的跑间波动**，而 bind pose 一侧稳定。
很可能与动画推进本身有关（跑得越慢，动画越往后，负载可能越重）。

因此：

- 「同一头发资产、同样 2000 guide，只换角色就从 ~35 fps 掉到 ~10 fps」这个**方向**
  是可信的（两侧差距远大于波动）；
- 但**具体倍数（3.5x 还是 1.6x）不可信**，不能拿它去验证任何机制；
- 之前用这组数字做的「头发只多跑 2.5 个子步」推断也只是量级估算，不是结论。

### 下一步（先稳住再测）

1. 用**快照式对比**而不是平均帧率：固定相同的模拟帧数（如 60 / 120 / 180 帧各取一个 fps），
   看帧率随时间的走势，而不是只取一个中位数；
2. 确认动画是否真的推进（关掉动画 ↔ bind pose 应该等价），
   否则两个场景的差异可能还有别的来源；
3. 只有在第 1、2 步稳定之后，才能回到「那 70 ms 在哪」的定量问题，
   并把 profiler 的阶段表补到覆盖 geometry / deferred / postprocess。

这也是本阶段的总结性教训：**先确认测量本身在同一设置下可重复，再用它推断机制**。
这个会话里因为跨过这一步，已经反复得出了不可靠的结论。

### 重大更正：「10.0 fps」是 clamp 地板，不是测量值

把测量协议交给子代理重做（22 次启动、二进制 SHA256 全程未变），得到两条
源码级事实，都已自行复核：

1. `fps` 字段是 `1 / clamp(frameDelta, 1/240, 0.1)`
   （`renderFrameRecorder.cpp:225-229`），所以 **`fps` 被夹在 [10.0, 240.0]**。
   任何慢于 100 ms 的帧都报成 恰好 `10.000`，`cpuMs` 报成 `100.000`。
   实测：61 个 skinned 行里 **32 个恰好是 10.000**（32/61 ≈ 52.5%），这是地板而非速率。
2. `--perf-jsonl` **每秒最多写一行**（`enginePerformance.cpp` 的 `TickPerformanceJsonl`），
   而不是每帧一行。所以「跨 200 帧取中位数」实际上是对 2–11 个样本取中位数。

而 `gpuTotalMs` 与 `passes[].ms` 是**未 clamp** 的时间戳值，才是可信量。

**修正后的稳态（600 帧标定，±3%）：**

| 场景 | 稳态 fps | 帧时间 |
| --- | --- | --- |
| bind pose 角色 | 33.3–35.3 | 28.3–30.0 ms |
| 动画角色 | 18.2–18.8 | 53.3–55.0 ms |

所以动画角色的代价是 **约 1.84x（+80%）**，**不是 3.5x**。之前那个「10 fps」是 ≥100 ms 的
卡顿被 clamp 抹平后的地板值。

### 另一个真实现象：动画场景的头发成本随时间上涨且呈锯齿

同一组量重复，但变换运行长度（取最后一行的未 clamp 值）：

| 帧数 | bindpose hairSim | skinned hairSim |
| --- | --- | --- |
| 60 | 28.2 ms | 27.8 ms |
| 120 | 23.7 ms | 44.4 ms |
| 180 | 21.2 ms | 55.9 ms |

- bind pose：趋于**下降并稳定**（600 帧后横在 21.2–21.8 ms）；
- 动画角色：**在 40→62 ms 之间以约 9–11 s 为周期锯齿上涨**。

锯齿 + 上涨形状与前面记录的 debt 持续累积（bind 4.6 s / 动画 6.4 s）一致，说明它不是稳态负载而是
**随时间演变的行为**。

因此以后的测量纪律（已分交给子代理执行）：

1. **不用 `fps` 做头发性能判据**，用 `gpuTotalMs` / `passes[].ms`；
2. **固定帧数**做对比，且 ≥180 帧（成本会漂移 100+ 帧）；
3. 比较时必须报**同一帧数下的 CV**（实测 0.7–5.1%，在 ±10% 内）；
4. 若要报 fps，必须标注它是否撞到 10.0 地板。

### 子代理审计：debt 无上限，每帧最多退休 33.3 ms，负反馈从结构上不可能收敛

以下来自只读源码审计（未编译、未运行），关键常量与算术已逐条核对：

```text
substepBudget = (progressive && maxIterationsLastFence > 12) ? 1 : 8     rodGpuRecording.h:310-312
steps         = min(substepBudget, floor((debt + 1e-9) / (1/240)))      rodGpuRecording.h:313-315
added/frame   = delta                                                   rodGpuRecording.h:295-301
retired/frame = steps / 240                                             rodGpuRecording.h:555-556
```

**每帧最多只能退休 8 × 1/240 = 33.3 ms 仿真时间**（硬帧状态下只有 4.17 ms），
而每一帧都会加入 `min(frameDelta, 0.1)`。所以只要帧慢于 30 fps，debt 就只会单调增长，
**而清零 debt 的唯一途径是 pose 历史的 `Reset`**（即传送）。

用两个常量直接算出的结果与实测列对得上（误差 < 2%）：

| 场景 | 每帧加入 | 每帧退休 | 子步/帧 | 净增长 | ×178 帧 | 实测 debt |
| --- | --- | --- | --- | --- | --- | --- |
| bind pose | 29.7 ms | 4.33 ms | 1.04 | +25.4 ms | 4.52 s | 4.61 s |
| 动画角色 | 49.9 ms | 14.55 ms | 3.49 | +35.3 ms | 6.29 s | 6.37 s |

三个直接推论：

1. **「1.0 个子步/帧」本身就证明硬帧分支常开**：约 3 帧之后 debt 就 > 8/240，
   `floor(debt/step)` 永远 > 8，能跑出 1.04 只能是 `substepBudget == 1` 几乎每帧生效。
   bind pose 因此**长期停在硬帧分支**，与它已知的「有 guide 永不收敛（maxIterations=64 > 12）」
   一致——**一个坟子 guide 拖住了整个 2000-guide 对象**。
2. 动画场景靠 8 步上限也救不回来：178 帧共收到 8.83 s，即使每帧都跑 8 步也只能退休 5.93 s，
   ≥ 2.9 s 是不可避免的，实测 6.37 s 还多了「约 64% 帧实际只跑 1 步」那部分。
3. **正反馈在帧循环里闭合**：头发 pass 本身约 55 ms > 33.3 ms，每帧加入都大于可退休上限，
   唯一平衡点是 `debt → ∞`。

### 锯齿的来源：动画循环回绕触发 Reset

循环动画在回绕时会增加 `SkinPose::discontinuity`（`skeletalPlayback.cpp:170-178`），
`GuideSimulationHistory::Plan` 把它变成 `Reset`（`tressfxSimulationHistory.h:60-70`），rod 路径随后调
`ClearTimeline`（`rodGpuRuntime.h:198-214`）——**它把 received/dispatched/debt 全部归零、重新挂起硬帧门**。
这同时给出了锯齿的下降边（debt 被抹掉、回到 1 步）与随后的重新爬升。

独立旁证：`tool/hair/capture_hair_physics.py:556` 记录 Idle 循环约 **10 s** 回绕一次并触发 reset；
`documents/rationale/tressfx5-hair-physics-record.md:459` 记录了 600 帧运行的第 479 帧出现 `reason=4 (pose-discontinuity, loop-wrap)`。

### 审计顺带发现的缺陷（含我自己两个）

1. `debtSeconds` **永不上限、不衰减、不丢弃**，只有 `ClearTimeline` 会清零。
2. 硬帧门是**对象级且取 guide 最大值**：一个不收敛的 guide 就把 2000 个 guide 全部降到 1 子步。
3. 唯一清 debt 的事件是传送：`ClearTimeline(pose)` 从当前姿态重新起算，**静默丢弃 4–6 s 永远没仿真过的时间**。
4. 门的输入 `maxIterationsLastFence` 写了两处、**任何地方都不输出**，CSV 也没有 `substeps` / `iterations` 列。
5. `failureEvidenceWritten` 置位后**从不复位**（连 `ClearTimeline` 也不清），于是每个对象生命周期内只会捕获一次失败证据。
6. **我上一轮加的 `hair.rodSolve` GPU 分段测不到任何东西**：`BeginPass` 后紧接 `EndPass`，
   而 `RecordRod` 在二者之后（`hairSimulationPass.cpp:493-499`）——那个 `0.00 ms` 是我自己造成的假象。
7. `VL_ROD_FIXED_SUBSTEPS` **会破坏 debt 记账**（绕过 `debt/step` 限制，debt 可以变负、间隔队列被清空）——
   **用它测成本可以，但不能用它读 debt**。
8. `VL_ROD_ITERATION_BUDGET` 是 GLSL 编译期定义，不是环境变量，用它要重建着色器。
9. `[HairCPU]` 把「本帧值」与「累计平均」混在一行打印，容易读错。

### 审计对下一步的建议（已按性价比排序）

- **把硬帧门从对象级改为逐-guide 或逐-warp**：一个坟子 guide 不应该把 2000 个 guide 拖到 1 子步。
  这是 bind pose 帧时间的直接成因，且不改变大多数 guide 的物理。
- **给 debt 加上限与主动丢弃策略**（宁可头发比动画慢一点，不要用帧率去追），
  否则唯一的减压阀是传送式 reset（也就是那个可见的头发跳变）。
- 把 `substeps` / `maxIterationsLastFence` 写进诊断 CSV：现在帧费用的主要自变量完全不可观测。

### 探针 bug 修复后：36 个 pass 重新可见，hair 确实是帧成本主体

之前「只有 15 个监控阶段」不是引擎只有这么多 pass，而是**探针泄漏**：
`GpuPassTimer` 只有一个不可重入的 `passOpen` 闸，而 `renderFrameRecorder.cpp` 里
`VolumetricFog` / `ParticleSoftDepth` / `LightCulling` 三个分支 `continue` 前**没有 `EndPass`**，
闸从此保持为真，之后所有 pass 的 `BeginPass` 静默失败。因为 pass 顺序把这三个
排在 geometry 之前，所以**几何、deferred、forward、sky、fog、bloom、toneMapping 全部不可见**。

修复后（已提交 `5f63f20d`），同一场景的 GPU 表变成 **36 个图形 pass +
hairSimulation + skinnedDeform，共 38 行**，且 `lightCulling` 不再是 `n/a`。

修复后的 bind-pose 帧内分布（窗口 1–120）：

```text
hairSimulation 23.30 ms | skinnedDeform 1.30 | geometry 0.28 | deferredLighting 0.17 |
forwardTransparent 1.06 | forwardOpaque 0.08 | toneMapping 0.12 | 其余均 <0.05 | total 26.98 ms
```

**所以头发 pass 确实占了整帧的绝大部分（23.3 / 27.0）**——这与前面几轮
「hair GPU 只有 0.2 ms」的结论相反；那个 0.2 ms 来自两个错误：

1. 分段模式下上层不开整链分段，而当时又只把 `RecordPass` 和影子包了进去；
2. **我自己把 `hair.rodSolve` 写错了**：`BeginPass` 后紧接 `EndPass`，而 `RecordRod`
   在二者之后（`hairSimulationPass.cpp:493-499`），所以那个「0.00 ms」是假的。

### 同时更正：`renderRecord` 的 27→63 ms 不是 CPU 录制

新增的 `[RecordCPU]`（`VL_ROD_RECORD_STAGE_TIMING` 门控）直接量到：

| 项 | bind pose | 动画角色 |
| --- | --- | --- |
| 整段 pass 录制循环 | 0.95 ms/帧 | 1.03 ms/帧 |
| `RecordSkinDeform`（含 palette 上传） | 0.037 ms/帧 | 0.043 ms/帧 |
| 合计 | **0.99 ms/帧** | **1.04 ms/帧** |

两边只差 **0.05 ms**，而 `renderRecord` 差了 +36.9 ms。原因：单工作线程下
`renderRecord` 包住了 `RenderLatestSnapshotOrLastGood()`，其第一步就是
`BeginFrame → WaitForFence`（`rendererBackendVulkan.cpp:450-453`），**它测的是 GPU 帧时间而非 CPU 工作**。
双工作线程对照：`renderRecord` 两边都是 0.002 ms，同一个差值转移到 `submitPresent`
（28.9 vs 53.6–57.7 ms）。

**因此那 +35 ms 是 GPU 头发求解工作，不是录制书记。** 在固定 8 个子步下直接对比：
hair GPU **bind 30.6 ms vs 动画 104.5 ms（约 3.4x）**——动画角色让**求解器本身**贵了 3.4 倍。

### C 阶段的真正结论（经三次推翻后）

1. 帧成本主体是**头发 GPU 求解器**（23–60 ms），不是录制、不是蒙皮、不是 SDF、不是后处理。
2. 它的成本 = `子步数 × 每子步迭代代价`，而**子步数由无上限的 debt 驱动**（每帧最多退休 33.3 ms）；
   迭代代价随根骨运动量上升（固定子步下 3.4x）。
3. bind pose 之所以也慢，是因为**一个不收敛的 guide 把整个 2000-guide 对象
   钉在 1 个子步**（门取 guide 最大值），于是 debt 只能单调上涨，
   唯一的减压阀是传送式 Reset。

### debt 上限已接受，新门判据改为默认关闭（A/B 为负结果）

子代理实现了三件事并实测，我的整合决定是**接受两件、把第三件降级为可选**：

| 改动 | 实测 | 决定 |
| --- | --- | --- |
| debt 上限 0.5 s（超出主动丢弃 + 计数） | debt 在 400 帧后饱和在上限，不再无限上涨 | **接受** |
| 诊断 CSV 追加 substeps / maxIterations / hardGuides / droppedSeconds（前 10 列不变） | 成功，14 列 | **接受** |
| 硬帧门改为「超阈值 guide 计数」判据 | bind pose **22.71 → 45.39 ms**，动画 47.07 → 61.38 ms | **默认关闭**，保留 `VL_ROD_GATE_MODE=new` |

新判据变慢的原因很具体：它让更多 substep 跑起来，而那几个
`maxIterations = 64` 的 guide 会在**每个 substep** 里重算。固定子步的对照组也印证了这点：

| 配置 | bind hair | maxIterations |
| --- | --- | --- |
| `VL_ROD_FIXED_SUBSTEPS=8` | **32.9 ms** | 8（收敛） |
| 新门、8 子步 | **45.4 ms** | **64**（卡住） |

同样是 8 个子步，一个收敛、一个卡在 64，帧费用差 38%。

**所以真正的杠杆不是门，而是「为什么那几个 guide 永远收敛不了」。**

### 一个子代理推测被否定：reset 并没有被丢掉

子代理在风险里提到「reset 帧 `steps` 被强制为 0，而 `RodInitialize` 只在 dispatch
执行时才跑，所以 reset 到不了 GPU」。我逐行核对后否定了这条：

- `rodGpuRecording.h:542` 的 `solver->Dispatch(commandBuffer, state.guides, 1, 1)` 是**无条件**的；
- `steps` 只影响 `params.substeps`，也就是 shader 内部走几个子步；
- `params.flags.x`（reset 标志）在 `rodGpuRecording.h:393` 上传，且在同一帧末尾才清除（:601）。

所以 reset 帧仍会发一次 dispatch（substeps=0），`RodInitialize` 会跑。这条路径正常，
不能用它解释卡住的 guide。

### 重测：占用率确实是瓶颈，shuffle CR 确实有效——两个旧结论都被推翻

之前「加 8/16/24 KB dummy shared 几乎无影响 → 占用率不是瓶颈」
与「shuffle CR 无速度收益」这两个结论，都是用**含空等的粗分段**
量出来的。工具修复后重测（固定 8 子步、400 帧、行 CV 0.4–1.3%）：

**实验 A：用 dummy shared 测对占用的敏感度（shuffle CR 开）**

| 静态 shared | bind hair | Δ | 动画 hair | Δ |
| --- | --- | --- | --- | --- |
| 14.36 KB | 31.01 ms | — | 105.44 ms | — |
| 22.36 KB | 31.10 | +0.3%（噪声） | 119.66 | **+13.5%** |
| 30.36 KB | 33.30 | +7.4% | 139.09 | **+31.9%** |
| 38.36 KB | 41.86 | **+35.0%** | 167.90 | **+59.2%** |

单调且 15–40 倍噪声。所以**静态 shared 占用确实限制这个 kernel**（而 pad 是死数据，
所以因果路径只能是分配体积本身：驻留 workgroup 数和/或 L1 carveout，
这个实验无法区分两者，审计里也如此声明）。

**实验 B：shuffle CR vs 共享内存 CR**

| 场景 | shuffle CR | shared CR | Δ |
| --- | --- | --- | --- |
| bind 2000 | **31.01 ms** | 33.53 ms | **−2.52 ms（−7.5%）** |
| 动画 2000 | **105.44 ms** | 149.08 ms | **−43.65 ms（−29.3%）** |

静态 shared：shuffle **14,700 B** vs shared **34,668 B**（差值恰好是
4×`RodV2Matrix[32]` + 2×`RodV2Vector[32]` = 19,968 B）。两个 Δ 分别是 ~10σ 与 ~70σ。

而 pad 实验给出了机制：30.36 KB 的**死** shared 就能复现几乎全部共享版惩罚
（bind +7.4% vs +8.1%），pad16→pad24 线性外推到 33.86 KB 预测动画 +43.8%、实测 +41.4%。
**所以 shuffle 的收益几乎全部来自体积减小，而不是指令更快。**

### 由此确定的下一个杠杆：继续压静态 shared 体积

当前静态 shared 只剩 **14.36 KB**，全部是 per-point 状态（`positions`/`velocities`/
`orientations`/`targets`/`masses`/`restStrains` 等约 30 个 32 元素数组）。它们大多数只被
本 lane 访问，而邻点访问可以用 `subgroupShuffle`（和 CR 那一刀完全同一套变换）。

按 pad 曲线估算：从 14.36 KB 降到 ~4 KB，动画场景应该能拿到两位数百分比。
这是目前唯一有实测依据、且不需要改算法的方向。

### shared 体积不是单调杠杆：它被占用率量化台阶支配，且 7 个驻留 workgroup 是陷阱

把 per-point 状态从 shared 搬进寄存器的尝试做了，结果是**一个带数字的否定结论**，
而且揭示了一个比「少用 shared」更重要的机制。

**清点（以默认非 SDF pipeline 为准）**：活着的静态 shared 共 **14,700 B**，其中

| 访问类型 | 数组数 | 字节 | 占比 |
| --- | --- | --- | --- |
| 只被本 lane 访问 | 21 | 9,204 | 62.6% |
| 需要邻点访问 | 8 | 3,904 | 26.6% |
| 归约/广播/动态 | 21 | 1,592 | 10.8% |
| 声明但被 DCE 消除 | 49 | 0 | — |

且其中 4,304 B 在 `rodKernel.glsl`，V1 的 `rodSolve.comp` 也编译它、那些数组在 V1 里确实有邻点访问，
不能就地改。

**第二阶段：4,532 B 转寄存器，数值完全通过但性能回退，已回退**

- shared：14,700 → **10,168 B**（−30.8%，SPIR-V 里验证）；
- 门禁：`cap16` **0.009791187942**、`static` **15.92730141**（2 失败）、`sdf_static` **0.008789402433**、
  `sdf_animated` 0.00998（在该场景历史波动带内），零 VUID——**正确性没有问题**；
- 性能：bind **30.891 → 33.564 ms（+8.7%）**，动画 **105.576 → 102.065 ms（−3.3%）**。

一个场景变好、另一个变差，所以回退。

**为什么：两个独立机制**

1. **寄存器压力**：每移一组 *向量* 横向量，bind 就输 3–9%——因为这些值要活过
   试算循环的 `barrier()` 与内联的 KKT 求解；只有单个标量是净收益。
2. **占用率量化**：14,700 B 只能放 **6** 个 workgroup/SM，14,572 B 能放 **7**，而
   **7 个在动画 kernel 上比 6 个还差**。直接证据：给只换了标量的那个版本
   补 128 B 死数据回到恰好 14,700 B，动画就回到 104.99 ms（−0.6%），而 bind 仍然 −2.9%。

100 KiB carveout 下的阈值（实测推导）：

```text
6 wg <= 17066 B   7 wg <= 14628 B   8 wg <= 12800 B
9 wg <= 11377 B  10 wg <= 10240 B
```

所以「降 128 B 的进步」实际上是一次 4.4% 的动画回退；而要到真正好的 10 wg 点（≤10,240 B）
必须付出那个在 bind 上输 8.7% 的寄存器代价。

### 对策：优先「不增寄存器的减法」，并避开 7 wg 陷阱带

- `v2Metric`（512 B）是纯 lane-0 归约，可以用 `subgroupAdd`/`subgroupMax` + 广播替换，
  不增加长存活向量——但单独降 512 B 会落在 7 wg 陷阱带，必须和其他减法一起做到 ≤12,800 B（8 wg）。
- `rodKernel.glsl` 里那 4,304 B 需要 V3 专用声明才能动（V1 共用），而且预期会遇到同样的 bind 惩罚。
- 一个**不改体积**的小胜利已测到：`v3CubicStiffness` 转寄存器 + 128 B 死数据补齐到 14,700 B
  → bind −2.9%、动画 −0.6%。但那 128 B 是占用率的承重部件，属于证据而非应该落地的设计。

### C 阶段的体积杠杆基本到头

14.36 KB 里真正可安全削的只有那 8.99 KB own-lane 部分，而它已经被证明是
**寄存器压力与占用率的零和博弈**：换来 10 wg 的代价是 bind +8.7%。
要真正突破，需要减少**每点状态本身**（合并或压缩数组），而不是在 shared 与寄存器之间搬家。

### 子步长改为 1/120 s × 4：既提速，又修复了「永久卡住的 guide」

此前我把静态场景那两根卡住的 guide 写成「**作者冲突，不是求解器缺陷**」，
依据是「把预算加到 128 轮也收敛不了」。**那个结论是错的**：它只试了预算一个维度，
没试子步长。把步长从 1/240 s 改为 1/120 s 后，它们**收敛了**。

机制：`RodKkt10RepairOverlap` 每个子步把嵌入胶囊的点推到表面，牛顿再把它拉回弹性静止形状；
在 1/240 s 的小步长下这两股力量处于持久拉锯状态，进度不可能积累；子步变大后每步能走出更多位置，
于是能脱离这个局部最小点。

**新旧对比（600 帧 soak，同构建同会话）：**

| 场景 | 配置 | 每 1 ms 仿真成本 | defect | 失败 | reset |
| --- | --- | --- | --- | --- | --- |
| bind | 旧 1/240×8 | 5.10 | 15.9268（永久） | 597 帧 | 0 |
| bind | **新 1/120×4** | **0.45** | **0.00996868** | 仅 f2–61 启动段 | 0 |
| 动画 | 旧 1/240×8 | 3.10 | 9.02 | 192 帧 / 361 行 | 2 |
| 动画 | **新 1/120×4** | **2.45** | **1.02** | 9 帧 / 31 行 | 3（均为循环回绕 reason=4） |

等退休隔离（强制子步）确认是步长本身的收益：bind **14.85 vs 30.83 ms（2.08x）**，
动画 **80.01 vs 106.99 ms（1.34x）**。

### 基准变更（需要明确记录，不是静默改数）

| 场景 | 旧基准 | 新基准 |
| --- | --- | --- |
| `SC_rod_v3_capsules_16` | 0.009791187942 / 0 失败 | **不变** |
| `SC_tressfx_rod_sdf_static` | 0.008789402433 / 0 | 0.008789401501 / 0（同量级，末位浮动） |
| `SC_tressfx_rod_static` | **15.92730141 / 2 失败 / gpuStatus 3** | **0.00996868033 / 0 失败 / gpuStatus 0** |
| `SC_tressfx_rod_animated` | 0.00998（跑间波动 0.0098–0.0100） | 同范围 |
| `SC_tressfx_rod_sdf_animated` | 同上 | 同范围 |

`SC_tressfx_rod_static` 那一行是**改善**：十年的「绑定姿态冲突」债务消失，而不是基准被调松。
顺带：这个场景不再需要「2 根已知失败」这个例外说明。

### 补充小坑

- `VL_ROD_FIXED_SUBSTEPS` 现在被夹到策略上限（4），因此做旧的 8 子步 A/B
  必须同时设 `VL_ROD_SUBSTEP_CAP=8`（代码里已注明）。
- 门禁数值跑间有 ~1e-8（`capsules_16`）到 ~3e-5（`sdf_animated`）的振动，
  因为采样轨迹依赖 frame-delta 与 debt 模式；判定基准要用量级而不是逐位相等
  （`capsules_16` 和 `sdf_static` 除外，它们仍然精确）。
- 被恢复那根 guide 现在**会发布几何**（之前永不发布），属于可见变化，但它收敛到 1e-2 可视阈之内，
  是正确行为。

### 与外部方案的量级对比：差距在算法，不在参数

C 阶段一直在 1.3—1.5x 的范围里摩。为了确认这个范围本身是否合理，查了当前公开的同类方案：

**Stable Cosserat Rods**（Hsu / Wang / Wu / Yuksel，SIGGRAPH 2025，DOI 10.1145/3721238.3730618；
摘要与全文 <https://par.nsf.gov/biblio/10668229>，GPU 实现
<https://github.com/jerry060599/YarnBall>）。它的三个关键点直接对应我们的代价结构：

1. 细杆的角动量可忽略（J = 0），于是隐式变分问题拆成**位置与姿态交替的两个局部优化**；
2. 姿态步有**闭式 Gauss-Seidel 更新**：令 λ ≈ |v| + |b|（v 为拉伸项、b 为弯曲项之和），则
   `q = normalize(v e3 + λ b)`；需要严格对齐传统 Cosserat 能量时可再加一步不动点迭代；
3. 位置步用 **Vertex Block Descent**（每顶点局部块 Hessian，**无全局线性系统**）。

它的实测：**每个时间步 4 次迭代就收敛**（同一问题 XPBD 需要上千次、VBD 收敛不到）；
**1.4M 发丝顶点、不含碰撞 7 ms/帧（RTX 3090）**，并且明确写着「**每根发丝 32 个顶点恰好能放进
shared memory**」、GPU 上只需位置/姿态分两个 pass + Jacobi 更新。

对比我们（同一种状态量：发丝杆顶点）：

| | 我们的 V3 | Stable Cosserat Rods |
| --- | --- | --- |
| 每子步迭代 | 最多 **64** 次牛顿（merit + 回溯线搜索） | **4** 次 |
| 每次迭代做什么 | 每点 10 自由度 KKT → Schur 写成 6×6 → 32 块循环约化（5 层） | 每顶点 3×3 对角局部解 + 每段闭式四元数 |
| 规模 / 时间 | 64,000 点，稳态 47 ms | 1.4M 点，7 ms（不含碰撞） |
| **每点-迭代成本** | **~175 ns** | **~1.25 ns** |

即使按「GPU 差 2.5×、碰撞占一半」去折算，仍是 **20–25× 的结构性差距**。
这说明我们这几轮 1.3—1.5x 的收益并不是「已经没空间」，而是**没碰到真正的杠杆**：
我们在一个「贵的全局求解器」里优化，而行业当前答案是「**局部闭式 + 4 次迭代**」。

**ADMM 路线**（Daviet，"Interactive Hair Simulation on the GPU using ADMM"，
<https://research.nvidia.com/labs/prl/daviet23/interactivehair2023.pdf>）作为备选：它把弹性与摩擦接触分解成 ADMM 子问题，
报告**一次 ADMM 迭代比一次牛顿迭代便宜约 30×**，16k 发丝 / 480k 粒子可交互（每帧 4 个时间步 × 10 次迭代）。
它仍需全局预分解矩阵，因此作为第二选择。

**顶点数也是可用杠杆**：[TressFX 5.0 开发者指南](https://gpuopen.com/download/TressFX5.0_DeveloperGuide.pdf)
给出的参考是短发 8 个顶点、长发 32、需要更滑的卷发 64。我们固定在 32，
而且 32 也是我们 CR 块数与 shared 体积的根源（降到 16 就同时减半工作量与体积）。

### 由此形成的方向

不再在 V3 的 KKT/CR/线搜索里继续微调，而是引入一个**局部闭式求解器 profile**（V4）：位置与姿态分 pass、
接触改局部 barrier/投影、取消 CR 与 Schur 凝聚，V3 保留为回退。
详细设计与分阶段迁移计划另行编写，并由独立审核体对抗性复核。

### 两行漏掉的定义：SDF 管线一直在跑共享内存版 CR，补上后 −30.2%

正在编写 V4 方案的子代理发现了一件下面这种事：`rodV3ImplicitSdf.comp` 自己的注释写着
「与 `rodV3ImplicitCrCompensated.comp` 的唯一差异是 `VL_ROD_SDF`」，**但它同时缺了
`#extension GL_KHR_shader_subgroup_shuffle` 与 `#define VL_ROD_CR_SHUFFLE` 两行**。于是：

- 我们的**4 个 `SC_tressfx_rod_*` 验收场景全部声明了 `hairCollision.mesh`**，跑的是 SDF 管线；
- 而 SDF 管线一直用的是**共享内存版 CR**，静态 shared **34,668 B**（约 2 个 workgroup/SM），
  而不是我一直引用的 14,700 B；
- 只有 16-guide 胶囊基准场景（`hairCollision` 只有 `capsules`）跑 14,700 B 那条。

补上那两行后实测（同构建、`SC_tressfx_rod_sdf_animated`、固定 4 子步、400 帧）：

| 配置 | 静态 shared（自己用 SPIR-V walker 复核） | hair 中位数 | CV |
| --- | --- | --- | --- |
| SDF + 共享内存 CR（原状） | 34,668 B | **107.80 ms** | 1.7% |
| SDF + shuffle CR（+2 行） | **14,700 B** | **75.21 ms** | 0.9% |

**−30.2%，而且只是补上漏掉的两行。** 这是整个 C 阶段单项最大的收益，比我这几轮磨出来的 1.3–1.5x 大得多。

门禁：`cap16` 0.009791177697、`sdf_static` 0.008789401501、`static` 0.00996868033、
`animated` 0.009974499、`sdf_animated` **0.009568191**（比原来的 0.0098–0.0100 更接近收敛，
因为 CR 实现换了、舍入路径不同）；全部 0 失败、`gpuStatus 0`、零 validation。

**教训：注释声称的「唯一差异」与实际 define 集不一致，会让整个验收集在一个从未测过的配置上跑。
以后新增管线变体时，应当用脚本比对各 `.comp` 的 define 集，而不是相信注释。

### 最重要的一个发现：V3 的 SDF 接触从未生效，而且修对之后求解器吸收不了

**事实链**（每一环都有 file:line 或实测）：

| 环节 | 证据 |
| --- | --- |
| 求解器位置与胶囊几何在**米** | 录制时 `transformed.a/b/radius *= 0.01f`（`rodGpuRecording.h:432-434`） |
| SDF 网格在**厘米** | `tressfxSdfCommon.glsl:22`；运行日志 origin≈123 cm、cellSize≈1.13 |
| V1 正确做法 | `rodKernel.glsl:229` 先 `* 100.0` |
| V3 缺这一步 | 域检查全拒：**0 接受 / 133,353 拒绝** |

**而这个缺陷是本会话第 20 轮左右我自己造成的**：当时把 `position * 100.0` 删掉，
依据是「`RodKkt10PointPosition` 已经是厘米」——错了；它和胶囊同一块米制坐标。
当时看到 defect 从 0.542 降到 0.00828，当成修复，实际是**把 SDF 接触整个关掉了**。

为什么没被发现：**验收指标压在收敛地板上**（`RodKkt10ConvergenceFloor = 1e-2`，而所有绿色
基线都是 0.0088–0.0100），所以「算对了」和「没接触」它分不出来。同时 SDF 场景的牛顿迭代数是 **0**（`{0:2000}`），
因为没有接触、残差天生就低于阈值——这也解释了为什么 CR 的任何改动在 SDF 场景上「没有变化」。

**修对之后更难：求解器吸收不了真实的 SDF 接触。**
打开 `VL_ROD_SDF_UNIT_FIX` 后，SDF 接触首次真正生效（63,962 个采样被接受、
1,999/2000 guides 有接触），但四个验收场景全部崩掉：

| 场景 | 修正前 | 修正后 | 失败 guide |
| --- | --- | --- | --- |
| `SC_tressfx_rod_static` | 0.00997 | **1.0446** | 11 |
| `SC_tressfx_rod_sdf_static` | 0.00879 | **1.0979** | 8 |
| `SC_tressfx_rod_animated` | 0.0098–0.0100 | **7.358** | 5 |
| `SC_tressfx_rod_sdf_animated` | 0.0095–0.0100 | **0.255** | 9 |

每毫秒仿真成本贵 2.6–13.4×，`maxIterations` 32–64，`gpuStatus 3`。
两个静态场景的失败值可精确复现（帧 148 与 398 相同），是锁死状态而非瞬态。

**处理**：修正进树但**默认关闭**（`VL_ROD_SDF_UNIT_FIX`），HEAD 保持绿，并在契约里写明
「那四个场景的绿色基线不是 SDF 接触可用的证据」。

### 对 V4 方案的影响

审核体据以估算的「SDF 路径占比 f ≈ 0.285」、「忠实移植约 1.0×、最多 3.7×」等数字，
**建立在「SDF 查询什么都不做」的前提上，因此全部作废**。真实占比要等 SDF 接触能被吸收
之后才能测量。在那之前，性能优化的数字都是「把功能关掉才跑得动」的数字。

另：**本会话重复出现的「测出某个改动无效」，很大一部分都是这个缺陷造成的假阴性**：
只要测试场景带 `hairCollision.mesh`，SDF 查询就一直在域检查处早退，于是 SDF 梯度、SDF 采样数、
CR 实现、接触几何缓存……这些改动都“测不出区别”。

### 成本分解（终于可信的一次）：CR 占 67%

用现有的双跑探针（都带「运行期永假、编译期不可知」的反 DCE 引用），
在 SDF 接触真正开着、固定 4 子步、300 帧、`passes[hairSimulation].ms`（未 clamp，CV 0.8–1.4%）下重测：

| 配置 | hair | 差值 = 该段占比 |
| --- | --- | --- |
| 基线 | **84.53 ms** | — |
| 双跑 CR | 141.56 ms | **CR = 57.0 ms，67%** |
| 双跑压缩 | 92.76 ms | **压缩 = 8.2 ms，10%** |
| 余下 | — | 接触查询 + 残差 + 线搜索 + 装载 + 写回 ≈ **19.3 ms，23%** |

**这一条直接支持 V4 的核心主张**：三分之二的时间花在「耦合 6×6 块的带求解」上，
而位置/姿态解耦（Stable Cosserat Rods 的 J=0 拆分）之后这一步**不存在**，不是被优化。

也回答了「能不能预烘焙」这个问题：**CR 本身不能烘焙**，因为每个 6×6 块都由
当前状态决定（弹性刚度、惯量、接触梯度）。可烘焙的只有三类：① 每 guide 的模态/PCA 基（子空间求解，
预期 5–10×，但改变模型）；② guide 聚类（离线做 300–500 个代表 + 散布权重，预期 4–6×）；
③ rest 数据（已预烘焙）。

### 门判据修正：头发不跟头皮走的真正原因

业主从截图发现「动画下头发没跟着头皮动」。数据对应的是：

- 旧判据取 **guide 最大值**，而启用真实 SDF 接触后 2000 个 guide 里有 **1–5 个**（0.05–0.25%）
  迭代到 16–64 轮 → 整个对象被钉在每帧 **1 个子步**；
- 于是仿真每帧只推进 4.2 ms，而帧长 43 ms → **约 10% 实速**；根骨按仿真时间采样，
  所以连根部都停在过去的姿态，看起来就是脱离头皮；

改成「超阈值 guide 计数 > 1%」后（`VL_ROD_GATE_MODE=legacy` 可回退）：

| | legacy | 计数判据 |
| --- | --- | --- |
| 子步/帧 | 1.00 | **4.00** |
| 仿真推进 | 4.2 ms/帧 ≈ 10% 实速 | **16.7 ms/帧 ≈ 39% 实速** |
| 每 ms 仿真成本 | 10.74 ms | **3.47 ms** |
| 帧时间 | 44.7 ms | 57.9 ms |

在「允许少量穿插、整体动态对即可」的判据下，**这个交换是对的**：
头发不跟头皮就是动态错，比少量穿插严重得多。

### 剩余账：还差 1.75× 才能真正跟上

一个子步推进 1/120 s = 8.33 ms 仿真，而当前每子步约 14.5 ms（实测 57.9 ms / 4）。
所以即使跑满 4 子步也只有 39% 实速；**要真正跟上必须把子步成本压到 ≤8.33 ms，即便宜 1.75×**。
而成本分解说：这 1.75× 有 67% 的可能性来自取消 CR，而不是优化它。

### 迭代预算居然不是杠杆：降 N 只省 10–15%，但会直接把求解器饿死

为了验证离线原型的预言（「32 顶点、h/l=0.86 的杆上 4 次迭代不够」），
在真机上扫了**全局**迭代上限（新增 `VL_ROD_ITERATION_CAP_GLOBAL` 作用域开关，
配合环境变量 `VL_ROD_ITERATION_CAP`），`SC_tressfx_rod_sdf_animated`、固定 4 子步、300 帧：

| 全局上限 | hair | 每 ms 仿真成本 | **失败 guide（中位数）** | maxIter |
| --- | --- | --- | --- | --- |
| 64（默认） | 83.3 ms | 5.00 | **4** | 24 |
| 16 | 79.9 | 4.80 | 5 | 22 |
| 8 | 74.9 | 4.49 | **11** | 19 |
| 4 | 70.7 | 4.24 | **168（8.4%）** | 16 |
| 2 | **23.4** | **1.40** | **1873（94%）** | 5 |

两个结论：

1. **原型的预言在真机上成立**：上限压到 4 时有 168 个 guide（8.4%）不收敛，
   压到 2 时 94% 不收敛——「4 次迭代就够」在我们的杆上确实不成立。
2. **但迭代次数根本不是成本杠杆**：从 64 压到 4（理论上省 15/16）只省 **15%**（83.3 → 70.7）。
   这与成本分解一致：时间在**每次迭代的固定工作（CR 占 67%）**上，而稳态平均迭代数本来就只有 1–2。
   同样，「只有少数 guide 跑满上限」那部分就是那 15%——前面已测过「2.6% 的迭代占 27% 时间」。

因此：**不要指望通过降少迭代数来提速**（上限 15%，且 168 个 guide 失败就是动态错了）；
真正的杠杆是降**单次迭代的固定成本**，而它的 67% 是 CR。
这一条同时给 V4 的方向加了一个反向保证：即使 V4 只能做到和现有完全一样多的迭代，
只要它把那 67% 的 6×6 耦合带消练变成局部 3×3，就能拿到大头。

### 顺带修复的实验缺陴

第一次扫描时只在着色器里定义了作用域宏、**没设环境变量 `VL_ROD_ITERATION_CAP`**，
而上限值是由 C++ 写进 `params.support.w` 的，于是四个配置跑出来完全平坦（83.5–84.5 ms）。
平坦本身就是一个信号：**开关没生效时不会报错，只会给出一个看上去合理的负结果**。
以后这类开关实验必须先验证开关真的生效（例如看 maxIter 是否随上限变化）。

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
