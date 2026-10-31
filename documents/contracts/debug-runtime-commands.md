# Debug Runtime Commands

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或`tool/architecture-boundary-audit.ps1` 的门禁规则；不含对未来行为的承诺——那些在 `documents/rationale/`。

VulkanLearn 的 DebugConsole 通过 `CommandBus` 投递调试意图，EngineLoop 在稳定的 GT 命令点执行。Console 不直接持有 World、Camera 或 Vulkan 资源。
`editor <JSON>` 借用编辑器 facade，通过独立的 `EditorCommandBus` 提交版本化资产命令，
并在后续帧输出结构化终态；需要编辑器 runtime。协议与 headless 调用见
[资产修改接口](asset-modify-api.md)。

## Camera

```text
camera get
camera position <x> <y> <z>
camera lookat <x> <y> <z>
camera pose <px> <py> <pz> <tx> <ty> <tz>
```

相机命令作用于当前 active World 的 Camera。`lookat` 保留当前位置和当前 up；`pose` 使用世界空间位置与目标点重建朝向。命令执行前要求所有输入为有限值，目标点不能与相机位置重合。

## Screenshot

```text
screenshot
screenshot debug_view.bmp
```

相对路径统一写入 `resourcePath/Generated/Screenshots/`，绝对路径可以直接使用。截图请求在当前帧 UI 录制完成并提交后执行，backend 等待该次提交完成，从 swapchain image 做一次性 GPU 回读并写出 24-bit BMP。这样截图不依赖窗口是否位于前台，且不会让 DebugConsole 线程访问 Vulkan 对象。

截图是显式调试操作，因此只在请求发生的帧执行一次 `WaitIdle`；正常帧路径不增加同步开销。

## Particle Dump

```text
particledump
particledump dump_smoke.json
particledump dump_smoke.json 32
```

把当前 `ParticleRenderer` 的每个发射器粒子池从 GPU 回读到 CPU，并写出统计 JSON。
相对路径固定在 `resourcePath/Generated/ParticleDumps/`；文件名必须相对、非空、不含 `..`，
扩展名统一为 `.json`。未给文件名时使用 `particles.json`；`maxPerEmitter` 默认 128，
范围 `0..4096`（0 表示只写统计，不写粒子明细）。

执行前会先停住可选的渲染线程并等待设备空闲，然后为每个发射器创建 host-visible staging
buffer，用一次性 command buffer 从粒子池做 `copyBuffer`。JSON 根字段为
`totalEmitters`、`visibleEmitters`、`totalParticles`、`aliveParticles`、
`estimatedDrawnVertices`、`gpuTimingSupported`、`gpuMilliseconds`、
`gpuAverageMilliseconds`、`gpuMaxMilliseconds`、`gpuSampleCount`，
每个 emitter 含 `key`、`maxParticles`、`aliveCount`、
`culled`、`sortParticles`、`localSpace`、`renderMode`、`bounds.center`、`bounds.radius`、
`elapsedSeconds`、`drawnVertices` 与最多 N 个活粒子的
`position/age/velocity/lifetime` 明细。

`drawnVertices` 按当前 draw 路径估算：billboard 为 `maxParticles * 6`，
ribbon 为 `maxParticles * ribbonSegments * 6`；
`estimatedDrawnVertices` 只累加未被 cull 的发射器。位置/速度是模拟空间原始值：
`localSpace: true` 时是发射器局部坐标，否则是世界坐标。这是 CPU 回读统计，
GPU 字段与 `particlestats` 同源（particle graphics pass 的 timestamp），
不是 overdraw 计数或显存带宽统计。

## Particle Stats

```text
particlestats
```

不接受额外参数；多写参数时打印 `Usage: particlestats`。
命令投递到 GT 后读取 `RenderSystem` 的上一帧粒子预算快照，并在 stdout 打印两行：
第一行是固定两位小数的单行摘要
`particles frame=... elapsed=...s emitters=... visible=... culled=... sorted=...
drawCalls=... vertices=... particles=... sortDispatches=... sortCapacity=...
gpuTimingSupported=... gpuMs=... gpuAvgMs=... gpuMaxMs=... gpuSamples=...`，
第二行是同一份数据的 camelCase JSON。

`gpuMs` 系列来自 particle graphics pass 的 GPU timestamp：每个 swapchain image
独占一对 query，在该 image 下一次 `BeginFrame` 回收上一轮结果。只覆盖
`RecordGraphics`，不含 compute 模拟与排序 dispatch。后端不支持 graphics timestamp
时 `gpuTimingSupported=false`，其余 GPU 数值为 0，渲染不受影响；计时启用时
`gpuMs/gpuAvgMs/gpuMaxMs` 固定三位小数。

统计来自渲染器当前帧的 CPU 侧录制结果，不等待设备空闲、不做 GPU 回读；没有 active World
时也照常输出。`totalParticles` 是作者池容量总和，`drawCalls`/`vertices` 只累计实际执行
`draw` 的 emitter，`sortDispatchCount` 只累计未剔除且启用排序的 emitter 的 bitonic pass。
它不包含 GPU overdraw、GPU timer、带宽或实际存活粒子数（存活数走 `particledump`）。

## Particle Instances

```text
particlespawn <name> <PS_asset_path> [x y z]
particledestroy <name>
particlerestart <name>
```

三个命令都作用于当前 active World 的粒子系统实例，不修改 `PS_*.json` 资产，也不写场景文件。

- `particlespawn` 用 `ParticleAssetLoader::Prepare` 读取并校验资产，然后在 World 中新增一个
  `WorldParticleSystem`；位置默认 `(0,0,0)`，只做平移摆放。同名实例已存在时覆盖原实例，
  日志会区分 `spawned` 与 `replaced`。名称或资产路径为空、位置含 NaN/Inf、资产读取或校验失败
  都会被拒绝并给出 warning。
