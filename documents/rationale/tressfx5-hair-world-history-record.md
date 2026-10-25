# TressFX 5 发丝世界位置历史与角色运动惯性记录

状态：2026-10-24 的源码审计、数学推导与模块测试补充；非实现契约。
参考源固定为 `D:\YYBWorkSpace\GitHub\UnrealEngine-TressFX5`，revision
`61f8dd9b51b88d41937ef8d73618aac778642f41`。行号分两类：

- `UE544_TressFX_Engine_Patch.patch:N-M`：仓库根 patch 中的源 shader；
- `Engine/Plugins/AMD/TressFX/Source/TressFXCore/...:N`：源 host / 组件。

本轮不改步长、VSP、碰撞求解与渲染；只回答“history 在什么空间/单位、placement
变化时是否与源世界空间 solver 逐点等价”，并为 VL 的局部表示补独立 oracle。

## 1. 源行为

### 1.1 guide 蒙皮：`LocalToWorld` 进入，随后整体转米

`UE544_TressFX_Engine_Patch.patch:8273-8312` 的 `ApplyVertexBoneSkinning`：

1. 8278-8300：按权重混合 `BoneMatricesBuffer`。注释明确写的是
   “Interpolate world space bone matrices”，但这一步实际是
   **component 空间的 ReferenceToLocal 组合**，还没有乘 Actor 变换。
2. 8303-8305：`float4x4 LocalToWorld = DFDemote(PrimitiveData.LocalToWorld);`
   `BoneMatrix = mul(BoneMatrix, LocalToWorld);`——这是 guide 根点/整点的
   **世界化**入口，`LocalToWorld` 是 TressFX 组件所属 skeletal mesh primitive
   的变换（`TressFXComponent.cpp:298` 用 `GetLocalToWorld()` 写入 primitive）。
3. 8307-8308：`BoneQuat` 从同一矩阵分解，`NewVertexPos = mul(float4(VertexPos,1), BoneMatrix).xyz`。
4. 8424-8425：`InitialPos[GIndex] = ApplyVertexBoneSkinning(...)` 后
   `InitialPos[GIndex] *= 0.01f;`。因此 `InitialPos` 是 **世界空间、米**。

MT（Patch 内的多三角根绑定）分支在 8520-8572：`ApplyBlendShapeSkinning`
先把 guide 顶点投到根三角局部（8483-8501），再用变形后的根三角回到
component 空间（8544-8561），最后 8565-8566 同样乘 `PrimitiveData.LocalToWorld`；
8587-8588 再 `*= 0.01f`。结论相同：**蒙皮目标始终是世界空间，最后统一转米**。

### 1.2 三份位置历史：世界米，Verlet 隐式速度

`UE544_TressFX_Engine_Patch.patch:8383-8400`：

- 8390：`OutputPos.xyz = CurPosition.xyz + Decay*(CurPosition.xyz - OldPosition.xyz) + Force.xyz*TimeStep*TimeStep;`
  速度不是独立状态，而是 `(CurPosition - OldPosition)`；`Decay = exp(-DampingCoeff*TimeStep*60)`。
- 8395-8400 `UpdateFinalVertexPositions`：`PrevPrev <- Prev`、`Prev <- OldPosition`、
  `Current <- NewPosition`，即三份都是世界米位置。

`...:8419-8467` 主 pass：

- 8419 `RestPos` 来自 local/component rest buffer；
- 8423-8425 计算世界米 `InitialPos`；
- 8428 `CurrentPos = GuidesDeformedPositionBuffer`（上一帧结果）；
- 8429-8430 根前缀把 `CurrentPos.w = 0`；
- 8433 `OldPos = GuidesDeformedPrevPositionBuffer`；
- 8438-8444 `ResetPositions != 0` 时**三份 buffer 全部写 `float4(InitialPos, RestPos.w)` 并立即 return**，
  旧速度被清零；
- 8446-8451 可移动点积分，不可移动点写 `float4(InitialPos,0)`；
- 8453-8464 global shape 只作用于可移动点；
- 8466-8467 统一提交三份历史。

所以：

