# 编辑器多窗口路线图（路线 2）

> 状态：**计划，未实现**。按 `documents/README.md` 的分层，本文件属于 `plan/`，
> **不是契约**，其中的技术描述不得当作事实使用。实现后职责应写进 `contracts/`。
>
> 决策来源：用户要求"按 UE 的来，走路线 2"。UE 的模型是资产编辑器可以**拖拽成独立窗口**，
> 因此"资产编辑器标签会盖住视口"在 UE 里不是问题 —— 这正是走多窗口的理由。

## 为什么必须做地基改造

当前引擎是**严格单窗口**，且 surface/swapchain 是单例：

| 事实 | 位置 |
| --- | --- |
| 仅一处 `SDL_CreateWindow` | `source/platform/platformWindow.cpp` |
| 仅一处 `SDL_Vulkan_CreateSurface` | `source/vulkanManager.cpp` |
| `VulkanManager` 持有单个 `vk::SurfaceKHR` / `vk::SwapchainKHR` | `source/VulkanManager.h` |
| 仅一处 `Rml::CreateContext(...)` | `source/ui/uiSubsystem.cpp` |

所以"把标签拖成独立窗口"不是配置工作：需要**每窗口一个 surface + swapchain**，
以及每窗口的 UI 上下文与输入路由。

## 参考模型：Blender，不是 UE

两者都需要多窗口，但 Blender 的形态明显更省：

- **每窗口一个 GPU context，共享同一份资源**。`source/blender/gpu/intern/gpu_context_private.hh`
  的文件头写着 "This interface allow GPU to manage GL objects for multiple context and threads"，
  每个 context 持有 `GHOST_IWindow* ghost_window_`（可空 = 离屏），并有 `context_counter`
  说明确实存在多个 context。
- **所有窗口由主线程驱动，按需切换 context**（`Context::get()` / `activate()` / `deactivate()`）。
  这消掉了多线程呈现的同步成本 —— UE 编辑器最贵的那部分在这里不存在。
- **Blender 的 Vulkan 多窗口仍是未完成的 PR**（`blender/blender#126961`）。
  即使是从 GHOST 起就多窗口、且有多年 Vulkan 后端积累的项目，"Vulkan + 多窗口"依然是一块
  独立工作。这决定了本路线图的规模预期：**不要指望一轮做完**。

Vulkan 没有 GL 的隐式 context sharing，因此每窗口必须有**显式**的 surface + swapchain；
可以共享的是 `VkDevice` 与资源池。这就是目标形态。

## 耦合面（实测，不是估计）

> 修订记录：本节初版用的是 `source/render/…` 迁移**之前**的旧路径，且"54"统计的是访问器
> **出现次数**。下面的数字来自逐调用点的可达性测绘，取代初版。

**唯一漏斗**：`VulkanManager` 是 Meyers 单例（`source/VulkanManager.h:18-22`），第 24 行之后
**整个 API 都是 private**，唯一 friend 是 `VL::RHIDeviceVulkan`（`VulkanManager.h:25`）。
裸的 `vk::SwapchainKHR` / `vk::SurfaceKHR` / `PresentInfoKHR` / `acquireNextImage` / `presentKHR`
只出现在 `VulkanManager.h:44,113,118`、`source/render/rhi/vulkan/rhiDeviceVulkan.h:282`、
`rhiDeviceVulkan.cpp:93,117-126,238`。

**结论是好消息**：这不是面条代码，问题是**这个漏斗没有"哪个目标"的键**。

真正的成本不是"改多少处引用"，而是**多少处需要知道目标身份**：

| 类别 | 处数 | 说明 |
| --- | --- | --- |
| 帧路径入口（每帧都要带目标句柄） | **8** | `BeginFrame` / `SubmitFrame` / `RecreateSwapchain` / 4 个 getter / 截图 |
| 初始化期消费者（只在创建时知道一次） | **24** | RenderGraph 8、UI overlay 6、offline display 3、frame resources 5、object resource manager 1、环境 3 |
| 编织/编排点 | **6** | `RecordAndSubmitCurrentRenderScene`、`GetSceneRenderExtent`、Release/Rebuild、`RecreateRendererForWindowResize`、out-of-date 块、验证服务快照 |
| RHI + backend 的签名变更 | **21** | RHI 11 + backend 10 |

**若采用 Blender 形态（目标对象自己持有 swapchain / 视图 / 每图像同步 / 命令缓冲，
由 backend 创建并按句柄传递），逐帧只有 8 个入口需要句柄，其余约 43 处只在 Initialize 时取一次。**
这是选定的形态。

### 共享 vs 每目标

