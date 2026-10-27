# 场景风对象（wind）

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码或测试；不含对未来行为的承诺。

风是场景里的全局对象，全场景至多一个。本契约覆盖场景数据路径、World/RenderSystem 的运行时状态，
以及植被（SpeedTree）对场景风的消费；头发仍使用各自既有的风数据（P1c 未实施）。

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
- `RenderSystem` 在准备 World 时把风状态复制进 `PreparedRuntimeBinding`，提交时整体赋给 `activeWind`；
  `SetActiveWorld` 同步更新。`RenderSystem::GetActiveWind()` 只读。
- 风时间 `RenderSystem::GetWindTimeSeconds()` 由 `AdvanceSpeedTreeWindProfiles` 每渲染帧固定推进 1/60 s，
  不读墙钟；World 换代时清零。
- 渲染器资源加载跳过 `wind` 对象（不产生 GPU 资源）；编辑器对象类型白名单接受 `wind`。

## 植被消费（SpeedTree）

- `AdvanceSpeedTreeWindProfiles` 每帧把 `activeWind` 与固定 tick 风时间交给 `SpeedTreeWindProfileSet::AdvanceToSceneWind`。
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

## 验证入口

- `tool/script-tests/scriptRuntimeTests.cpp` 中的 `SceneWind.*` 用例：合法解析、零方向、负强度、
  阵风越界、未知键、第二个 wind 对象、缺省无风。