- **current / previous / prevPrev 都是世界空间、单位米**，且不是“局部位置 + 一个刚性变换”的分解。
- `w` 在源里是 inverse mass / movable 标记，**不是齐次坐标**；三份历史换基必须用 `w=1` 做点变换。

### 1.3 SDF 代理与渲染

- SDF 碰撞网格也做世界化：`...:9717-9771` 的 `SDFMeshSkinning`，9736-9761 混合骨骼，
  9763-9766 乘 `PrimitiveData.LocalToWorld`，9768 写世界 cm；`SDFMeshSkinningNoBone`
  （9804-9846）直接用静态位置，没有世界变换。
- SDF 域/碰撞统一转米在 splat 侧：`...:9148-9150` 三个三角形顶点 `*0.01f`；
  `...:9556-9560` 的 box min/max 也 `*0.01f`。因此 SDF 与 guide 在同一个世界米空间相交。
- 渲染侧**不再乘一次 `LocalToWorld`**：hair follow 在 `...:6868` 把
  `GuidesDeformedPositionBuffer * 100.f` 从米转回世界厘米，渲染 VF 直接把它当
  translated world：`...:11171-11178`、`...:13984-13987`。
  `...:11019` 的 `PreviousLocalToWorld` 只进材质 `FMaterialVertexParameters::PrevFrameLocalToWorld`；
  `...:11186-11199` 相关代码是注释掉的。渲染另有一个 `TressFXVF.PreviousPositionBuffer`
  （`...:10760`、`...:11192`；host 绑定见
  `TressFXVertexFactory.cpp:43,64,77`、`TressFXCoreRendering.cpp:696-697`），
  由 `TressFXManager.cpp:493-501` 在非暂停世界逐帧交换 current/previous，用于材质上一帧
  世界位置/运动矢量，**不回灌 solver**。

### 1.4 host 调度（只作对照，不在本轮改动范围）

- `TressFXCoreSimulation.cpp:933` 每帧 `++Instance->Guides.Frame`；
- `:935` 取 `Instance->TimeStep`，`:941` 立即把下一帧 `TimeStep` 设为 `0.0125f`；
- `:948-957` 取 `ParentComponent` 的 `GetComponentTransform().ToMatrixWithScale()` 作 `ComponentToWorld`；
  无模拟时 `:963-972` 用它走 rest→deformed；
- `:986-988` 用 `MeshObject->GetReferenceToLocalMatrices()` 上传骨骼；
- `:1012-1019` `while (TimeStep > 0 && NumLoops < 2)`，`:1026`/`:1096` 传当前 `Instance->TimeStep`，
  `:1135` 第一次循环后清 `bResetPositions`，`:1217` 每轮 `TimeStep -= 0.0167f`；
- `:152`/`:271` 的 Integration 参数另外 clamp 到 `0.016f`；
- 组件在 `TressFXComponent.cpp:1046-1050` 动画起始、`:1055-1066` 复位区间设置
  `bResetPositions`。

这些说明源同一组件帧内可能最多两个固定/变量混合步。VL 在 2026-10-24 后已按同一
n=0/1/2 × 0.0125 s 调度（见
[时间/参数记录](tressfx5-hair-timing-params-record.md) §6）；本文仍只讨论
“给定同一次 placement 变化”的空间换基，不把调度差异混入空间等价结论。

### 1.5 源是否使用 `PreviousLocalToWorld` 或显式速度

- **solver 不读 `PreviousLocalToWorld`**：patch 中唯一的非注释使用是
  `...:11019` 的材质参数，以及 `...:11186-11199` 的注释代码。
- **solver 没有显式速度 buffer**：速度就是 `CurPosition - OldPosition`
  （`...:8390`），历史由 `UpdateFinalVertexPositions`（`...:8395-8400`）推进。
- 渲染的 `PreviousPositionBuffer` 是另一条链路（上一帧世界位置，供材质/运动矢量），
  不参与积分、碰撞或 reset。

## 2. 物理含义

- `current` 是这一帧求解结束后的世界位置；`previous` 是上一帧结束后的世界位置；
  `prevPrev` 是上上帧结束后的世界位置。
