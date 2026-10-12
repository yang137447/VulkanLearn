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

引用 swapchain 访问器（`GetSwapChain()`、`GetSwapChainImageCount()`、`GetSurfaceFormat()`、
`GetSwapChainImageViews()`、`ReCreateSwapChain()`）的处数：

| 文件 | 处数 |
| --- | --- |
| `source/rhiDeviceVulkan.cpp` | 11 |
| `source/rendererBackendVulkan.cpp` | 8 |
| `source/renderGraph.cpp` | 7 |
| `source/VulkanManager.h` | 5 |
| `source/rendererObjectResourceManager.cpp` | 4 |
| `source/rhiDeviceVulkan.h` | 4 |
| `source/rendererBackendVulkan.h` | 3 |
| `source/ui/uiOverlayRendererVulkan.cpp` | 3 |
| `source/offlineDisplayVulkan.cpp` | 2 |
| `source/proceduralSkyCubeGenerator.cpp` | 2 |
| `source/vulkanManager.cpp` | 2 |
| 其余 4 个文件各 1 处 | 4 |
| **合计** | **54** |

主要战场是 **RHI 设备层**（`rhiDeviceVulkan` 共 15 处），这一点符合预期：
换窗口呈现对上层应当是"换一个 RHI 目标"，而不是让 render graph 自己关心窗口。

## 阶段划分

每个阶段都必须**单独可构建、可验证**，不允许出现半迁移状态。

### 阶段 1：每窗口的呈现目标（不动场景渲染）

目标：把"surface + swapchain + 图像/视图 + extent + per-window 同步对象"收进一个
**呈现目标对象**，`VulkanManager` 从"持有那一个"变成"持有一个集合"。

- 新建呈现目标类型，把 `VulkanManager` 里的 surface/swapchain/图像/视图/extent/同步对象搬进去
- `VulkanManager` 保留 device 与资源池的所有权，新增按窗口索引的目标
- 现有单窗口路径改为"注册第一个窗口" —— **行为必须完全不变**
- 验证：现有全部测试与渲染结果不变（这是纯搬移，任何画面变化都说明搬错了）

判据：跑通全量测试，且渲染截图与搬移前逐像素一致。

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

## 尚未回答、需要实测的问题

- 场景渲染图有多少资源是 extent 耦合的、多少是尺寸无关的（决定阶段 3 的大小）。
- UI overlay 是否假定占满 swapchain extent（决定阶段 2 能否只渲染 UI）。
- 主循环当前是否有"每帧只处理一个窗口"的隐含假设。
- 事件队列的消费方式是否天然支持多窗口，还是需要一个窗口 id 的分派层。

这些问题由 `VulkanManager` / RHI 层的耦合面测绘回答，测绘结论应补进本文件。
