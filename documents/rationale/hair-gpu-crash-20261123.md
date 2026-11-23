# Rod GPU 崩溃定位与修复记录（2026-11-23）

状态：**rationale（历史与决策）**，不是 runtime 契约，也不构成物理或性能放行。
记录一次真实的 DeviceLost 定位、三处修复、验证规模与仍然开放的风险。

## 症状

`SC_tressfx_rod_validation` 系列场景（2000 模拟 guide、SDF 代理 + 4 胶囊）在
`--worker-thread-count 1/2`、`--no-dev-ui`、`--frame-limit 60` 下反复启动时，
修复前约 50%–100% 的 run 会在启动后约 1–2 s 抛：

```text
Render thread failed: vk::Queue::submit: ErrorDeviceLost
terminate called ... vk::Device::waitForFences / waitIdle: ErrorDeviceLost
```

Windows System 日志同一时刻有密集的 `nvlddmkm` **id=153**（GPU 故障）事件，
没有 TDR 4101，也没有稳定复现的帧号。用 `--perf-jsonl` 留下的样本只到首帧
附近，说明崩溃发生在早期帧而不是退出阶段。

## 定位过程（场景变体，每项 N 次 240 帧运行）

| 变体 | 崩溃 |
| --- | --- |
| 完整场景（SDF + 胶囊 + skinning，2000 guides） | 8 / 8 |
| 无 collision（无 SDF、无胶囊） | 0 / 16 |
| 仅胶囊（无 SDF） | 0 / 8 |
| 仅 SDF（无胶囊） | 4 / 8 |
| 2000 guides 变 200 guides（仍含 SDF + 胶囊） | 0 / 8 |

结论：崩溃只出现在 **SDF 路径**，且与 guide 数量（采样次数）正相关；不是纯性能超时
（GPU hairSimulation 稳态只有 5–7.5 ms，`--frame-limit 15/240` 都不改变结论）。

另外用 Vulkan SDK 1.4.363 的 validation layer 跑通了校验：
正常帧没有任何 VUID，只有退出阶段每轮出现 2 次
`VUID-vkDestroyDescriptorPool-descriptorPool-00303`（见“未完成”）；
GPU-AV 跑了 3 × 240 帧也没有报 shader 越界（很可能被 GPU-AV 的减速掩盖）。
校验层本身没有复现 DeviceLost。

## 修复 1：未初始化的 SDF 历史被采样（主因）

`RodGpuObject::sdfHistory` 是 DEVICE_LOCAL，创建时不清零；`RodSdfSurfaceVelocity`
原先只在 `rod.flags.x != 0`（reset 帧）早退。若某帧 `sdfValid == false`
（SDF 尚未建立、或该帧没有写历史），下一帧 reset 标志已清，shader 会读到
未初始化的 `dims/originCell/worldToHair`；一旦 `timing.w` 恰好非零，就会用垃圾
维度计算 `cells[]` 索引，形成越界读，直接把设备打掉。这解释了“50% 概率 + 与
采样次数正相关 + 首帧附近”的全部特征。

修复（Host 显式状态 + Shader 双重保护）：

- `RodGpuObject` 新增 `sdfHistoryValid`；`ClearTimeline` 时清零。
- `params.timing.z` 携带该标志；只有真正记录了 `rodStoreSdf` 的下一帧才置 1。
- `rodSdfHistory.glsl` 在 `rod.timing.z <= 0` 或 `dims` 任一维为 0 时直接返回
  `SdfOutOfRangeDistance`；`RodSdfSurfaceVelocity` 同样早退。

效果：完整场景从 8/8 降到约 1/8–2/16（240 帧、同步；30 帧样本 2/16）。

## 修复 2：统计路径把 VK_NOT_READY 当致命错误

残余崩溃里有一部分不是 DeviceLost，而是：

```text
what(): Vulkan timestamp query was not ready after its swapchain image fence completed.
```

`EnvironmentGpuTimer::CollectFrameResults` 用会抛异常的 `ReadTimestampQueryPair`。
这与 `gpuPassTimer` 已声明的“统计路径绝不阻塞渲染线程”策略不一致。
改为 `TryReadTimestampQueryPair`：未就绪就丢弃本轮样本，不终止进程。

## 修复 3：启用设备级 robustBufferAccess（防护）

修复 1/2 之后仍有约 12% 的 DeviceLost（2/16、1/8），说明还存在一处 shader
缓冲区越界。打开 `VkPhysicalDeviceFeatures::robustBufferAccess` 后：

- 同步：0 / 36 次 240 帧崩溃（前 12 次 + 后 24 次）；
- 异步（`--worker-thread-count 2`）：0 / 6 次；
- 性能：`hairSimulation` GPU 仍为 6.1–7.5 ms，无可测退化。

**这是防护而不是根因修复**：越界写会被丢弃、越界读返回 0，可能把错误变成静默的
数值异常。具体越界点在 GPU-AV 的 3 次长跑中没有暴露，仍需继续定位；找到并修掉后
应移除该开关（或改为可选诊断）。它同时是设备级设置，影响整个引擎，不是 hair 专属。

## 验证范围

- 崩溃率：见上表与“0/36、0/6”；样本都是 240 帧、`SC_tressfx_rod_static`
  或 `Generated/Validation/` 下的派生场景。
- 场景仍能正常出图：修复后重拍了静止与 20 m/s 风下 0.5 s / 2.0 s 的 1280×720
  截图（资源侧 `Generated/Validation/SC_rod_wind_static.json` 只是把作家场景的
  `"strength"` 改为 2000，未改正式验证场景）。
- 这不构成物理正确性、真实头发外观或性能验收；本记录只声明“不再随机 DeviceLost”。

## 未完成

1. **具体越界访问未定位**。候选项集中在 SDF 相关的 descriptor 访问；下一步可用
   `VK_EXT_device_fault` 在 DeviceLost 时打印 fault type/address，或让 GPU-AV
   在关闭 robust 的情况下跑更长样本。
2. **退出时的 `VUID-vkDestroyDescriptorPool-descriptorPool-00303`** 每轮必现：
   销毁 descriptor pool 时仍有已提交命令引用它。可能来自 `--exit-after-frames`
   在“已录制未提交”的帧上退出，也可能是 world 清理顺序；目前不影响交互使用，
   但属于真实的 API 违规。
3. 修复 1 的标志没有进入 trace/诊断字段；后续若做 GPU 状态对比，应把它写进
   `RodGpuParams` 的既有诊断输出。

## 复现命令

```powershell
cd <tressfx5-hair-port>\VulkanLearn
# 修复前可复现（当前代码应稳定）
build\bin\main_editor.exe --initial-scene Maps/SC_tressfx_rod_validation/SC_tressfx_rod_static.json `
  --frame-limit 60 --worker-thread-count 1 --no-dev-ui --exit-after-frames 240
# 可选的校验层（需安装 Vulkan SDK，并设置 VK_LAYER_PATH/VK_INSTANCE_LAYERS）
$env:VK_LAYER_PATH = "$env:VULKAN_SDK\Bin"
$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
```

## 改动文件

- `source/hair/rodGpuRuntime.h`：`sdfHistoryValid` 状态与 reset 清理。
- `source/hair/rodGpuRecording.h`：`timing.z` 传递、store 后置位。
- `shader/glsl/hair/rodSdfHistory.glsl`：历史有效性早退与 dims 防御。
- `source/render/environment/environmentGpuTimer.cpp`：timestamp 非阻塞读取。
- `source/vulkanManager.cpp`：设备级 `robustBufferAccess`。
