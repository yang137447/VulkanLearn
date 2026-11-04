# TressFX 5 源调度、步长、阻尼、VSP 与碰撞参数记录

> 性质：`rationale`（**不是契约**）。记录本轮对 UE TressFX 5 源实现的核对、数值推导、
> VL 当前差距与建议；技术细节可能随实现变化，不应被当作现行行为引用。
> 源参考：`D:\YYBWorkSpace\GitHub\UnrealEngine-TressFX5`，revision
> `61f8dd9b51b88d41937ef8d73618aac778642f41`；shader 行号引用
> `UE544_TressFX_Engine_Patch.patch`（同目录），C++ 行号引用插件
> `Engine/Plugins/AMD/TressFX/Source/TressFXCore/`。
> VL 侧：2026-10-24 已按本记录落地原生调度（GPU `hairSimulationPass` 与 CPU
> `GuideSimulation::StepFrame`），见 §6；下文标注“VL 当前”的段落若仍描述旧的
> 一帧一变量步，以 §6 的实施记录为准。

## 1. 源每帧如何推进物理时间

### 1.1 调用链与写入点

1. 游戏线程 `UTressFXComponent::TickComponent` 每 Tick 把帧增量写进实例：
   `TressFXGroupInstance->TimeStep = DeltaTime;`（`TressFXComponent.cpp:946-952`）。
   该组件只设置了 `PrimaryComponentTick.bCanEverTick = true`（`TressFXComponent.cpp:363`），
   没有 `bTickEvenWhenPaused`。
2. 渲染线程每帧进入 `ComputeTressFXSimulation`（`TressFXManager.cpp:447-448`，由
   `DeferredShadingRenderer.cpp` 的 bookmark 调用，patch:14565-14591；
   `CreateTressFXBookmarkParameters(TArray<FViewInfo>&)` 取 `Views[0]`，patch:20100-20103），
   首先 `++Instance->Guides.Frame;`（`TressFXCoreSimulation.cpp:933`）。
3. 关键的三行（`TressFXCoreSimulation.cpp:935-941`）：

   ```cpp
   float TimeStep = Instance->TimeStep;   // 935：局部量 = 本帧 DeltaTime
   ...
   Instance->TimeStep = 0.0125f;          // 941：本帧所有 pass 的 dt
   ```

4. 循环（`TressFXCoreSimulation.cpp:1013`、`1217-1218`）：

   ```cpp
   int32 NumLoops = 0;
   while (TimeStep > 0.f && NumLoops < 2)
   {
       // 1026 / 1096 / 1177 传入的是 Instance->TimeStep，即 0.0125
       // 各 pass 依次录制：Integration/GlobalShape -> StrandLevel -> [VSP] ->
       // [LocalShape] -> [LengthWind] -> [SDF Collision]
       TimeStep -= 0.0167f;   // 只扣局部剩余量
       ++NumLoops;
   }
   ```

因此：

- **solver step = 0.0125 s**：每轮 dispatch 给 shader 的 dt 恒等于 0.0125
  （`Instance->TimeStep` 在循环内不再被改写）。
- **循环扣除量 = 0.0167 s**：只决定这一帧 dispatch 1 次还是 2 次。
- 两者不是同一个量。把 0.0167 当 dt 会同时算错次数和每帧物理时间；用
  `DeltaTime` 反推 dt 也不对，因为循环根本没有把 `DeltaTime` 传给 pass。
- `TimeStep` 的初值是**上一帧** `TickComponent` 写的 `DeltaTime`。若某帧
  `TickComponent` 没跑（暂停、组件未 Tick），这里读到的是上一次的值。

### 1.2 0.016 clamp 作用在哪

`AddGuideIntegrationAndGlobalShapeConstraintsPass` 与 MT 变体在参数函数里做
`TimeStep > 0.016f ? 0.016f : TimeStep`（`TressFXCoreSimulation.cpp:152/271`），
Length/Wind 路径则直接赋值（`:760`）。但主路径传进去的已经是 0.0125，所以
**这个 clamp 在指南针主路径上永远不生效**。它只说明“若有人把帧增量直接当
Integration dt，会被截到 0.016”，不是源调度策略。VL 的 `maxDt = 0.016`
（`source/hair/meshAssetTypes.h` 邻近字段、`source/hair/tressfxDynamics.h:29`）
借用的正是这个数值，但语义已经变成“每帧唯一一步的上限”。

### 1.3 闭式表

记输入帧增量为 Δ（秒），`n(Δ)` 为 substep 次数：

- Δ ≤ 0（含 NaN）：n = 0；
- 0 < Δ ≤ 0.0167：n = 1；
- Δ > 0.0167：n = 2（Δ > 0.0334 也被 `NumLoops < 2` 截断）。

每帧物理时间 `T(Δ) = n·0.0125`，每真实秒倍率 `M = T/Δ`。D 取默认 0.035 时：