- `particledestroy` 从 World 移除同名实例；不存在时只发 warning，不改变其它实例。
  GPU 池由下一次 Render 的 `ParticleRenderer::Synchronize` 交给 retire queue。
- `particlerestart` 要求实例存在。它先把该场景对象名下所有 emitter 的
  `elapsedSeconds`、`prewarm`、`needsClear`、`culled` 重置，使 burst 周期、flipbook
  系统时间与连续发射从 0 重新开始；不重新读取资产、不重建纹理或 GPU 池，也不影响
  其它场景对象。

restart 在 GT 命令点执行；分线程渲染时先等待渲染线程空闲，避免在帧中途改 emitter 状态。
GPU 池尚不存在（例如同一批命令里先 spawn 再 restart）时 restart 是空操作，新池本来就会
从预热状态开始。

## Animation

```text
animation get
animation play
animation pause
animation seek <seconds>
animation clip <zero-based-index>
```

作用于当前 realtime World 的所有原生骨骼播放器；同 Actor 的 sections 只控制一次。
GT 拒绝非有限值、越界时间/片段和陈旧 UI generation，范围校验先覆盖全体播放器。
seek 同时暂停，clip 从零开始并保持播放开关；get 按 mesh identity 输出每个
唯一播放器的片段、时间、时长与开关（同一播放器的 sections 只输出一次）。
这些命令不保存资产、不控制 C# 生命周期。实现边界见
[动画播放与蒙皮](animation-runtime-skinning.md)。
`debugview 90` 直接显示 deferred GBuffer 速度，零速度为 0.5、XY 增益为 20，
供暂停、seek 和连续蒙皮检查；颜色仍由最终 framebuffer 编码。

```text
motiondebug <0|1>
```

设置当前 realtime World 的轨迹叠层；只接受 0 或 1，离线拒绝。
青色是实际物理后根历史，橙色是未求未来碰撞的预测，绿色是采样关节。
UI 的 Motion checkbox 使用同一带 generation 验证的请求路径；
状态只属于当前 World，不写资产。采样与投影边界见
[轨迹与特征数据库](animation-motion-features.md)。

## Assembly

```text
reloadassembly
```

从当前配置的 Gameplay assembly 路径重新加载 C# 类目录并走既有 World 事务。
Editing 下成功替换；Playing/Paused 若存在受控骨骼控制器，按迁移契约拒绝并
提示先 Stop。离线或 C# 未启用时拒绝。命令不写资产，也不改变 World generation。
独立包用它验证导出后的程序集重载路径。

## Input

```text
injectinput <action> <down|up>
```

向当前 gameplay action 层注入合成动作，对应 UE `UPlayerInput::InjectInputForAction`。
它不要求 SDL 键盘焦点，不伪造 SDL held 状态，也不写资产；用于自动化验证
`Interact`/`Jump` 等 action 到 Blueprint/Controller 的路径。真实 OS 键盘路径
仍需窗口焦点，二者不能混为一谈。

## DrawDebug

```text
debugdraw on
debugdraw off
debugdraw status
debugdraw demo
debugdraw demo depth
debugdraw demo overlay
debugdraw category "AI Paths" off
debugdraw owner "Script/player"
debugdraw owner *
debugdraw clear
debugdraw clear owner "Demo"
debugdraw clear category "Physics"
```

操作当前 realtime World 的公共收集器，离线拒绝。status 输出可见/存储项数、
几何数、类别与 owner；demo 通过正式 DrawDebug API 在世界原点周围画出全部形状和标签，
owner 为 Demo，category 为 DrawDebug，重复命令按固定 key 替换。
demo 缺省或 `overlay` 为穿透显示；`depth` 对全部示例形状启用场景遮挡。
带空格的身份用双引号；owner `*` 恢复全部。clear 保留过滤设置。
Console 解析只排队类型化控制值，执行、预算与生命周期见 [公共 DrawDebug](debug-draw.md)。

## 启动参数排队命令

自动化采集（例如论文 case showcase 截图）不需要手敲控制台，可以直接在启动参数里排队：

```text
--console-command "<line>"          可重复，按命令行顺序提交
--console-command-delay <frames>    默认 60；等待后每帧提交一条，命令可跨帧生效
```

```powershell
.\build\bin\main.exe --initial-scene "Maps/SC_x/SC_x.json" `
  --console-command "debugview 67" `
  --console-command "screenshot case.bmp" --console-command-delay 120
```

命令经 `DebugConsole::SubmitCommandLine()` 提交，与交互输入走同一条 `ProcessCommand` 解析路径，
因此脚本命令和手输命令的校验、日志完全一致。延迟是必需的：第 1 帧时环境贴图、CLUT 与
swapchain 内容都还没稳定，立即截图只会得到无效画面。延迟结束后命令按参数顺序**每帧提交一条**；
例如 `particlespawn` 后的 `particlestats` 会读取 spawn 已经生效的下一帧。

两个自动化注意事项：启动进程时**不要把窗口最小化**（client area 失效会导致 swapchain
out-of-date，渲染线程以 `presentKHR: ErrorOutOfDateKHR` 退出，延迟帧走不到）；用
`Start-Process -ArgumentList` 时**要自己拼好引号**，否则 `"debugview 67"` 会被拆成两个参数。

完整用例（场景、材质实例、像素级验证方法与已确立的材质 / 着色器契约）见
`documents/plan/rendering/shading-model-alignment-plan.md` §1.4「共享 A｜求值与测量设施」与 §1.7
「实测确立的材质与着色器契约」。
