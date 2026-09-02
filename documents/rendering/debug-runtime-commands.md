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
screenshot hair_debug.bmp
```

相对路径统一写入 `resourcePath/Generated/Screenshots/`，绝对路径可以直接使用。截图请求在当前帧 UI 录制完成并提交后执行，backend 等待该次提交完成，从 swapchain image 做一次性 GPU 回读并写出 24-bit BMP。这样截图不依赖窗口是否位于前台，且不会让 DebugConsole 线程访问 Vulkan 对象。

截图是显式调试操作，因此只在请求发生的帧执行一次 `WaitIdle`；正常帧路径不增加同步开销。
