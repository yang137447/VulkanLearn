# VulkanLearn

## 简介
这是一个基于 Vulkan 的渲染学习项目，支持阴影映射、后处理、场景加载等功能。项目集成了 Tracy Profiler 和 NVIDIA Nsight Systems 用于性能分析。

## 文档分工

为了避免“现状说明”、“AI 协作说明”和“未来规划”混在一起，仓库内文档按职责分工如下：

- `README.md`
  - 面向人类读者的项目入口
  - 负责说明项目是什么、怎么构建、怎么运行、去哪里继续读
  - 只写当前仍然有效的上手信息，不承载长期规划细节
- `AGENTS.md`
  - 面向 AI coding agent 的仓库协作说明
  - 负责说明阅读顺序、修改约定、高风险区域、隐式耦合和工作方式
  - 不负责替代架构设计文档
- `documents/README.md`
  - 面向所有协作者的正式文档目录
  - 负责说明文档分类、每类文档的职责和收录规则
- `documents/architecture/`
  - 存放正式架构、线程模型、系统边界、数据流与通用编码约定
  - 这里描述当前认可的架构边界和落地状态
- `documents/rendering/`
  - 存放当前已经落地或正在被代码使用的渲染契约
  - 例如材质参数生成、贴图资产 JSON、descriptor image info 管理
- `documents/plan/`
  - 存放未实现、部分实现或长期路线型文档
  - 例如 Sky Pass、compute bloom、deferred GBuffer、foliage / SpeedTree、weather / GI 等规划
- `documents/reference/`
  - 存放教程、课程和背景学习资料

当前有效的正式文档入口：

- `AGENTS.md`
- `documents/README.md`
- `documents/architecture/vulkanlearn-architecture.html`
- `documents/rendering/texture-asset-json-v1.md`
- `documents/rendering/material-param-authoring-and-reflection.md`
- `documents/rendering/descriptor-imageinfo-management.md`

## 环境要求
- Windows 10/11
- CMake 3.20+
- MinGW-w64 with C++17 support
- Vulkan SDK (需包含 Nsight Systems)

当前仓库使用的是 MinGW 工具链，现有 `build/` 目录记录的 CMake generator 为 `MinGW Makefiles`。

## 构建指南

### 1. 配置项目
在项目根目录下打开终端，执行以下命令生成构建文件：

```bash
cmake -S . -B build -G "MinGW Makefiles"
```

### 2. 编译项目
执行以下命令进行编译：

```bash
cmake --build build -j
```

如果你的 MinGW 环境没有自动进入 `PATH`，请先确保 `g++`、`gcc` 和 `mingw32-make` 可用，再执行上面的 CMake 命令。

编译成功后，可执行文件 `main.exe` 将生成在 `build/bin` 目录下。

## 运行时资源位置

当前仓库不再以 git 形式保存完整 `resources/` 资产。

程序运行时从 `config/config.json` 的 `resourcePath` 读取资源树。该路径应直接指向独立资源仓库根目录，例如：

```json
"resourcePath": "D:\\YYBWorkSpace\\GitHub\\VukanLearnResources"
```

运行前请确保 `resourcePath` 至少包含：

- `scenes/`
- `models/`
- `terrains/`（使用地形场景时需要）
- `materials/`
- `textures/`
- `hdri/`

本地生成资源输出写入 `resourcePath/generated/`。

## 性能分析工具使用

本项目集成了两种性能分析工具：**Tracy Profiler** 和 **NVIDIA Nsight Systems**。代码中的 `PROFILE_SCOPE` 宏会同时触发两者的标记，你可以根据需求选择使用。

开发时可以打开 Vulkan validation layer 来持续监测 API 使用错误；这类构建适合查 correctness，不适合作为长时间挂机 FPS 基线。Profiler 标记和 validation layer 都会显著改变高 FPS 空转场景下的 CPU/Vulkan API 开销，长时间挂机测性能时不要把它们打开后的 FPS 当作真实性能。

重新配置性能基线时显式传入 `OFF`，避免已有 `build/` 缓存沿用开发监测配置：

```bash
cmake -S . -B build -G "MinGW Makefiles" -DVULKANLEARN_ENABLE_TRACY=OFF -DVULKANLEARN_ENABLE_NVTX=OFF -DVULKANLEARN_ENABLE_VULKAN_VALIDATION=OFF
cmake --build build -j
```

需要做性能分析或 Vulkan API 校验时，显式打开对应选项后重新配置。日常开发如果只想监测 Vulkan 报错，可以只打开 `VULKANLEARN_ENABLE_VULKAN_VALIDATION`：

```bash
cmake -S . -B build -G "MinGW Makefiles" -DVULKANLEARN_ENABLE_TRACY=ON -DVULKANLEARN_ENABLE_NVTX=ON -DVULKANLEARN_ENABLE_VULKAN_VALIDATION=ON
cmake --build build -j
```

