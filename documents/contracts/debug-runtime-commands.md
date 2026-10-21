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
--console-command-delay <frames>    默认 60，提交前等待的帧数
```

```powershell
.\build\bin\main.exe --initial-scene "Maps/SC_x/SC_x.json" `
  --console-command "debugview 67" `
  --console-command "screenshot case.bmp" --console-command-delay 120
```

命令经 `DebugConsole::SubmitCommandLine()` 提交，与交互输入走同一条 `ProcessCommand` 解析路径，
因此脚本命令和手输命令的校验、日志完全一致。延迟是必需的：第 1 帧时环境贴图、CLUT 与
swapchain 内容都还没稳定，立即截图只会得到无效画面。

两个自动化注意事项：启动进程时**不要把窗口最小化**（client area 失效会导致 swapchain
out-of-date，渲染线程以 `presentKHR: ErrorOutOfDateKHR` 退出，延迟帧走不到）；用
`Start-Process -ArgumentList` 时**要自己拼好引号**，否则 `"debugview 67"` 会被拆成两个参数。

完整用例（场景、材质实例、像素级验证方法与已确立的材质 / 着色器契约）见
`documents/plan/rendering/shading-model-alignment-plan.md` §1.4「共享 A｜求值与测量设施」与 §1.7
「实测确立的材质与着色器契约」。
