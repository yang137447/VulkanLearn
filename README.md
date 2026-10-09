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
  - 存放未实现、部分实现或长期路线型文档；已落地方案可以保留决策记录
  - 当前实现契约必须迁移到 `documents/architecture/` 或 `documents/rendering/`
- `documents/reference/`
  - 存放教程、课程和背景学习资料

当前有效的正式文档入口：

- `AGENTS.md`
- `documents/README.md`
- `documents/architecture/vulkanlearn-architecture.html`
- `documents/architecture/game-ui-stack.md`
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

### UI 构建与运行

默认构建包含 RmlUi 运行时 UI 和 Dear ImGui 开发者 UI。只需要运行时 UI
时，可以在配置阶段关闭开发者层：

```bash
cmake -S . -B build -G "MinGW Makefiles" -DVULKANLEARN_ENABLE_DEVELOPER_UI=OFF
cmake --build build -j
```

运行时 UI 资产位于仓库内的 `ui/`，由 `config/config.json -> ui` 配置块选择。
`F10` 或手柄 `Start` 打开/关闭运行时控制页，`Esc` 切换鼠标捕获，
`F1` 切换开发者工具。启动参数
`--dev-ui` 和 `--no-dev-ui` 可以覆盖配置中的开发者 UI 开关：

```bash
build/bin/main.exe --dev-ui
build/bin/main.exe --no-dev-ui
```

RML/RCSS/本地化文件在开发模式下支持候选解析、校验、提交式热重载；失败候选
不会替换最后一个有效页面。实现契约见 `documents/architecture/game-ui-stack.md`。

### VL 离线渲染

VL 是离线渲染的唯一出图入口；纯 CPU `vl_offline_core` 由根工程构建，
数值测试不需要 Vulkan 窗口。当前架构边界见
`documents/architecture/vl-offline-rendering.md`。VL 已接入冻结的
World/MI/mesh/纹理输入、渐进会话和 Vulkan 离线显示；支持范围只限
Lambert、ClothSheen、ClothFiber、AshikhminVelvet、AshikhminSatin 的已验证配置，遇到不支持的环境、光源、
动画或材质时会在准备阶段拒绝，不回退到 DefaultLit。资源仓库中的
`Maps/SC_offline_shared/SC_offline_shared.json` 是共用实时/离线路径的
Lambert 受控场景，启动不修改全局 `initScene`：

```powershell
build/bin/main.exe --initial-scene Maps/SC_offline_shared/SC_offline_shared.json --dev-ui
build/bin/main.exe --offline --initial-scene Maps/SC_offline_shared/SC_offline_shared.json --dev-ui
```

离线-only 的 A/B 双片案例在
`Maps/SC_offline_cloth/SC_offline_cloth.json`（方向光）和
`Maps/SC_offline_cloth/SC_offline_cloth_area.json`（单面方形面光）；
`SC_offline_cloth_texture.json` 验证 sRGB UV checker，
`SC_offline_cloth_groom.json` 验证 B 臂 signed-RG PFM；
它们使用资源仓库内的 Linen profile，实时模式会明确拒绝面光和 CPU-only
模型。这些是 UV 平片的能力探针，不是论文原图复刻。
同一资源目录的 `SC_offline_cloth_drape.json` 让 A/B 共用一片带褶皱和
有效 UV 的 FBX 布面，观察真实几何下的两种离线散射响应：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_offline_cloth/SC_offline_cloth_drape.json --dev-ui
```

这仍是自建几何对照，不是带人模服装的 S-17 验收或论文原图复刻。
FBX 随附的 normal/AO 等烘焙图尚未进入离线材质适配，不参与此场景。

Ashikhmin、Premože 与 Shirley 2000 Fig. 11 的 Satin / Velvet 是两种独立
离线模型，不是同一材质的粗糙度变体。以下两场景共用同一 FBX、物体变换、
无地面的远处竖直背景板、相机与平行光；主光旋转 `[-25,-5,0]`、
强度 `5`。背景板正常受光，投影落在画面外，不通过关闭阴影修饰布料。
启动后设 `offline bounces 0`：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_paper_case_ashikhmin_premoze_shirley_2000_fig11_satin/SC_paper_case_ashikhmin_premoze_shirley_2000_fig11_satin.json
build/bin/main.exe --offline --initial-scene Maps/SC_Cloth_Ashikhmin2000_Fig11Velvet/SC_Cloth_Ashikhmin2000_Fig11Velvet.json
```