| 对象 | 归属 | 依据 |
| --- | --- | --- |
| `instance` / `device` / `graphicsQueue` / `commandPool` | **共享** | 设备只创建一次；窗口 2 复用 `graphicsQueueFamilyIndex`，只对新 surface 做 `getSurfaceSupportKHR` |
| `imageAcquiredSemaphores` | **共享**（`MAX_FRAMES_IN_FLIGHT` = 2） | 帧级，不是图像级；✅ 已与设备同生命周期（`9d929f8`） |
| `taskFinishedFences` | **共享**（同上） | 帧级；✅ 同上 |
| `surface` / `surfaceFormat` / `swapChainExtent` / `swapChainImageCount` / `swapChain` / `presentImages` / `swapChainImageViews` | **每目标** | ✅ 已移入 `PresentTarget`（`8c52b07`） |
| `renderFinishedSemaphores` / `imagesInFlightFences` / `commandBuffers` | **每目标**，按 `swapChainImageCount` 分配 | ✅ 均已移入 `PresentTarget`（`a183bbf` / `9d929f8`） |
| `presentQueueFamilyIndex` | **每目标**（对各自 surface 解析） | ⬜ 已移入 `PresentTarget`，但解析仍只对目标 0 做（`vulkanManager.cpp:133`） |

两套索引的纪律记录在 `source/render/backend/rendererBackendVulkan.cpp:1803-1832`，
由 `rhiDeviceVulkan.cpp:27-41` 的 `RequireFrameSyncIndex` 强制。

## 阶段划分

每个阶段都必须**单独可构建、可验证**，不允许出现半迁移状态。

### 阶段 1：每窗口的呈现目标（不动场景渲染）

目标：把"surface + swapchain + 图像/视图 + extent + per-window 同步对象"收进一个
**呈现目标对象**，`VulkanManager` 从"持有那一个"变成"持有一个集合"。

**进度：**

- ✅ **第一片（`8c52b07`）**：新增 `VL::PresentTarget`（`source/render/rhi/vulkan/presentTarget.h`），
  surface / swapchain / 图像 / 视图 / extent / 图像数 / 每图像同步对象搬进去；
  `VulkanManager` 持有 `std::vector<PresentTarget>`。**现有访问器签名不变，委托给目标 0**，
  因此约 54 处调用点一行未改 —— 这一片因此可证行为中性。
  验证：搬移前后同场景 1280x720 截图逐像素比对，差异 **37 像素且全部落在
  `x1238..1243, y705..712`（状态栏 FPS 计数）**，区域外 **0** 差异。
- ✅ **第二片（`ecc1834`）**：呈现函数改为接受 `VL::PresentTarget&`（RHI 10 个 + backend 帧路径），
  并**删掉** `VulkanManager` 的 8 个按目标访问器与 RHI 的 `GetSwapchain()`。
  身份用**引用**而不是索引：调用点要知道"哪个目标"，而不是"容器第几项"。
  默认目标只剩一处（`VulkanManager::defaultTarget()`），让"多窗口尚未接线"这个事实可被 grep。
  **仍未做到**：backend 的 `BeginFrame`/`SubmitFrame` 与 `RenderGraph` 依然隐含主窗口 ——
  这一片消除的是"当初不可能"的原因，不是把第二窗口打通。
- ✅ **第三片（`a183bbf`）**：命令池与命令缓冲拆成两个生命周期。
  共享池只在 `Init` 建一次、析构销毁一次，**重建路径完全不碰它**；
  每图像主命令缓冲移入 `PresentTarget`（与同样按图像数分配的信号量/fence 并列）。
  这同时**修掉一个真实缺陷**：`DestroyVkCommandBuffer` 以前会销毁共享池，而
  `BeginSingleTimeCommands` 与着色器热重载校验的 8 个调用点都在吃那个池 ——
  也就是**每次窗口 resize 都会毁掉别的用途正在用的池**，只因销毁与重建在同一函数里紧挨着才没炸。
  验证覆盖了这条路径本身：启动后**真实 resize 两次**（1536x879 → 1136x669 → 1536x879），
  进程与窗口存活、渲染继续（截图 1520x840，非背景采样点 141366）。
- ✅ **第四片（`9d929f8`）**：同步对象按分配尺寸拆开。
  帧级（`imageAcquiredSemaphores` / `taskFinishedFences`，按 `MAX_FRAMES_IN_FLIGHT`）
  生命周期与设备一致；每目标（`renderFinishedSemaphores` / `imagesInFlightFences`，按图像数）
  随该目标重建。**重建路径现在只调用六个目标作用域函数，不碰任何全局对象** ——
  这正是阶段 2 需要的性质：窗口 A 的 resize 不得扰动窗口 B。
  验证：真实 resize 两次，进程存活、渲染继续、**日志无 validation 报错或 VUID**
  （同步对象改错的表现通常是校验层报错而不是崩溃，所以这一条比平时更重要）。