- 单步位移 `current - previous` 就是 1 步的世界速度（乘以 `1/dt`）；阻尼
  `Decay = exp(-DampingCoeff*dt*60)` 乘在这个位移上，重力/风则作为 `force*dt²`
  叠加。因此“角色瞬移但发丝留在原地”表现为：根点钉到新的蒙皮位置，自由点的
  `current/previous` 世界位置不变，下一步 `current - previous` 仍保持旧速度。
- Reset 把三份历史都写成当前蒙皮 rest，等价于清空全部惯性；不是只钉根点。

## 3. VL 实现逐项对照

VL 的 SSBO 存的是 **hair 模型局部坐标（厘米）**，渲染时 vertex factory 再乘
`uboM.model`（`shader/glsl/engine/tressfxVertexFactory.glsl:14-32`、
`shader/glsl/engine/passTemplate/base.vert.glsl:42-51`）。这与源“SSBO 存世界、
渲染不再乘 model”是同一物理量的两种表示。

| 语义 | 源 | VL | 结论 |
| --- | --- | --- | --- |
| root/skinning 目标 | `ApplyVertexBoneSkinning` 世界化后 `*0.01`（patch 8303-8308、8424-8425） | `TressfxFrameContext` 存 `hairModel⁻¹*sourceModel*palette`（`source/hair/tressfxFrameContext.h:37-44`），shader 算 `skinning*rest`（`shader/glsl/hair/tressfxSimIntegration.comp:43`） | 世界目标和源一致 |
| 三份历史 | 世界米 | hair 局部厘米，每帧用 `H = currentModel⁻¹*lastModel` 换基（`source/hair/tressfxFrameContext.h:71-77`；`shader/glsl/hair/tressfxSimIntegration.comp:63-79`） | 表示不同，世界位置应逐点一致 |
| 积分 | 世界 `cur + Decay*(cur-old) + g_world*dt²`（patch 8390） | 局部 `cur + Decay*(cur-old) + (M₃⁻¹g_world)*dt²`（`tressfxSimIntegration.comp:84-89`） | 乘回 `M` 后严格相等（见 §4） |
| 重力/风 | 世界向量 | 世界生成后乘 `M₃⁻¹`（`source/hair/tressfxSimulationParams.h:85-98`） | 力/方向协变；点级精确 |
| Reset | 三份=`InitialPos`（世界米），return（patch 8438-8444）；host 仍在该 substep 录制 VSP/LocalShape/LengthWind/SDF（`TressFXCoreSimulation.cpp:1137-1214`） | `motion.y` 分支三份=`initialPos`（局部），return（`tressfxSimIntegration.comp:50-59`）；随后 `continue`，不跑后续 pass | 三份历史的写入等价；reset 帧的**最终姿态**不同：源会再受一轮约束/风/碰撞，VL 停在纯 rest |
| 暂停但 placement 变化 | 源无独立“rebase”；世界表示下位置自然不动 | `motion.z` 只换基三份并 return（`tressfxSimIntegration.comp:67-76`） | 表示坐标变、世界状态不变 |
| 丢 pose | 源 `MeshObject` 无效时直接 return / 走 rest 路径 | 保留上次 `frameContext`；placement 变化时把缓存矩阵也乘 `H`（`source/hair/hairSimulationPass.cpp:660-668`） | 缓存、SSBO、shader 同步换基 |
| 首帧 | 初值来自 rest→world 或 reset | `TressfxModelHistory` 未初始化时 `HasPlacementChange` 必为 false（`source/hair/tressfxFrameContext.h:66-77`） | 首帧不会把初始 placement 当移动 |
| 时钟 | 源按组件 `TimeStep` | `TressfxFrameClock` 冻结 GT 帧（`source/hair/tressfxFrameContext.h:94-117`） | 语义独立，本轮不改 |
| 提交顺序 | 源每轮 `bResetPositions` 在首轮后清除（`TressFXCoreSimulation.cpp:1135`） | Integration 录制后统一 `modelHistory.Commit`（`source/hair/hairSimulationPass.cpp:728`），Reset/rebase 也会走到 | Reset/rebase 正确提交 |

