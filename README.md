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
  - 存放中长期架构目标、线程模型、系统边界、数据流等设计文档
  - 这里描述“目标状态”和“演进方向”，不是当前实现说明
- `documents/rendering/`
  - 存放渲染功能路线图和专题规划，例如 PBR、IBL、TOD
  - 这里描述“某个功能域未来准备怎么做”

当前有效的正式文档入口：

- `AGENTS.md`
- `documents/README.md`
- `documents/architecture/future-render-architecture.md`
- `documents/rendering/pbr-ibl-tod-roadmap.md`

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

程序运行时默认从仓库同级目录读取外部资源树：

```text
../VulkanLearnAssets/resources
```

按当前实现，仓库目录与外部资源目录的关系应为：

```text
ParentFolder/
  VulkanLearn/
  VulkanLearnAssets/
    resources/
```

运行前请确保外部 `resources/` 至少包含：

- `scenes/`
- `models/`
- `materials/`
- `textures/`
- `hdri/`

仓库内保留的 `resources/README.md` 仅用于说明资源迁移约定；本地生成内容仍可能写入 `resources/generated/`。

## 性能分析工具使用

本项目集成了两种性能分析工具：**Tracy Profiler** 和 **NVIDIA Nsight Systems**。代码中的 `PROFILE_SCOPE` 宏会同时触发两者的标记，你可以根据需求选择使用。

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
    - **Working directory**: 建议设置为项目根目录 (例如 `D:\GitHub\VulkanLearn`)，并确保同级目录存在 `VulkanLearnAssets/resources`，以便正确加载外部资源。
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
- `resources/`: 仓库内仅保留说明文件和本地生成内容，运行时资产位于同级 `VulkanLearnAssets/resources`
- `config/`: 渲染管线和场景配置
- `documents/`: 正式设计文档和规划文档
- `extern/`: 第三方库 (SDL3, Assimp, GLM, Tracy, NVTX 等)
