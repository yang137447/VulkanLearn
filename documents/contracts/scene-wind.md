# 场景风对象（wind）

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码或测试；不含对未来行为的承诺。

风是场景里的全局对象，全场景至多一个。本契约覆盖场景数据路径、World/RenderSystem 的运行时状态，
以及植被（SpeedTree）与头发（TressFX）对场景风的消费。

## 场景 JSON

`"type": "wind"`，由 `ValidateSceneAssetJson` 校验（`source/scene/validation/sceneJsonValidator.h`），
字段解析与校验在 `source/world/worldWind.h`。

| 字段 | 必填 | 约束 |
| --- | --- | --- |
| `name` | 是 | 非空字符串，场景对象通用字段 |
| `direction` | 是 | 3 个有限数，长度不能为零；加载后归一化，作者长度不影响方向 |
| `strength` | 是 | 有限数，`>= 0`，单位 cm/s |
| `gust` | 否 | 对象；缺省时阵风关闭 |
| `gust.amplitude` | 随 `gust` | 有限数，`[0, 1]` |
| `gust.frequency` | 随 `gust` | 有限数，`>= 0` |
| `gust.turbulence` | 随 `gust` | 有限数，`[0, 1]` |

- wind 对象与 `gust` 对象都拒绝未知键；wind 对象不接受 `position`、`rotation` 等空间字段。
- 一个场景出现第二个 `wind` 对象时加载失败，错误信息包含场景名，场景路径由 `SceneAssetValidator` 追加。
- 缺省 `wind` 对象即无风：下述运行时状态保持默认值。

## 运行时状态

- `World::GetWind()` 返回 `VL::WorldWind`（方向、强度、阵风参数、`authored`）。没有 `wind` 对象时默认强度为 0、阵风关闭、`authored=false`；
  显式作者零风时 `authored=true`，编辑器据此保留可再次调节的行。
- `WorldBuilder` 与 `environment` 在同一次 World 构建中写入风状态，随 World 的发布与替换整体切换。
- 风随 `WorldSnapshot` 冻结，再由 `RendererFrontend::BuildRenderScene` 复制进 `RenderScene::wind`
  （作者目标）；`RenderSystem` 提交一代 RenderScene 时整体交换该字段。不存在单独的 `activeWind` 副本。
- 每帧由 `SceneWindRuntime` 按固定响应时间缓动场级强度/方向，并施加场景阵风，结果写入
  `RenderScene::resolvedWind`；该结果供头发等场级消费者使用，SpeedTree 植被不读它。
  `RenderSystem::GetActiveWind()` 只读返回该场级解析结果；`GetWindTimeSeconds()` 返回本帧
  使用的秒数。植被读 `RenderScene::wind` 作者目标，在每物种 profile 内独立缓动/阵风。
- `AdvanceSceneWind` 每渲染帧固定推进 1/60 s，不读墙钟；World 换代时清零风时间并
  `Reset` 运行时，使首帧直接采用新场景目标值。同一帧同时把原始 `wind` 目标交给
  `WindResponseProfileSet`，每条 profile 的首帧也直接采用目标值。
- 渲染器资源加载跳过 `wind` 对象（不产生 GPU 资源）；编辑器对象类型白名单接受 `wind`。

## 植被消费（SpeedTree）

- `AdvanceSceneWind` 每帧把 `currentRenderScene.wind`（作者目标，不是 `resolvedWind`）交给
  `WindResponseProfileSet::AdvanceToSceneWind`；每个 profile 的 `SpeedTreeWindSystem` 相当于
  一个 SpeedTree `CWindStateMgr`。
- 场景强度除以 3270 cm/s（蒲福 12 级下界）得到 `[0, 1]` 曲线横轴。每个 profile 用自身
  `strengthResponse`/`directionResponse` 缓动方向与强度，再用自身 `gustFrequency`、
  `gustStrengthMin/Max`、`gustDurationMin/Max`、`gustRise/Fall` 推进离散阵风事件。
- 场景 `gust.enabled` 控制该 profile 是否允许 gust，`gust.amplitude` 缩放 profile 的 gust
  输出；场景 `gust.frequency`/`gust.turbulence` 只用于场级消费者（头发），不覆盖 profile
  的 gust 参数。
- 同一 base tree 的所有实例共享该 profile 的 live state；实例差异由世界位置和顶点
  Branch/Ripple 权重提供，与 SpeedTree 的 base-tree 粒度一致。