| 帧率 Hz | Δ (s) | n | 每步 dt (s) | 每帧物理时间 (s) | 倍率 M | 源每秒阻尼保留 |
| --- | --- | --- | --- | --- | --- | --- |
| 20 | 0.050000 | 2 | 0.0125 | 0.025 | 0.50× | 0.349938 |
| 30 | 0.033333 | 2 | 0.0125 | 0.025 | 0.75× | 0.207008 |
| 48 | 0.020833 | 2 | 0.0125 | 0.025 | 1.20× | 0.080460 |
| 60 | 0.016667 | 1 | 0.0125 | 0.0125 | 0.75× | 0.207008 |
| 120 | 0.008333 | 1 | 0.0125 | 0.0125 | 1.50× | 0.042852 |
| 144 | 0.006944 | 1 | 0.0125 | 0.0125 | 1.80× | 0.022823 |
| 240 | 0.004167 | 1 | 0.0125 | 0.0125 | 3.00× | 0.001836 |

边界行为（`tool/hair-tests/tressfxSourceScheduleTests.cpp` 逐条固定）：

- Δ = 0 → 0 次、物理时间 0；Δ < 0 与 NaN 同。
- Δ = 0.0167（恰好）→ 扣一次后剩余为 0，n = 1；只大一个 ulp 就 n = 2。
- Δ = 0.0334（2×0.0167）→ n = 2；再大也只 n = 2。
- 60 Hz 的 1/60 = 0.0166667 严格小于 0.0167，所以是 **1 步**：60 Hz 反而
  比 48 Hz 推进得少。倍率在 60 Hz 附近从 0.75 跳到 1.5（跨过 0.0167 时），
  这是源实现最反直觉的一处：帧率越高，每秒物理时间越多，最高到 3×。
- Δ 很大（卡顿 0.25 s）→ 仍然只有 2 步 × 0.0125 = 0.025 s，丢失的时间不补。

### 1.4 风帧语义

- `++Instance->Guides.Frame`（`:933`）每渲染帧 +1，**不随 substep 增加**。
  `SetWind` 的相位是 `pow(sin(frame*0.001),2)+0.5`（`:686`），所以同一帧两轮
  substep 的相位相同。
- 但 `AddGuideLengthConstraintsWindAndCollisionPass` 在每次录制时都会重新调用
  `SetWind(..., Guides.WindMagnitude * (0.3f + FMath::FRand()), ...)`
  （`:763`），即每个 substep 还各乘一个独立的随机幅值 0.3..1.3。于是同一帧的
  两轮风在相位上相同、在幅值上不同，且不可复现。
- `Guides.Frame` 在 reset 时也不归零（reset 只清三份位置缓冲，见 1.5）。

VL 当前（原生调度后）：`simulationFrame` 每个有 substep 的帧 +1
（`hairSimulationPass.cpp` 的 `Record`），同一帧的所有 substep 共用该值；
n == 0 的帧不递增，也不消费 reset/clock/history。`TressfxWindFrameModulation`
仍只有相位项（`tressfxSolverMath.h:135-139`），没有源每 substep 的
`0.3 + FRand()` 随机幅值；这是有意的可复现化，不是源行为。

### 1.5 暂停、恢复、reset、丢帧、mailbox

| 场景 | 源行为 | VL 当前 |
| --- | --- | --- |
| 暂停（PIE pause） | 没有显式 pause gate。`TickComponent` 默认不 Tick，`Instance->TimeStep` 停在最后一次非零 Δ；渲染 bookmark 仍每帧执行，于是按旧 Δ 继续以 0.0125/0.025 s 每帧步进（发丝继续动），直到恢复。若引擎在暂停时仍以 Δ=0 Tick 组件，则 n=0、冻结。**两种分支未在 UE 内实测**。 | `GuideSimulation::Step`/`StepFrame` 有 `paused` 早退（`tressfxDynamics.cpp`）；GPU 路径由 `TressfxFrameClock` 对同一 `(worldGeneration, frameIndex)` 返回 0，再由源调度得到 n=0，本帧不录制也不消费。 |
| 恢复 | 第一帧的 Δ 可能很大，但最多 2×0.0125 = 0.025 s；不补算暂停期间的时间。 | 恢复帧按源调度最多 2 步 × 0.0125 s，同样不补算；`maxDt` 不再截断。 |
| reset（`bResetPositions`） | 动画时间 < 0.033 或作者 reset 区间会置位（`TressFXComponent.cpp:1048-1065`）。Integration 在该帧**第一个 substep** 把 current/prev/prevPrev 全部写成 InitialPos 并 return（patch:8438-8444），但 host 仍会为该 substep 继续录制 VSP、LocalShape、LengthWind、SDF collision（`TressFXCoreSimulation.cpp:1137-1214`），之后才清标志（`:1135`）。因此 reset 帧留下的不是精确 rest，而是 rest 再经过一轮后续约束/风/碰撞；若 Δ > 0.0167，第二个 substep 从该状态继续积分 0.0125 s。若该帧 Δ ≤ 0，循环不执行，reset 被推迟到下一个有正 Δ 的帧。`Guides.Frame` 不归零。 | 已按源实施：reset 只在 n > 0 帧的首个 substep Integration 生效（独立 resetParams），同轮后续 pass 与 SDF collide 继续，n == 2 时第二轮正常；`simulationFrame` 每帧（含 reset 帧）+1。n == 0 时 reset 事件不消费，留到下一个正 Δ 帧。见 §6.2。 |
| 丢帧/卡顿 | 只补到 2 步（0.025 s），其余丢弃；不会累积 debt。 | 与源一致：最多 2 × 0.0125 s，其余丢弃；不再 clamp 到 0.016。 |
| mailbox 覆盖旧帧 | 渲染线程只读 `Instance->TimeStep` 的当前值。GT 多跑、RT 少跑时中间 Δ 被覆盖丢失；同一帧若再次进入 `ComputeTressFXSimulation`，读到的是上一次已改写的 0.0125（不是原 Δ），会再步一个 0.0125 s。 | `TressfxFrameClock::DeltaForFrame` 对已消费的 `(worldGeneration, frameIndex)` 返回 0（`tressfxFrameContext.h:91-111`），再由源调度得到 n=0：重复重绘不录制、不消费；mailbox latest-wins，跳过的帧不补算。这一条 VL 比源更严格。 |