Satin 的纤维方向取 UV0 U，MI 可通过 `u_fiberRotationDegrees` 旋转；
两臂的染色、灯位和机位是本地选择，不是恢复的原场景元数据。
验证结果与差异见 `documents/plan/rendering/shading-model-case-records.md`
的 `AS-01` / `AS-02`。两者实时模式均明确拒绝。

单面方形面光的背面虽不发光，光源几何仍会遮挡相机主射线；调灯位或
尺寸时要避免让它进入主视线。
Estévez 与 Kulla 2017 Fig. 1 的 A 臂红布粗糙度扫描使用独立论文场景，
五块同源布面从左到右取 `r=0.15/0.25/0.40/0.65/1.0`：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_paper_case_estevez_kulla_2017_sheen_roughness/SC_paper_case_estevez_kulla_2017_sheen_roughness.json --dev-ui
```

论文没有给出 Fig. 1 的完整相机、灯光和颜色数值；这里的平行光与灰色
舞台是受控对照所选配置，不是对原图灯型的断言。

Ashikhmin、Premože 与 Shirley 2000 §6.4 / Fig. 11 右图的 Velvet
独立离线 case 使用倒高斯微法线分布 `sigma=0.5` 和相关遮蔽。
场景沿用 VL 布料 FBX，近黑色、背景板、平行光和相机均为本地选择。
论文明确使用单个远处小光源且不包含间接光；在离线控制台执行
`offline bounces 0` 后再观察和导出：

```powershell
build/bin/main.exe --offline --initial-scene Maps/SC_Cloth_Ashikhmin2000_Fig11Velvet/SC_Cloth_Ashikhmin2000_Fig11Velvet.json --dev-ui
```

此 case 验证材质响应，不是 Fig. 11 的像素级复刻，也不增加 runtime `ShadingModelID`。

开发者 UI 的 Offline Renderer 面板可切换模式、设置 spp/反弹/seed、
暂停、继续、停止、重启，查看 Combined/Direct/Indirect 并导出。
使用 `--dev-ui` 启用后，可在运行时按 F10，进入 System 页点击
`Toggle F1 diagnostics` 打开面板；无需依赖可能被其它程序占用的 F1。
控制台也可输入 `offline on|off|samples N|bounces N|seed N|lightSamples N|aov MODE|exposure X|pause|resume|stop|restart|export`。
离线相机移动时以 1 spp 多核预览并放大显示：默认半宽、半高，准备与
等待超过 30 ms 时按每轴 1/2、1/4、1/8、1/16 逐级降档；低档预览
连续三张低于 5 ms 才升一级，给约四倍像素量留出余量，避免频繁跳档。
上一张预览尚未完成时合并后续
相机请求，完成后再取最新视角。最后一次
相机变化后 200 ms 无新变化，恢复原分辨率与面板或命令设置的目标
spp，并重新累积。移动预览与全尺寸导出是不同输入版本，不能混用
像素或宣称复用预览样本。移动预览还带约 30 ms 的软截止：按分散
8×8 tile 计算，截止后不再领取新 tile，已领取 tile 安全完成后发布
有效像素掩码；显示端只用同一预览的有效像素补洞，零有效像素时保留
上一张图。部分预览不进入正式 film、spp 累积或导出，超时期间仍可能
因单条光路或正在执行的 tile 出现软超时；新图未就绪时视口继续显示
上一张已发布结果。
导出位于配置 `resourcePath/Generated/OfflineRenderer/`，包含线性 PFM、
展示 PPM 与 JSON 清单；`--offline` 是进程启动模式，不改变场景资产。
边界、回归与未完成的人工交互验收见
`documents/plan/rendering/offline-renderer-vl-integration-plan.md`。
模块测试由根 CTest 运行；VL 场景导出和压力检查在
`tool/validation/offline/`，不另建独立渲染程序或第二份场景/材质配置。

### SpeedTree 风数值验证

本机安装 `C:\Software\SpeedTree_10.2.0_extracted\{app}` 时，默认构建额外的
`speedtree_wind_validation.exe`。它直接使用 Modeler 的 `SpeedTreeWind.h` 推进
官方 Runtime SDK CPU 状态，并逐层比较 SDK source-space 与 VulkanLearn Y-up
公式：

```bash
cmake --build build --target speedtree_wind_validation -j
build/bin/speedtree_wind_validation.exe
ctest --test-dir build -R speedtree_wind_numeric --output-on-failure
```

默认从 `config/config.json -> resourcePath` 查找
`Maps/SC_speedtree/Source/Models/Oak_Complex_Rules.stsdk`，也可以把其他 `.stsdk` 路径作为第一个参数传入。
Oak 使用固定顶点和贴图接缝回归；其他树种自动选择各 section 中风权重最大的代表顶点。

如果 Modeler 安装在其他位置，配置时传入
`-DSPEEDTREE_MODELER_ROOT=<Modeler 根目录>`。没有 SDK 的机器会跳过这个可选目标，
不会影响 `main.exe`。顶点对照目前是 CPU 上的双公式求值，不是 Vulkan shader 的
GPU readback；实际 SPIR-V 输出仍需后续 capture/readback 验证。

## 运行时资源位置

当前仓库不再以 git 形式保存完整 `resources/` 资产。

程序运行时从 `config/config.json` 的 `resourcePath` 读取资源树。该路径应直接指向独立资源仓库根目录，例如：

```json
"resourcePath": "D:\\YYBWorkSpace\\GitHub\\VukanLearnResources"
```

运行前请确保 `resourcePath` 至少包含：`Common/`、对应的 `Maps/SC_<scene-name>/`，以及用于导入、运行时、验证和截图输出的 `Generated/`。场景专属资产放在场景目录下的 `Meshes/`、`Materials/`、`Textures/`、`Environments/` 和 `Source/`；RenderGraph Pass 材质和引擎级 Profile 放在 `Common/`。

本地生成资源输出写入 `resourcePath/Generated/`。

单次启动需要临时覆盖 `config/config.json -> initScene` 时，可以使用进程级参数，
不会改写配置文件：

```bash
build/bin/main.exe --initial-scene Maps/SC_speedtree/SC_speedtree.json
```

## Shader 增量编译与热重载

启动阶段 Shader 编译是内容寻址增量构建：未修改的 entry/variant 直接命中
`shader/spv/shader-build-cache.json`，不再调用 shaderc。首次启动摘要类似：

```text
Shader build: entries=29, artifacts=17, hits=0, misses=17, compiled=17,
shaderc=29, failed=0, elapsedMs=2043.1
```

第二次无修改启动应看到 `compiled=0, shaderc=0`。缓存身份、manifest schema 和
失败语义见 `documents/rendering/shader-build-cache.md`。

运行时支持以下调试命令：

```text
shaderreload changed   重新编译并事务性发布受影响的 live graphics shader
shaderreload all       强制重新发布全部 live graphics shader
shadercache stats      输出启动构建统计与 manifest artifact 数
```

`shader/glsl` 下的 `.vert/.frag/.comp/.glsl` 与 `M_*.json` 会被自动监听：
CPU 编译在独立 worker 完成，EngineLoop 在 Render Thread 安全点创建 Vulkan
对象并事务提交，旧 Pipeline/descriptor 经 GPU frame epoch 延迟退休。热重载
的 ABI 边界、参与者和事务语义见 `documents/rendering/shader-hot-reload.md`。
连续保存会按 stable source identity 求并集；source epoch 用于快速拒绝旧任务，正式
提交前仍复核 primary/include digest。`M_*.json` schema 更新会在候选 World/Graph
中迁移兼容的 live 参数和贴图，失败时 active runtime 与正式 artifact 整包保持。

可重复验证入口（全部 `--exit-after-tests`，退出码 0 成功、2 失败，串行执行）：

```text
build/bin/main.exe --shader-reload-test --exit-after-tests
build/bin/main.exe --shader-compute-reload-test --exit-after-tests
build/bin/main.exe --world-graph-transaction-test --exit-after-tests
```

`--shader-force-rebuild` 强制重建全部启动 artifact。模块单元/集成测试统一由
GoogleTest 承载，CTest 会按每个 `TEST` 独立发现和报告；首次配置会通过 CMake
FetchContent 获取固定的 GoogleTest 版本。测试覆盖 BLAKE3 向量、规范化边界、
write-if-changed、FileMonitor 防抖、warm start、依赖失效、manifest 损坏/schema
升级和提交回滚：

```bash
ctest --test-dir build --output-on-failure
ctest --test-dir build -R shader_build --output-on-failure
```

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
