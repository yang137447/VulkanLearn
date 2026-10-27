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

- `World::GetWind()` 返回 `VL::WorldWind`（方向、强度、阵风参数）。没有 `wind` 对象时默认强度为 0、阵风关闭。
- `WorldBuilder` 与 `environment` 在同一次 World 构建中写入风状态，随 World 的发布与替换整体切换。
- 风随 `WorldSnapshot` 冻结，再由 `RendererFrontend::BuildRenderScene` 复制进 `RenderScene::wind`；
  `RenderSystem` 提交一代 RenderScene 时整体交换该字段。不存在单独的 `activeWind` 副本，
  `RenderSystem::GetActiveWind()` 只读返回 `currentRenderScene.wind`。
- 风时间 `RenderSystem::GetWindTimeSeconds()` 是当前帧使用的秒数。`AdvanceSpeedTreeWindProfiles`
  在每渲染帧推进前把它写进 `currentRenderScene.windTimeSeconds`，然后固定推进 1/60 s，不读墙钟；
  World 换代时清零。
- 渲染器资源加载跳过 `wind` 对象（不产生 GPU 资源）；编辑器对象类型白名单接受 `wind`。

## 植被消费（SpeedTree）

- `AdvanceSpeedTreeWindProfiles` 每帧把 `currentRenderScene.wind` 与本帧风时间交给 `SpeedTreeWindProfileSet::AdvanceToSceneWind`。
- 每个物种的 `SpeedTreeWindSystem` 把场景强度（cm/s）除以 3270 cm/s（蒲福 12 级下界）得到 `[0, 1]` 的曲线横轴，
  再用该物种自己的响应曲线采样（bend / oscillation / speed / turbulence / flexibility 等），
  风向取场景风的归一化方向。阵风为 `frequency` 决定格点频率、`amplitude` 决定相对调制、
  `turbulence` 在粗细两个八度之间插值的连续值噪声，不产生离散阵风事件。
- 场景强度为 0（或阵风调制到 0）时，GPU 风向写为零向量，着色器的零风早退使植被保持原始位置；
  `TransformSpeedTreeWindDirectionToLocal` 对零向量显式返回零。
- GPU 布局未变：`SpeedTreeWindStateGPU` 仍为 208 字节，`UBOModel.speedTreeWind` 偏移 128 不变，着色器无改动。
  `windVector.w` 仍为归一化强度 [0, 1]。
- 响应曲线采样与 UBO 打包由 `UpdateGpuState` 共享；SDK 自驱动状态机（`SetStrength`、`ForceGust` 等）
  只供 `tool/speedtree-wind-validation` 对照，渲染路径不调用。
- 控制台/UI 的 SpeedTree 强度与阵风开关仍存在于 `RenderSystem`，但不再影响渲染（待 P1d 清理）。

## 头发消费（TressFX）

- `simulationDefaults.wind` 已从 TressFX 资产合同移除。资产或放置/分组 overrides 仍带 `wind` 时，
  `MeshAssetValidator` 在加载期直接失败，错误信息指向场景 `wind` 对象（`type: "wind"`）；
  不再静默落回无风。
- `HairSimulationPass::Record` 从同一帧的 `renderScene.wind` 与 `renderScene.windTimeSeconds` 取风：
  有效风向量（cm/s）= `direction * strength * gustFactor`，其中 `gustFactor` 由共享的
  `ApplySceneWindGust(1.0, gust, windTimeSeconds)` 得到，与植被同一阵风相位。
- 头发不再乘 TressFX SetWind 的 `sin(frame*0.001)² + 0.5` 逐帧调制：时间调制由场景阵风独占，
  再乘一次会双重调制。这是与 TressFX 5.0 的刻意偏差，`TressfxWindFrameModulation` 只保留给
  legacy/SDK 对照与离线测试。
- 场景强度为 0 时有效风向量为零：Host 不做零向量归一化，四个风锥全零，头发只受重力/约束/碰撞驱动。
- CPU 参考后端（`backend: "cpu"`）在游戏线程消费场景风的方向与强度；它不拥有渲染线程的风时钟，
  因此不应用场景阵风，GPU 路径才与植被共享阵风相位。
- GPU 参数块布局未变（`wind0..3` 仍是四路锥形风），着色器无改动。
- 编辑器 TF 资产页的 `wind` 实时参数已移除；场景风的本地调试控件属于 P1d，尚未实现。

## 编辑器视图

- 大纲把 World 的场景风发布为只读的 `Scene Wind` actor（稳定身份 `wind|scene`，与每物种
  `windProfile|<key>` 分组分开）。只有 `strength > 0` 或阵风开启时出现；详情显示方向、
  强度（cm/s）与阵风参数。它是 World 状态的只读投影，不写回场景。
- 场景风没有 authored 标记；强度 0 且阵风关闭的 World 不产生该 actor（与"缺省即无风"一致）。
- 该行没有 TRS、没有可见性开关；本地风编辑与 SpeedTree 旧控件清理仍属 P1d。

## 验证入口

- `tool/script-tests/scriptRuntimeTests.cpp` 中的 `SceneWind.*` 用例：合法解析、零方向、负强度、
  阵风越界、未知键、第二个 wind 对象、缺省无风。
- `tool/hair-tests/` 中的场景风用例：场景风向量/阵风因子、零风、`MakeGuideSimulationParams` 打包；
  资产校验用例覆盖 `simulationDefaults.wind` 与 overrides `wind` 的加载期拒绝。