## 2. 阻尼：每步、每帧、每真实秒

源公式：`Decay = exp(-DampingCoeff * TimeStep * 60.0)`，`Force * TimeStep²`
（patch:8383-8390）。作者字段 `damping` 直接就是 `DampingCoeff`
（`TressfxCoreSimulation.cpp:149`），不做 `-ln` 保留率变换。默认 0.035。

D = 0.035、每步 dt = 0.0125：

- 每步保留：`exp(-0.035*0.0125*60) = exp(-0.02625) = 0.974092`
- 两步帧（20/30/48 Hz）：每帧 `0.974092² = 0.948854`
- 每真实秒：`0.974092^(n·Hz)`，见表 1 最后一列。

旧 VL（原生调度前）一帧一步、`dt = clamp(Δ, 0, 0.016)`（下表保留为历史对照；
现行 `StepFrame`/GPU 已与源同表 1）：

| 帧率 | VL dt | VL 每帧保留 | VL 每秒保留 | 源每秒保留 |
| --- | --- | --- | --- | --- |
| 20 | 0.016 | 0.966958 | 0.510686 | 0.349938 |
| 30 | 0.016 | 0.966958 | 0.364948 | 0.207008 |
| 48 | 0.016 | 0.966958 | 0.199329 | 0.080460 |
| 60 | 0.016 | 0.966958 | 0.133187 | 0.207008 |
| 120 | 0.008333 | 0.982652 | 0.122456 | 0.042852 |
| 144 | 0.006944 | 0.985522 | 0.122456 | 0.022823 |
| 240 | 0.004167 | 0.991288 | 0.122456 | 0.001836 |

差异要点：

1. 60 Hz 下源每步 0.0125、VL 每步 0.016：VL 每帧多衰减约 0.75 个百分点的
   保留比例（0.974092 → 0.966958），每秒保留 0.133 对 0.207，指数 2.016 对
   1.575，约 1.28 倍。
2. 源的“每秒阻尼”由每秒步数决定：60/30 Hz 恰好都是 60 步/秒，所以两者
   每秒保留相同（0.207008）；120 Hz 变成 120 步/秒，衰减快得多。
3. VL 在 dt 未触顶（> 62.5 Hz）时每秒指数恒为 `0.035*60 = 2.1`，与帧率无关；
   在 20..60 Hz 则随帧率变化。也就是说，两者都“不帧率无关”，但曲线不同。

## 3. VSP：阈值量纲与作者曲线

### 3.1 阈值 1.4 m → 140 cm

- 源 `TressFXAssetSimulation.h:78-79`：`VSPAccelThresholdMax = 1.4f`，UI 范围
  `[1.0, 2000.0]`；位置在 shader 里统一乘 0.01 转米（patch:8425），所以 1.4 的
  单位是 **world m**。
- 比较量（patch:9925）：`Accel = length(pos_new[1] - 2*pos_old[1] + pos_old_old[1])`，
  严格大于阈值时 `VspCoeff = 1`（patch:9927）。它没有除 dt²，因此量纲是
  **长度**（二阶位置差幅值），不是 m/s²。字段名里的 `Accel` 是历史命名。
- 单位换算：1.4 m = 140 cm。VL 的 `TressfxDefaultVspAccelThresholdMaxCm = 140`
  （`tressfxSolverMath.h:41`）、CPU/资产默认值都用它
  （`tressfxDynamics.h:38`、`meshAssetTypes.h:97`）。`TressfxComputeStrandTransform`
  直接用 cm 位置与 cm 阈值比较（`tressfxSolverMath.h:283-304`），**量纲一致**；
  shader 同样直接比较 `params.shape.w`（`tressfxSimStrandLevel.comp:60-64`）。
- 旧记录里的 **5000 cm** 不是 1.4 m 的换算：5000 cm = 50 m，是 140 的 35.7 倍。
  `documents/contracts/tressfx-asset-import.md:308` 与 `:851` 仍写着“当前默认
  5000 cm”，与源码默认 140 矛盾（见“未决问题”）。5000 与 140 都只是长度，
  不能当加速度；同一个 140 cm 在 12.5 ms 步长下等效 896000 cm/s²，
  在 16 ms 下等效 546875 cm/s²，在 25 ms 下只剩 224000 cm/s²。