**阶段 1 完成后的目标形态**：`ReCreateSwapChain` 的全部调用都在单个目标上，
共享的只剩设备、队列、命令池与帧级同步对象 —— 它们按设备生命周期分配，与窗口无关。

判据：跑通全量测试，且渲染截图与搬移前逐像素一致（允许状态栏 FPS 文本变化）。

### 阶段 2：第二个窗口能开出来并呈现

目标：第二窗口有自己的 swapchain，**只渲染 UI**，场景渲染图不动。

- 平台层：多 `SDL_Window` 注册表、窗口生命周期、事件归属
- UI 层：每窗口一个 `Rml::Context`（同一份 RML 可被多个 context 各自加载实例）
- 主循环：单线程按窗口遍历，切换呈现目标后记录+提交+呈现

判据：第二个窗口能显示编辑器 UI、能响应点击，且第一窗口的画面与阶段 1 完全一致。

### 阶段 3：场景渲染跟随窗口

目标：让场景渲染图按**目标窗口的 extent** 工作，而不是按那个单例 extent。

- 把与 extent 耦合的资源（framebuffer、attachment、viewport/scissor、UI overlay 的尺寸来源）
  改为按呈现目标推导
- 需要回答的关键问题：**同一份场景渲染到两个不同尺寸的窗口时，是各自渲染一遍，
  还是渲染一次再缩放**。UE 与 Blender 都是各自渲染（每个视图有自己的视口）。
  本仓库应各自渲染，但这一步要单独验证成本。

### 阶段 4：标签拖出 / 拖入

目标：把停靠树里的标签拖到窗口外 → 成为新窗口的根；拖回 → 并入某个栈。

- 复用现有拖拽状态机（`EditorDockingController::UpdateDrag` / `FinishDrag` / `DockEdge`）
- 跨窗口交接：标签所在的面板要从窗口 A 的 context 迁移到窗口 B 的 context
- 这里会遇到一个已知硬约束：`DockWorkspace::Validate` 要求基础面板恰好
  `Panels().size() - 1` 个，且 `EditorDockingController` 的 `BodyId`/`TabId` 对未注册名
  有**静默回退到 Renderer 正文**的兜底（见 `rationale/pitfalls.md`）。
  多窗口会让"哪些面板在哪个窗口"成为布局状态的一部分，持久化格式需要相应扩展。

## 已知风险

1. **swapchain 重建与多窗口的交互**：窗口 A 重建时窗口 B 可能正在帧中。需要先弄清当前
   重建会拆掉哪些全局对象（图像、视图、framebuffer、描述符集），再决定重建的作用域。
2. **`DockWorkspace` 的校验硬编码**：新增面板名会让既有 `workspace-v1.json` 失效，
   需要迁移路径（仓库已有 v1 注入 `"resource"` 的先例）。
3. **渲染验证成本**：阶段 3 之后，任何渲染改动都要在**两个窗口尺寸**下验证，
   否则"在一个尺寸下对"会掩盖另一个尺寸的假设。
4. **不要一轮做完**。阶段 1 与 2 是地基，各自都够大；把 3、4 混进来会让失败原因不可分辨。

## 与既有决策的关系

- 用户否决了"路线 1（主窗口内浮动面板）"作为替代方案：UE 的行为是**独立窗口**，
  而 `DockWorkspace` 的浮动矩形是主窗口内的归一化坐标，做不到。
- 用户否决了"路线 C（不做资产编辑器标签）"：那会保留 Details 同时承载
  actor 与材质参数两块内容，与 UE 分工不符。
- `plan/ui/editor-tab-responsibility-design.md` 记录了职责层面的差距分析（哪个面板承载什么），
  本文件只负责**实现路径**。

## 已经查清的问题（取代初版的"待实测"清单）

- **场景渲染图基本全是 extent 耦合的。** `renderGraph.cpp` 的每个资源宽度都来自
  `swapchainExtent`（`CreateRenderResource`），只有带 `hasFixedWidth/Height` 的资源与
  `shadowMap` 例外。所以阶段 3 比预想大：不是"补几个尺寸来源"，而是"把 extent 从单例概念
  改成按目标传入"。**阶段 2 之所以能便宜，正是因为完全不碰它。**