实现中的关键点已逐项核对：

- `modelHistory.Commit` 在 `Integration` 之后、`rebaseOnly`/`Reset` 的 `continue`
  之前（`hairSimulationPass.cpp:725-732`、`:741-751`），所以三种分支都提交当前 model。
- `Reset` 时 `historyToCurrentLocal = Identity`（`:657-659`），而 shader 在
  `motion.y` 分支先于任何换基就写满三份并 return，故 Identity 与源语义一致。
- `rebaseOnly` 不更新 collider/SDF，也不录制 collision/SDF pass（`:672`、`:711-714`、
  `:731-732`）。恢复首帧会先 `UpdateTressfxFrameContext`，再在 `:672-694` 重写
  capsule/SDF palette，`:711-714` 重建 SDF 域；因此不会拿暂停期的旧 collider 做碰撞。
  若恢复首帧没有 pose，则 `:660-668` 已用同一 `H` 换基缓存矩阵，collider/SDF 仍与
  SSBO 同空间。
- 丢 pose 时缓存 `skinningMatrix` 与 `jointMatrices` 都用 `H` 左乘（`:664-667`），
  与 shader 对三份位置的 `H` 和 `w=1` 换基（`tressfxSimIntegration.comp:65-66,71`）一致。
- shader 三处换基都显式 `vec4(xyz, 1.0)`，`w` 只作为 movable 标记保留/写回
  （`tressfxSimIntegration.comp:54-57,65-79,94`），没有把 `w=0` 当齐次坐标。
- 首帧 `lastModel=Identity` 不会触发换基：`HasPlacementChange` 先看 `initialized`
  （`tressfxFrameContext.h:66-68`）；首帧 SSBO 初值就是局部 rest
  （`source/renderableObject.cpp:344-349`），随后首帧计划为 Reset（InitialState/FirstPose）。

## 4. 等价性推导与适用边界

### 4.1 逐点等价推导

记：

- `M_t`：hair 对象在帧 `t` 的 model（`hairDraw.model`）；
- `C_t`：源角色对象在帧 `t` 的 model（`sourceDraw->model`）；
- `B_t`：源 `bindToPoseMatrices` 中根关节的矩阵；
- 源的世界蒙皮目标 `R_t = C_t B_t r`（`r` 为 rest/component 空间点）；
- VL 的局部目标 `S_t = M_t⁻¹ C_t B_t`，故 `M_t S_t r = R_t`：**根点目标逐点相同**，
  与 pivot/旋转/平移无关。
- SSBO 表示 `p_t = M_t⁻¹ x_t`，三份历史都是这个表示。

换基（placement 变化）：

```
p ← H p,  H = M_t⁻¹ M_{t-1}
⇒ M_t H p = M_{t-1} p = x_{t-1}
```

因此 current/previous/prevPrev 的世界位置都保持不变；世界速度
`x_cur - x_prev = M_t H (p_cur - p_prev) = M_{t-1}(p_cur - p_prev)` 也保持不变。

积分：

```
p_new = p_cur + Decay (p_cur - p_prev) + (M_t,3⁻¹ g_world) dt² + ...
M_t p_new = x_cur + Decay (x_cur - x_prev) + (M_t,3 M_t,3⁻¹) g_world dt²
         = x_cur + Decay (x_cur - x_prev) + g_world dt²
```

`M_t,3 M_t,3⁻¹ = I` 对任何**可逆仿射** `M_t` 成立，所以平移、绕任意 pivot 的旋转、
组合，甚至非均匀缩放/剪切，在“无度量约束”的 Verlet + 重力/风 + global shape
上都是逐点精确的。global shape 是 `p += λ(target - p)` 的仿射线性插值，
乘 `M_t` 后仍是世界空间同系数插值；速度与历史位移同理由 `M_t` 线性映射。

### 4.2 边界