### 3.2 二阶差分不随 dt 缩放

对解析抛物线 `x(t) = ½at²`，二阶差分 `x(t) − 2x(t−dt) + x(t−2dt) = a·dt²`，
与 dt² 成正比。测试用 `a = 980 cm/s²` 构造：

- dt = 0.1 s → 9.8 cm；dt = 0.2 s → 39.2 cm（4 倍）；
- 匀速直线运动二阶差分恒为 0，不触发 VSP；
- 因此同一段真实运动在源 0.0125 s 步长与 0.025 s 步长下，触发难度相差 4 倍。
  这也意味着源阈值实际上是“按 0.0125 s 步长调出来的”。

### 3.3 作者曲线如何采样

`TressFXComponent.cpp:687-710`：

- VSP：`VSPStiffness` 数组长度 = `NumVerticesPerStrand`，第 i 项 =
  曲线在 `i / NumVerticesPerStrand` 处的值，i = 0..N−1。
- LocalShape：同样以 `i/N` 采样，再 `FMath::Min(value, 0.95f)`。
- 曲线默认键是 `(0,1)`、`(1,1)`（`TressFXAssetSimulation.cpp:33-34`，三次插值），
  作者可以改。

由此：

- x 轴根是 0、梢部是 `(N−1)/N < 1`，**曲线在 x=1 的值永远采不到**；
- VSP pass 用 `GET_SCALAR_ARRAY_ELEMENT(VSPStiffness, LocalVertexIndex)`
  （patch:10160），只对 `LocalVertexIndex >= NumVerticesFromRootNoSimulation`
  且 `VspCoeff < 0.9` 生效（patch:10153-10160）；
- LocalShape 用 `Stiffness[LocalVertexIndex]`，在 shader 里再乘 0.5
  （patch:8857）。

VL 当前（2026-10-24 阶段 4a）：数据路径与数学已对齐。`TressfxSimulationConfig`
新增可选 `vspStiffness` / `localShapeStiffness`，schema 限制 1..64 项、每项
`[0,1]`，导入期要求长度 == 该 TFX 的 `pointsPerStrand`；LocalShape 在加载期按
源 `min(value, 0.95)` clamp 并向 stderr 记录字段/索引/资产路径。CPU 与 GPU 共用
同一逐点选择：数组存在时取第 local 项，否则 VSP 走
`tressfxSolverMath.h` 的根前缀→梢部线性近似、LocalShape 走 `bendStiffness`
标量。params UBO 从 288 增到 800 bytes，两个 64 项数组各占 offset 288/544 的
16 个 vec4，`motion.w` 的 bit0/bit1 标记数组存在。`VspCoeff < 0.9` 的乘法条件
收敛到 `TressfxVspCoefficient`（shader 同名 helper），LocalShape 仍由
`TressfxLocalShapeDelta` 乘 0.5。

剩余差距与 2026-10-24 前相同，但性质从“数据面未迁移”变成“没有真实作者数据”：

1. 源作者面是曲线，VL 资产面是已按 `i/N` 采样后的逐顶点数组；把 Maya 曲线导成
   数组这一步尚未实现，正式 Female 资产保持空数组，运行时仍走两条 fallback。
2. 源 LocalShape 的上限在采样时 clamp；VL 在加载期 clamp 后缓存的就是 0.95，
   因此 GPU/CPU 看到的作者值与源一致，但原始 >0.95 的值已被记录后改写。
3. 根前缀内的 VSP 值不会被使用（VSP pass 跳过 root 前缀），与源一致；所以
   数组第 0..rootFixedPoints-1 项即使作者也只在数据身份中保留。

## 4. 碰撞响应

### 4.1 源 SDF-only 路径

`TressFXSimulationCollideStrandsVerticesWithSDF.usf`（patch:7795-8054）：

- 跳过 `LocalVertexIndex < NumVerticesFromRootBeforeSDFCollision`
  （patch:8001，取 `NumVerticesFromRootNoSimulation`，`TressFXCoreSimulation.cpp:845`）；
- 三线性采样有符号距离，`Distance > margin` 早退（patch:8010-8012）；
- 用 0.1×cell 的**前向差分**求梯度，`normalize` 后把点沿法线推
  `(margin - distance)`（patch:8045-8050）；
- **只写 current**；`GuidesDeformedPrevPositionBuffer` 的写入被注释掉
  （patch:8051）。因此没有摩擦、没有碰撞速度、没有 restitution；
- 每个 substep 只跑一次，没有迭代；`margin = CollisionMargin * CellSize.w`
  （patch:8010），默认 `SDFCollisionMargin = 0.05`（`TressFXMeshAsset.h:81`；
  实例默认 0.1，`TressFXMeshInstance.h:62`）；
- 位置与 SDF 已在同一空间（`VertexInSdfLocalSpace = Vertex`，patch:8005），
  没有额外换基。

### 4.2 VL 胶囊与 SDF 代理