调试断点会让进程暂停，Tracy 这类实时 profiler 的 socket 连接可能因此被对端关闭，并在 Windows 上表现为 `10054 ConnectionReset`。如果当前目标是单步调试逻辑而不是采样性能，可以关闭 Tracy/NVTX，但保留 Vulkan validation layer 用来检查 API 错误。

### 长时间 FPS smoke 诊断

`build/bin/main.exe --framesmoke [count] --exit-after-tests` 用于固定帧数性能基线。这个入口会每 5000 帧输出一次区间统计，包括 avg/min/max frame time、avg FPS、RenderLoop 耗时，以及 `ResourceRetireQueue` 的 pending/submitted/completed epoch。

这些区间统计是有意保留的诊断代码，不是临时调试残留。它只在 `--framesmoke` 运行期间采样和输出，普通运行不会打印区间统计；RenderLoop 计时也只在 frame smoke 激活时执行。保留它的原因是：长时间挂机掉帧通常需要区分 GPU/API 开销、渲染循环耗时增长、资源退休队列堆积等情况，单个最终平均 FPS 不足以定位问题。

一次已验证的结论：在高 FPS 空转场景下，开启 Vulkan validation layer / debug utils 会导致长时间 `--framesmoke` FPS 明显下降；Tracy/NVTX 会增加额外开销，但不是唯一来源。关闭 profiler 和 validation 后，60,000 帧 smoke run 稳定在约 2148 FPS，`retiredPending=0`，说明这次问题不是旧帧或退休资源队列堆积。

### 1. 使用 Tracy Profiler (实时 CPU 分析)

Tracy 适合在开发过程中实时查看 CPU 函数耗时、帧率波动和内存分配。

**版本说明：**
项目集成的 Tracy 代码已更新为 **v0.13.1** (发布版)，你可以直接从 [GitHub Releases](https://github.com/wolfpld/tracy/releases/tag/v0.13.1) 下载 `Tracy-0.13.1.7z`，解压后直接使用 `Tracy.exe`。
*注意：无需再手动构建客户端。*

**使用步骤：**
1.  **启动 Tracy**：
    运行下载的 `Tracy.exe`，点击界面的 **"Connect"** 按钮等待连接。
2.  **运行程序**：
    启动 `build/bin/main.exe`。
3.  **查看数据**：
    Tracy 会自动连接并开始滚动显示实时火焰图。

### 2. 使用 NVIDIA Nsight Systems (GPU 同步与系统分析)

Nsight Systems 适合分析 CPU 与 GPU 的交互、Vulkan API 的调用时序以及 GPU 的负载情况。

**步骤：**
1.  **安装工具**：
    确保已安装 NVIDIA Nsight Systems (通常随 Vulkan SDK 或 CUDA Toolkit 安装，也可单独下载)。
2.  **创建项目**：
    打开 Nsight Systems，创建一个新项目。
3.  **配置目标**：
    - **Target application**: 选择你的 `build/bin/main.exe`。
    - **Working directory**: 建议设置为项目根目录 (例如 `D:\GitHub\VulkanLearn`)，并确保 `config/config.json` 中的 `resourcePath` 指向可用资源仓库。
4.  **开始录制**：
    点击 **"Start"** 按钮，程序会自动运行。
5.  **结束分析**：
    运行一段时间后点击停止，Nsight 会生成报告。
6.  **分析时间轴**：
    在报告中，你可以在 "NVTX" 行 (或 "Threads" 下) 看到代码中埋点的 `PROFILE_SCOPE` 区域，并能将其与下方的 "Vulkan Queue" (GPU 工作队列) 上下对齐，从而精确分析 CPU 提交命令和 GPU 执行命令之间的延迟（例如 `WaitForFences` 的等待情况）。

## 代码接入指南

为了在性能分析工具中显示自定义的标记，请使用 `source/Profiler.h` 中定义的宏。这些宏会自动适配 Tracy 和 Nsight Systems。

### 1. 标记作用域 (PROFILE_SCOPE)

用于手动标记一段代码块的执行范围。

```cpp
#include "Profiler.h"

void SomeFunction() {
    {
        PROFILE_SCOPE("MyCustomScope");
        // ... 需要分析的代码 ...
    }
}
```

### 2. 标记函数 (PROFILE_FUNCTION)

用于自动标记整个函数，名称自动获取。

```cpp
#include "Profiler.h"

void MyFunction() {
    PROFILE_FUNCTION();
    // ... 函数体 ...
}
```

### 3. 标记帧 (PROFILE_FRAME)

用于标记帧的结束，通常在主循环末尾调用（Tracy 专用）。

```cpp
// 在主循环中
while (running) {
    // ... 渲染逻辑 ...
    PROFILE_FRAME();
}
```

## 目录结构
- `source/`: 源代码
- `shader/`: GLSL 着色器
- `resources/`: 不再作为运行时资产目录；运行时资产由 `config/config.json -> resourcePath` 指定
- `config/`: 渲染管线和场景配置
- `documents/`: 正式设计文档和规划文档
- `extern/`: 第三方库 (SDL3, Assimp, GLM, Tracy, NVTX 等)