- **hair model 与 source model 不一致**：源 shader 的 `PrimitiveData.LocalToWorld`
  属于 TressFX primitive（`TressFXCoreSimulation.cpp:1017` 的 `PrimitiveSceneProxy`），
  骨骼矩阵是 skeletal mesh 的 reference-to-local；源默认两者共享同一组件变换
  （`TressFXComponent.cpp:298` 把组件 `GetLocalToWorld()` 写进 primitive）。
  VL 的根目标取 `M_t S_t r = C_t B_t r`（用 source draw 的 model），只有在
  `M_t` 与 `C_t` 相同（常见挂接）时才与源该假设逐点一致。若两者是不同 placement，
  VL 选择“让 hair 渲染随 hairModel、蒙皮目标随 sourceModel”，与源在该配置下的
  行为没有源证据，按未核实处理。
- **非均匀缩放 / 剪切**：点位置、根目标、基础 Verlet、重力/风、global shape 仍精确；
  不再等价的是**度量约束**：rest length、capsule 半径、SDF cell/truncation、
  速度 clamp 阈值都是局部厘米常量。源的长度约束目标也是作者常量（patch 8729），
  并不按 Actor scale 缩放，因此 scale 下源与 VL 的“目标长度”语义本身都不完备；
  VL 不能声称与源在 scale 下等价。剪切还会让局部各向异性度量与世界不同。
- **暂停 rebase**：只改表示坐标，不推进时钟、不递增 `simulationFrame`、不跑
  constraint/collision；`state.simulationFrame` 与 pose 身份都不提交
  （`hairSimulationPass.cpp:729-732`）。恢复首帧重建 collider/SDF，见 §3。
- **Reset**：三份历史 = 当前 placement/skinning 的 rest，速度清零；`H` 不影响结果。
  源也是三份全写，不是只钉根点。但源 host 在 reset 的同一 substep 仍会继续录制
  VSP/LocalShape/LengthWind/SDF（`TressFXCoreSimulation.cpp:1137-1214`），下一
  substep 再从该状态积分；VL 在 Reset 分支后直接 `continue`，只写三份、不跑后续
  pass。因此两者 reset 帧的最终位置不同，VL 停在纯 rest；这是 host 调度差异，
  不是三份历史写入的差异。
- **丢 pose**：保留上次相对矩阵并随 `H` 换基；世界根目标保持冻结。pose 恢复时
  `GuideSimulationHistory` 计划 `PoseRecovered` Reset（`tressfxSimulationHistory.h:56-66,81-97`），
  旧惯性被清掉，不做位移阈值猜测。
- **首帧未初始化**：`TressfxModelHistory` 不换基；SSBO 初值是局部 rest，首帧计划
  Reset。若首帧同时缺 pose，则 `InitialPos = identity*rest`（即未蒙皮 rest），
  直到首次 pose 的 `FirstPose` Reset 才对齐骨骼目标——这是明确的边界，不是换基 bug。
- **多实例共享 buffer**：`TressfxSimulationOwner` 只有看到不同 `sceneObjectIdentity`
  才判冲突并禁用模拟（`hairSimulationPass.cpp:602-614`；`tressfxSimulationOwner.h:12-33`）。
  同一 identity 的两个 placement 若共享 RenderableObject、但 model 不同，现有检查
  无法区分，会按第一个 draw 的 model 换基；这是未解决风险。
- **CPU 后端**：`GuideSimulation`（`source/hair/tressfxDynamics.cpp:83-113`）存局部
  位置但没有 `TressfxModelHistory`，渲染同样乘 `uboM.model`；它不会产生共同 Actor
  运动惯性。当前主路径是 GPU guide sim，CPU 路径的定位/是否仍被资产使用未核实。
- **SDF 代理**：VL 的代理 palette 与域都是 hair 局部，碰撞度量在局部。若
  `sourceToHair` 含非均匀缩放，SDF 的 cell size/margin/truncation 不发生世界尺度
  重标定，不能与源世界 SDF 等价。单骨回退路径（`sdf.counts.y == 0`）同样如此。

## 5. 新增测试与期望

追加在 `tool/hair-tests/tressfxWorldHistoryTests.cpp`，不新增 CMake 条目：

1. `FirstFrameNeverRebasesActorInitialPlacement`
   - 空 history 对非单位初始 placement：`HasPlacementChange=false`、
     `TransformToCurrentLocal=Identity`；提交后同 model 仍 false；下一次真实移动
     才换基且世界位置保持。