原生调度后的 GPU 录制顺序（`hairSimulationPass.cpp` 的 `Record`）：

- 每帧一次、循环之前：SdfClear → SdfSplat（`sdfDomainValid` 时）；
- 每个 substep：Integration → StrandLevel → VSP（`vspCoefficient > 0` 才录）→
  LocalShape → LengthWind → **胶囊 Collision**（有 collider 才录）→
  SdfCollide（`sdfDomainValid` 时）。

重置帧不再例外：源 reset 之后同一轮仍跑后续碰撞，VL 现在也录 SDF clear/splat
与每个 substep 的 collide。

- 胶囊：`tressfxSimCollision.comp:31-79` 与 `tressfxCollision.h:97-112`。
  每个 collider 顺序应用、每个点最近点投影；**只写 current**，不改 previous；
  点恰在轴线上时用确定性垂直法线（+Y 或 cross(axis, ref)）。
- SDF：`tressfxSimSdfCollide.comp:41-60` 移植了源的投影量，
  只写 current，按 root→tip 串行，跳过不可移动点；有符号距离来自 VL 自建的
  逐帧代理 SDF（不是 .tfxmesh），margin 取 `sdf.range.x`。
- 重叠语义：胶囊按作者声明顺序串行，顺序决定重叠区结果；SDF 在胶囊**之后**
  执行，因此 SDF 可以把点重新推回胶囊内部，之后没有第二次胶囊投影。
  源只有 SDF 一条路径，不存在这个先后问题。
- 源的“每 substep 一次 SDF collide”已对齐；SDF 距离场本身仍每帧建一次。

### 4.3 分类：未移植 vs VL 新增

| 项 | 归属 |
| --- | --- |
| SDF 位置投影（有符号距离 + 梯度 + margin） | 源已有，VL 已移植（几何来源改为 proxy SDF） |
| 源逐帧 .tfxmesh body SDF 重建 | 源已有，VL 未移植（用 `collision.mesh` + proxy 替代） |
| 胶囊碰撞 | **源 guide 路径没有**，VL 新增（`tressfxSimCollision.comp:3-8` 注释明说） |
| 摩擦 / 碰撞速度 / 碰撞后速度清零 | 两边都没有；VL 也**没有**新增摩擦。任何“碰撞摩擦”只能标为 VL 后续新增，不能说成恢复源行为 |
| 多次碰撞迭代 | 两边都没有；源每 substep 一次，VL 已改为每 substep 一次 |
| 胶囊在 SDF 之后二次投影 | 源没有，VL 也没有（顺序固定为胶囊→SDF） |

## 5. VL 当前差距汇总

| 维度 | 源 | VL 当前 | 影响 |
| --- | --- | --- | --- |
| 步长 | 固定 0.0125 s | 原生调度固定 0.0125 s/substep；`maxDt` 只服务旧 `Step` | 已对齐；资产 `maxDt` 不再影响运行时 |
| 循环 | 0/1/2 步，按 0.0167 决定 | 同一 `TressfxSourceScheduleForDelta`：0/1/2 步 | 已对齐（30/48 Hz 2 步，60 Hz 1 步） |
| 每帧物理时间 | 0 / 0.0125 / 0.025 | 0 / 0.0125 / 0.025 | 已对齐；高帧率仍可到 3× |
| 风帧 | Frame 每帧 +1；每 substep 另有 0.3+FRand 随机幅值 | 每个有 substep 的帧 +1，无随机项 | 相位已对齐；随机幅值仍是有意省略 |
| 阻尼 | `exp(-D*0.0125*60)` 每步 | 同一公式、同一 0.0125 步长 | 已对齐（60 Hz 每秒 0.207） |
| VSP 阈值 | 1.4 m | 140 cm（同量纲） | 一致；旧文档 5000 cm 是缺陷 |
| VSP 曲线 | 逐顶点作者曲线 `i/N` | 可选逐顶点数组已对齐；空数组时根→梢线性近似 | 数据路径/数学一致；正式资产无曲线数据仍走回退 |
| LocalShape 曲线 | 逐顶点作者曲线 `i/N`，clamp 0.95 | 可选逐顶点数组已对齐，加载期 clamp 0.95；空数组时单一标量 | 数据路径/数学一致；正式资产无曲线数据仍走回退 |
| 碰撞 | SDF-only，只写 current，无摩擦 | 胶囊（新增）+ SDF，只写 current，无摩擦 | 顺序与几何来源不同，都没有摩擦 |
| 暂停/重绘 | 重复消费旧 Δ；无 pause gate | 同帧不二次步进；有 paused 标志 | VL 更严格，非源等价 |

## 6. 已实施的原生调度（2026-10-24）

本节替换原先“建议、本轮不改运行时”的段落，实现以代码为准。
`source/hair/tressfxSourceSchedule.h` 的 `PlanGuideSubsteps` 是 GPU 与 CPU 共用的
纯计划函数；它只由冻结帧增量、reset 事件与 placement 变化决定本帧结构。

### 6.1 每帧步进