- **UI overlay 确实假定占满 swapchain extent**（`uiOverlayRendererVulkan.cpp:380` 的
  `setRenderArea` 用 swapchain extent），但它的 pass 是**自足的**：自己的 render pass、
  自己的 framebuffer、自己的每图像顶点/索引缓冲与描述符集。而且它**已经有一个 `loadOp=eClear`、
  `initialLayout=eUndefined` 的变体**（`uiOverlayRendererVulkan.cpp:545-546`），
  正是"只渲染 UI 的第二窗口"需要的东西。**阶段 2 可以完全不碰 `renderGraph.cpp` /
  `passRuntime.cpp` / `passBarrier.cpp`。**
- **主循环确实假设只有一个窗口。** 每帧入口是 `RenderSystem::RecordAndSubmitCurrentRenderScene`
  （`renderSystem.cpp:2990`），它同时做 acquire、录制、提交、present；
  `PlatformEvent`（`source/platform/platformEvent.h:109-134`）**没有窗口 id 字段**，
  事件泵（`platformApplication.cpp:232-392`）对窗口不区分。所以需要一个窗口 id 分派层。
- **同步对象的归属已明确**（见上表）：`imageAcquiredSemaphores` 与 `taskFinishedFences` 是帧级、
  可共享；`renderFinishedSemaphores` / `imagesInFlightFences` / `commandBuffers` 是按图像数分配、
  必须每目标。

## 测绘发现的既有缺陷（多窗口会放大它们）

这几条**现在就存在**，不是多窗口引入的；但阶段 1/2 会让它们变成真实故障，所以先记录：

1. **`surfaceFormat` 可能保持默认构造。** `CreateVkSwapChain` 只在
   `R8G8B8A8Srgb` + `eSrgbNonlinear` 被提供时才赋值（`vulkanManager.cpp:219-226`），
   **没有回退到 `surfaceFormats[0]`**，而 `:292-293` 无条件使用它。第二个 surface 的可用格式
   一旦不同，这条就会暴露。
2. ✅ **已修（`a183bbf`）** —— **共享 `commandPool` 曾被重建路径销毁。** `DestroyVkCommandBuffer`
   会销毁 `commandPool`，而同一个池还服务所有 `BeginSingleTimeCommands`
   （`rhiDeviceVulkan.cpp:727-749`）与 shader 热重载校验路径，于是**每次窗口 resize 都会毁掉
   别的用途正在用的池** —— 只因销毁与重建在同一函数里紧挨着才没炸。
   现在池的生命周期与设备一致（`Init` 建一次、析构销毁一次），每图像命令缓冲移入
   `PresentTarget`，重建只动自己的目标。已在**真实 resize** 下验证（见阶段 1 第三片）。
3. **RmlUi 在本仓库是进程级单例。** `Rml::Initialise`/`Rml::Shutdown`
   （`uiSubsystem.cpp:823,325`）是全局的，且 context 名硬编码为 `"vulkanlearn"`（`:832`）。
   两个 `UiSubsystem` 不能按现状共存：第二个的 Shutdown 会拆掉第一个，`CreateContext` 同名会失败。
   但 `CreateContext` 接受**每 context 的 render interface**（`:836`），所以渲染接口本身不是障碍。
4. **逻辑尺寸与像素尺寸可能已经不一致。** `WindowResized` 带的是逻辑尺寸
   （`platformApplication.cpp:379-380`）并直接喂给 `RecreateRendererForWindowResize`
   （`engineLoop.cpp:1041`），而 out-of-date 路径用 `SDL_GetWindowSizeInPixels`（`:519`）。
   在 DPI 缩放下这两者会不同 —— 也就是说 **UI context 与 swapchain 可能今天就来自不同的数字**。
   值得在做多窗口**之前**先测一次，否则会把错误前提当地基。
5. **`RendererFrameContext` 是裸句柄值类型**（`rendererBackendVulkan.h:67-77`），跨 record 与
   submit 持有，**只带索引、不带目标身份** —— 这正是"跨窗口重建会悬垂"的原因。
   阶段 1 之后它必须能表达"属于哪个目标"。
6. **重建的作用域目前是全局的。** `ReCreateSwapChain` 先 `device.waitIdle()`
   （`vulkanManager.cpp:46`，设备级，会顺带排空窗口 B 的工作），再销毁**所有**窗口共用的
   同步对象与命令缓冲。上层 `RecreateRendererForWindowResize`（`engineLoop.cpp:1251`）更狠：
   拆掉整个 extent 耦合世界（render graph、UI overlay、offline display、资源缓存）。
   多窗口需要一条规则：**只在帧边界、要么全目标一起、要么都不做**。