- 场景强度为 0 且没有 gust 时 GPU 强度写 0；着色器的零风早退使植被保持原始位置。
- GPU 布局未变：`SpeedTreeWindStateGPU` 仍为 208 字节，`UBOModel.speedTreeWind` 偏移 128 不变，着色器无改动。
  `windVector.w` 仍为归一化强度 [0, 1]。
- 响应曲线采样与 UBO 打包由 `UpdateGpuState` 共享；运行时路径内部调用
  `SetStrength`/`SetDirection`/`SetGustingEnabled`/`AdvanceTo` 推进每 profile 状态，
  `ForceGust` 仍只供 `tool/speedtree-wind-validation` 对照。
- 控制台 `windstrength`/`windgust` 与它们的 UI action / view-model 字段已移除；植被风完全由场景风驱动。

## 头发消费（TressFX）

- `simulationDefaults.wind` 已从 TressFX 资产合同移除。资产或放置/分组 overrides 仍带 `wind` 时，
  `MeshAssetValidator` 在加载期直接失败，错误信息指向场景 `wind` 对象（`type: "wind"`）；
  不再静默落回无风。
- `HairSimulationPass::Record` 从同一帧的 `renderScene.resolvedWind` 取风：有效风向量（cm/s）
  就是解析结果的 `direction * strength`；缓动与阵风已在 `SceneWindRuntime` 完成，头发不再
  自行采样阵风。
- 头发不再乘 TressFX SetWind 的 `sin(frame*0.001)² + 0.5` 逐帧调制：时间调制由场景风层独占，
  再乘一次会双重调制。这是与 TressFX 5.0 的刻意偏差，`TressfxWindFrameModulation` 只保留给
  legacy/SDK 对照与离线测试。
- 场景强度为 0 时有效风向量为零：Host 不做零向量归一化，四个风锥全零，头发只受重力/约束/碰撞驱动。
- CPU 参考后端（`backend: "cpu"`）在游戏线程消费 `WorldWind` 的方向与强度；GT 不拥有渲染线程的
  `SceneWindRuntime`，因此不应用场景缓动/阵风。GPU 头发读场级 `resolvedWind`；这与植被的
  每 profile 状态不同源，植被不会读取 `resolvedWind`。
- GPU 参数块布局未变（`wind0..3` 仍是四路锥形风），着色器无改动。
- 编辑器 TF 资产页的 `wind` 实时参数已移除；场景风的本地调试控件属于 P1d，尚未实现。

## 编辑器视图

- 大纲把 World 的场景风发布为 `Wind` 分组下的作者 name（如 `MainWind`，身份 `wind|<name>`）。
  只要场景作者了 `wind`（`authored=true`）就出现，包括显式零风；每物种的 SpeedTree 响应曲线
  属于网格资产，不再作为 actor 出现在大纲。
- Details 沿用旧 SpeedTree 调节方式：方向 + `Strength` 滑条（0–3270 cm/s）+ `Gust` 滑条（0–1）。
  编辑通过 `SetActorProperty` 更新 World，并由 `SceneDocumentService` 暂存回场景 JSON；
  `Gust` 为 0 即关闭，非零时场级消费者使用固定的 `frequency=0.5`、`turbulence=0.2` 默认值，
  噪声频率/湍流不暴露在 UI；SpeedTree profile 的 gust 形状仍来自资产参数。
- 场景风没有 TRS、没有可见性开关；本地 TF Tab 风控件仍属 P1d。
- 风源数据只保留场景 `wind`；原始 `.stsdk` 的 `strengthResponse`/`directionResponse`/`gust*`
  由每个 SpeedTree profile 的 CWindStateMgr 语义使用。场景 `gust.amplitude` 缩放 profile
  gust，`frequency`/`turbulence` 只服务场级消费者，不覆盖 profile 的 gust 参数。

## 验证入口

- `tool/script-tests/scriptRuntimeTests.cpp` 中的 `SceneWind.*` 用例：合法解析、零方向、负强度、
  阵风越界、未知键、第二个 wind 对象、缺省无风。
- `tool/renderer-backend-tests/speedtree_wind_system_test.cpp`：同一场景目标下不同
  `strengthResponse`/`directionResponse` 的 profile 缓动、以及场景 gust amplitude 对
  profile gust 的缩放。
- `tool/hair-tests/` 中的场景风用例：`SceneWindRuntime` 的缓动/阵风/复位、解析风向量、零风、
  `MakeGuideSimulationParams` 打包；资产校验用例覆盖 `simulationDefaults.wind` 与 overrides `wind`
  的加载期拒绝。