- 每帧从 `TressfxFrameClock::DeltaForFrame(worldGeneration, frameIndex, frozenDelta)`
  取冻结帧增量，再由 `TressfxSourceScheduleForDelta` 得到 n。已消费的重绘帧
  返回 0，因此 n = 0。
- GPU（`hairSimulationPass.cpp` 的 `Record`）：
  - n == 0：不录制任何 dispatch，不 Commit `history`、不 `clock.Consume`、
    不 Commit `modelHistory`，也不记录缺失 pose；reset 事件原样保留到下一个
    正 delta 帧。
  - n > 0：每帧一个 `simulationFrame`（含 reset 帧），整帧只 +1；同一帧所有
    substep 共用该风相位与同一组 wind/gravity 参数。
  - `simulationFrame += 1`、`history.Commit`、`clock.Consume`、
    `modelHistory.Commit` 都在整帧最后各执行一次。
  - 每个 substep 的 `controls.x = 0.0125`；`maxDt` 不再参与 GPU 路径。
- CPU（`GuideSimulation::StepFrame`）：同一 n，循环求解体但固定 0.0125；
  `simulationTime += 0.0125` 每 substep 一次，`simulationFrame` 每帧一次。
  `Step` 保留为旧变量步长入口，仍按 `maxDt` clamp，现有测试继续覆盖它。
- `RendererResourceCache::AdvanceTressfxSimulations` 改走 `StepFrame`；
  `paused` 早退与显式 `Reset()` 语义不变。

### 6.2 reset 首轮

源 `bResetPositions` 只作用于首轮 Integration，同轮后续 pass 仍执行
（`TressFXCoreSimulation.cpp:1135-1214`）。VL 现在按同一顺序：

1. substep 0 的 Integration 绑定**独立的 resetParams UBO + descriptor set**：
   `motion.y = 1`、`controls.x = 0`。正常 `params[slot]` 永远 `motion.y = 0`、
   `controls.x = 0.0125`。不能“先写 reset 再覆盖同一 buffer”，因为命令缓冲
   执行时只有最后一次写入可见。
2. 同一 substep 继续录 StrandLevel / [VSP] / LocalShape / LengthWind /
   [胶囊 Collision] / [SdfCollide]。
3. n == 2 时第二轮用正常参数再录一整轮。
4. `ResetPositions` 日志每帧一次，字段顺序保持 `capture_hair_physics.py` 的
   正则兼容，末尾追加 `substeps=`。

reset 与 placement 换基互斥，同一份 resetParams 也承载“非 reset 且 placement
变化”的首个 Integration 换基矩阵，因此一个 buffer 够用；两者的初始化都在
整帧录制之前完成。

### 6.3 SDF 与胶囊顺序

- SdfClear / SdfSplat 每帧一次，放在 substep 循环之前；
- 每个 substep：LengthWind → 胶囊 Collision → SdfCollide；
- 胶囊 SSBO、SDF palette、SDF 域参数每帧写一次（循环之前）；
- reset 帧不再跳过碰撞：源 reset 之后同一轮的 SDF collide 仍执行，VL 现在一致。

### 6.4 maxDt 与 reset 边界

- `maxDt` 仍保留在 `meshAssetTypes.h` 与 `SimulationConfig`，仅为资产兼容与旧
  `GuideSimulation::Step`；GPU 原生路径与 `StepFrame` 不读取它。资产校验仍限制
  `(0, 0.016]`。
- reset 只在本帧 n > 0 时生效；n == 0 的 reset 不会被记录、Commit 或清标志，
  下一个正 delta 帧会再次 Plan 出同一原因。
- 卡顿帧最多 2 × 0.0125 s，丢失时间不补；与源一致。

### 6.5 与源仍有意的差异

1. n == 0 的帧不递增 `simulationFrame`：源在循环前无条件 `++Guides.Frame`。
   VL 对“已消费重绘帧/暂停帧”保持零副作用，否则恢复时风相位会凭空前进。
   VL 在整帧最后 +1（首帧相位用 0），源在循环前 +1（首帧用 1）：这是固定的
   一帧相位偏移，不随 substep 累积，也不改变同一帧内相位相同。
2. 每帧一次的 Commit/Consume 由 VL 的冻结帧时钟提供，源 mailbox 没有这层保护。
3. placement 换基是 VL 新增语义，只在首个 substep 的 Integration 应用一次。
4. 源每个 substep 还为风乘 `0.3 + FRand()`；VL 仍只用确定性相位。

## 7. 测试清单

`tool/hair-tests/tressfxSourceScheduleTests.cpp`（已注册进
`tool/hair-tests/CMakeLists.txt`）：

- `TressfxSourceSchedule.SubstepsPhysicalTimeAndRateMultiplierPerFrameRate`：
  20/30/48/60/120/144/240 Hz 的 n、每帧物理时间、倍率；物理时间只能是
  0/0.0125/0.025。
- `TressfxSourceSchedule.NonPositiveDeltaAdvancesNothing`：0、−0、负值、NaN。
- `TressfxSourceSchedule.FirstDeductionBoundaryKeepsOneStep`：0.0167 整 → 1；
  加一个 ulp → 2；1/60 < 0.0167 → 1。