2. `ZeroDeltaRebaseKeepsWorldVelocityAndClock`
   - `T(5)*Rz90` 换基后局部坐标解析为 `(1,-2)`、`(1,-1)`、`(1,0)`；世界位置
     `(7,1)`、`(6,1)`、`(5,1)` 与速度 `(1,0)` 不变；已消费时钟仍为 0，下一帧
     仍拿到自己的 `dt`。
3. `PivotRotationPreservesWorldVelocityDirection`
   - 绕 `(10,0,0)` 的 +90° 换基：局部解析 `(12,0)`、`(11,0)`、`(10.5,0)`；
     换基矩阵旋转部分非单位、平移部分 `(10,10,0)`；世界速度仍为旧 `+Y`，
     证明 pivot 旋转没有被当成纯平移。
4. `FastTranslationRootFollowsAndTipHoldsThenPullsBack`
   - 根点当帧到世界 `(100,0)`；发梢第一步仍在 `(0,-10)`，无 shape 时继续保持；
     打开 global shape 0.5 后解析为 `(50,-10)`，第二次到 `(100,-10)`，第三次因
     Verlet 惯性越过到 `(125,-10)`，证明它是软约束而不是硬钉。
5. `ScriptedPlacementMatchesIndependentWorldVerletOracle`
   - 4 点（1 根 + 3 自由），90 帧脚本轨迹覆盖平移、绕非原点旋转、组合；期望值由
     纯世界坐标 Verlet oracle 逐步给出（不使用被测 `TransformToCurrentLocal`
     反推），逐帧比较世界 current/previous 与 `current-previous` 速度。
     容差 `2e-2 cm`（位置）/`5e-2 cm`（速度），高于本机 float 实测
     `6.8e-3 cm`/`1.9e-4 cm`，又能区分漏换基时厘米级误差。

历史等价测试不调用 GPU、不读回 SSBO；它证明的是“局部 + rebase 的数学表示”与
“源世界求解”的一致性，不冒充 GPU dispatch 数值验收。

## 6. 未核实 / 风险

- 没有 GPU 逐点读回：三份 SSBO 的 shader 分支正确性来自源码与公式，不是运行证据。
- 源 host 一帧最多两个循环、`Instance->TimeStep` 固定 0.0125、`-0.0167` 扣减与
  `0.016` clamp（`TressFXCoreSimulation.cpp:935-941,1012-1219`）；VL 原生调度已按
  n=0/1/2 × 0.0125 s 对齐，但本文的逐点等价证明只针对“给定同一个 `dt` 与同一个
  placement 序列”的 solver 步骤，不构成 GPU 轨迹读回证据。
- 多骨蒙皮：VL 用全骨架 `bindToPoseMatrices`，旧资产回退 per-section
  `matrices`；后者的未参与关节 inverseBind 可能退化为绝对 model 变换
  （`hairSimulationPass.cpp:632-635` 注释），不能据此声称与源逐点一致。
- SDF 代理：VL 的逐顶点 LBS transfer 依赖最近绑定姿态身体顶点（`tressfxProxySkinning.cpp`），
  与源导出的 `.tfxbone` 权重是否逐点一致未核实；域每帧重建、裁剪与源 sparse
  distance/direction 链表也不完全相同。
- `TressfxSimulationOwner` 只按 `sceneObjectIdentity` 判共享冲突；同 identity、
  不同 model 的共享 placement 仍可能用错 model。多实例独立状态未实现。
- CPU 参考求解器没有世界历史/惯性；是否仍有资产走 CPU 后端未核实。
- 渲染的 `previousClipPosition` 仍用 `previousModel * 当前局部位置`
  （`base.vert.glsl:54-60`），没有使用发丝上一帧位置；这属于渲染/运动矢量范围，
  本轮未改，不能据此宣称发丝运动矢量与源一致。
- 截断/数量级：源 `InitialPos *= 0.01` 在 float 下与 VL 直接厘米计算存在舍入差异；
  本文的逐点等价是代数结论，测试容差覆盖 float 误差但不覆盖 GPU 浮点实现差异。
