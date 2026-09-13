# Debug Runtime Commands

VulkanLearn 的 DebugConsole 通过 `CommandBus` 投递调试意图，EngineLoop 在稳定的 GT 命令点执行。Console 不直接持有 World、Camera 或 Vulkan 资源。

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