- `TressfxSourceSchedule.SecondDeductionBoundaryAndHardCap`：2×0.0167 与
  0.0334 → 2；0.04/0.1/0.25/10 → 仍 2。
- `TressfxSourceSchedule.SolverStepIsConstantAndNotTheDeductionAmount`：
  0.0125 ≠ 0.0167，且每个 substep 的 dt 恒为 0.0125。
- `TressfxSourceSchedule.IntegrationClampNeverBindsOnSourcePath`：
  0.0125 < 0.016；30 Hz 的 0.0333 若直传会被 0.016 截断。
- `TressfxSourceSchedule.WindPhaseAdvancesOncePerRenderFrameAcrossSubsteps`：
  Frame 每渲染帧 +1，与 substep 数无关。
- `TressfxSourceSchedule.PlanGuideSubstepsKeepsResetInTheFirstSubstep`：reset 只在
  n 个 substep 的首轮 Integration 生效；同轮后续约束与第二轮都继续。
- `TressfxSourceSchedule.PlanGuideSubstepsDefersResetWhenDeltaHasNoSubsteps`：
  0/负/NaN 时无工作、不消费 reset；下一个正 delta 帧仍返回同一事件。
- `TressfxSourceSchedule.PlanGuideSubstepsRebasesHistoryOnlyInTheFirstSubstep`：
  placement 换基只在首个 substep 使用独立参数；reset 优先，两者互斥。
- `TressfxSourceDamping.PerStepMatchesSourceFormula`：每步 `exp(-D*0.0125*60)`，
  双步是单步平方。
- `TressfxSourceDamping.PerSecondRetentionFollowsStepCountNotFrameRate`：
  60/30 Hz 都是 60 步/秒 → 0.207008；120 Hz 0.042852；20 Hz 0.349938。
- `TressfxSourceDamping.VlVariableStepRetainsLessPerSecondAt60Hz`：
  VL 60 Hz 0.133187 vs 源 0.207008；并固定 20/30/48 Hz 的 VL 值。
- `TressfxSourceVsp.ThresholdIsALengthNotAnAcceleration`：1.4 m→140 cm；
  5000 与 140 的 35.7 倍关系；140 cm 在 12.5/16 ms 下的等效加速度；
  5000 cm 的等效加速度。
- `TressfxSourceVsp.PseudoAccelerationScalesWithStepSquared`：解析抛物线
  二阶差分 = a·dt²（4 倍关系）；匀速为 0；严格 `>` 边界；25 ms 下等效
  加速度是 12.5 ms 的 1/4。
- `TressfxSourceVsp.CurveSamplingUsesIOverNNotEndpoints`：N=8 时坐标
  0/0.125/0.875，梢部 < 1；与 VL 线性近似的根/梢及中间点差异。
- `TressfxSourceVsp.LocalShapeCurveIsClampedToPointNineFive`：0.95 上限。
- `TressfxCapsuleCollision.ProjectionDoesNotRewriteVelocityHistory`：
  `GuideSimulation` 步进后 current 被推到球面，previous/previousPrevious
  保持积分前值。
- `TressfxCapsuleCollision.ProjectionIsIdempotent`：二次投影不移动、返回 false。
- `TressfxCapsuleCollision.OverlapResultDependsOnCapsuleOrder`：两个重叠球，
  顺序 A→B 与 B→A 结果不同，且都落在两球外。

`tool/hair-tests/tressfxDynamicsTests.cpp`（StepFrame 运行时路径）：

- `TressfxDynamics.StepFrameAdvancesSourcePhysicalTimePerFrameRate`：
  30/48/60/120/144/240 Hz 的每帧物理时间与 substep 数；Frame 每帧 +1。
- `TressfxDynamics.StepFrameMatchesTwoFixedStepsWithoutWind`：无风时 30 Hz 一帧
  等价于两次 0.0125 s 步进，但 Frame 只 +1（旧 Step 是 +2）。
- `TressfxDynamics.StepFrameIgnoresMaxDtAndCapsAtTwoSubsteps`：`maxDt=0.005`
  仍按 0.0125 推进；1 s 卡顿仍只 2 步。
- `TressfxDynamics.StepFrameConsumesZeroDeltaWithoutAdvancingOrUnpausing`：
  0/负/NaN 不推进；暂停冻结；显式 Reset 仍清零。
- `TressfxDynamics.StepFrameAdvancesWindPhaseOncePerFrameNotPerSubstep`：
  30 Hz 双 substep 与 60 Hz 单 substep 都只 +1，0 步帧不消费。

`tool/hair-tests/tressfxSimulationHistoryTests.cpp`：

- `TressfxSimulationHistory.ResetEventSurvivesZeroDeltaFramesUntilSourceSubstepsRun`：
  delta = 0 时不 Commit、reset 事件保留；下一正 delta 帧首轮 reset 且同轮后续
  pass 继续；Commit 后恢复 Simulate 且不再使用独立参数。

阶段 4a 新增（`hair_asset_tests` 全量 238 项，2026-10-24）：

- `TressfxSolverMath.VspStiffnessUsesAuthoredArrayAtEveryPoint`：数组端点/中间点，
  未覆盖的 local 回退旧线性值。
- `TressfxSolverMath.VspCoefficientOnlyMultipliesCurveBelowPointNine`：0.8 乘曲线、
  0.9 与 1.0 不乘；无数组时乘线性 fallback。
- `TressfxSolverMath.LocalShapeStiffnessUsesAuthoredArrayPerPoint`：逐点选择与标量回退。
- `TressfxSolverMath.LocalShapeDeltaScalesLinearlyWithPerPointStiffness`：0.5 仍由
  `TressfxLocalShapeDelta` 施加。
- `TressfxDynamics.AuthoredCurvesPackIntoStd140AndSetMotionFlags`：sizeof 800、
  offset 288/544、`motion.w` bit0/bit1 与 64 项 `[local/4][local%4]` 打包。
- `TressfxDynamics.AuthoredLocalShapeCurveWorksWithZeroScalarStiffness`：标量为 0
  时数组仍驱动 LocalShape。
- `TressfxDescriptor.SimulationStiffnessCurvesAreParsedAndPartOfIdentity`：解析、
  1-ulp 差异与曲线存在性进入缓存身份。
- `TressfxDescriptor.SimulationStiffnessCurvesRejectInvalidEntries`：范围、空数组、
  65 项上限、非数字项。
- `TressfxDescriptor.LocalShapeCurveValuesAboveLimitAreClampedAndRecorded`：clamp
  0.95 且 stderr 记录字段/索引/资产路径。
- `TressfxDescriptor.SimulationRejectsUnknownCurveKeys`：拼错字段不再静默回退。
- `TressfxImport.CarriesMatchingAuthoredCurvesIntoSectionConfig`：长度匹配时数组
  进入 section 配置。
- `TressfxImport.RejectsAuthoredCurveLengthMismatchAgainstTfxPointsPerStrand`：
  8 项曲线 + 4 points/strand 导入期硬错误。

## 8. 未核实项与风险

1. **暂停分支**：源没有 pause gate；暂停时 `TickComponent` 是否以 Δ=0 调用
   决定“冻结”还是“按旧 Δ 继续步进”。本轮禁止启动 UE，未实测，两种分支都
   写进了 1.5 表。
2. **每帧一次的边界**：bookmark 从 `DeferredShadingRenderer::Render` 调，
   取 `Views[0]`；编辑器预览路径（patch:15748/15882）可能额外再跑一次。
   多视图/编辑器下是否一帧多次步进未实测。
3. **资产作者值**：在 `VukanLearnResources` 下没有搜到
   `vspAccelThresholdMax` / `vspCoefficient` 的作者值，因此“资产默认”只能
   确认到代码默认 140；实际资产可能用别的值（未加载核对）。阶段 4a 后仍没有
   `vspStiffness` / `localShapeStiffness` 的真实作者数据，正式 Female 资产两条
   曲线都为空，运行路径仍是 fallback；真实 Maya/UE 曲线采样值未导出。
4. **契约已同步（已解决）**：`documents/contracts/tressfx-asset-import.md` 的
   VSP 阈值默认值已是 140 cm（不再是 5000）；阶段 4a 又把两条作者曲线的字段、
   长度/clamp 校验、800 B UBO 布局与显式差异补进该契约。
5. **SDF margin**：源默认 0.05 m（mesh asset）；VL 的 margin 来自 proxy
   `sdf.range.x`，作者值未核对，两者数值是否可比未验证。
6. **`Instance->TimeStep` 的其他消费者**：本轮只核对了 TressFXCore 插件内
   的引用；插值/编辑器模块是否读它未逐一确认。
7. **0.0334 的浮点边界**：0.0334f 与 2×0.0167f 不保证逐位相等；测试对
   “0.0334 → 2 步”是稳健的（第一轮后必然仍 > 0），但不要把它当成
   “恰好 2×扣除量”的精确证明；精确边界用 `2.0f * TressfxSourceLoopDeductionSeconds`。
8. **GPU 原生调度已做启动/帧率烟测，仍无逐点数值验收**：Debug 启动日志
   （shader `failed=0`、`ResetPositions ... substeps=`、无 VUID、stderr 0）与
   Release 30/60/120 Hz 外部采集通过；30 Hz 的 hair GPU 中位从 1.669 ms 升到
   2.945 ms（n=2 的两轮 dispatch），60 Hz 实测 59.6 Hz 也跨到 2 步（2.04 ms）。
   未逐点读回 reset 后位置，也未测 48 Hz 的翻倍耗时；这些留给 hair trace。
9. **契约已由主线程同步**（2026-10-24）：`tressfx-asset-import.md` 的 maxDt
   行、reset 首轮、SDF clear/splat/collide 顺序、步进伪代码与 reset 日志字段
   已改为“n=0/1/2 × 0.0125 s、maxDt 仅兼容旧 Step”；VSP 默认 5000 也已改为
   140 cm。阶段 4a 后 `hair_asset_tests` 实际 238 项；`testing.md` 的 hair 计数
   由主线程统一同步。
