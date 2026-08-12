# UE-Lite 完成基线与后续门槛

本文档记录 VulkanLearn 的 UE-Lite 架构收尾基线和后续门槛。`UE-Lite` 只表示早期架构路线名、历史规划语境和验收脚本名；新增 C++ 引擎代码继续使用 `VL` namespace。当前正式架构说明以 `documents/architecture/vulkanlearn-architecture.html` 为准。

## 基线目标

UE-Lite 的完成标准不是“把所有类改成 Unreal 风格命名”，而是把 VulkanLearn 当前的学习 renderer 收敛到一套稳定的 V1 引擎骨架：

- `EngineLoop` 只编排启动、更新、命令执行、快照发布和渲染提交。
- `World` 是 gameplay/runtime 场景状态的唯一归属，GT 可写，RT 不读。
- `WorldSnapshot` 是 GT 到 renderer 的唯一跨线程渲染数据包。
- `RendererFrontend`、`FrameGraphCompiler`、`RendererBackendVulkan`、`RHIDeviceVulkan` 分层清楚；Vulkan 是唯一图形 API，`RHIDeviceVulkan` 是历史命名下的 Vulkan device boundary，不作为跨 API 抽象层继续扩展。
- asset/scene/material/shader 校验输出构建计划，不在校验层创建 Vulkan runtime 对象。
- scene reload、render graph reload、resize、失败回滚和资源 retire 都有自动化验收；启动参数触发的验收命令必须串行运行，避免多个进程同时写入共享的 `shader/spv/` 反射/debug 输出。
- hard cutover 后旧 `SceneLoader` / `LightManager` / renderer 反向读取 mutable scene 的路径不可恢复。

## 当前基线

截至 2026-06-04，代码已经具备以下 UE-Lite / VulkanLearn V1 基线：

- `WorldManager` 管理 active world generation、scene path 和 view target。
- `WorldTransitionCoordinator` 串起 scene validate、renderer resource load、`WorldBuilder` staging world 构建、active world 交换和失败回滚。
- `RuntimeResult` / `RuntimeError` 已作为加载与切换路径的结构化错误边界。
- `RuntimeCommand`、`CommandBus`、`RuntimeCommandExecutor`、`ConsoleSubsystem` 已把 reload/test 操作收敛到命令路径；`--reloadstress`、`--reloadfail*`、`--lightstress`、`--resizestress`、`--graphreloadstress`、`--framesmoke` 和 `--environmentstress` 的启动参数入口都只投递 runtime command。
- `RuntimeTestHooks` 统一持有 reload、rollback、light、resize、render graph reload、frame smoke 和环境增量更新等 runtime 验收状态机；环境测试的天空参数修改也通过 CommandBus 回到 active World owner 侧。
- `EngineLoop` 不保存测试阶段、计数、断言或完成状态，只在稳定帧点调用测试子系统，并提供窗口 / renderer 重建、render graph reload 和帧耗时等既有生产操作与观测值。
- `WorldSnapshot` / `WorldSnapshotQueue` / `WorldSnapshotBuilder` 已作为 GT -> RT 的只读 DTO 和 mailbox。
- `RenderScene` / `ResolvedRenderScene` / `RendererDrawExecutor` 已让 shadow / geometry draw 路径消费 frozen draw packet 和 backend resource entry。
- `RendererBackendVulkan` 已接管 frame begin、submit/present、fence epoch 和主要 GPU helper 门面。
- 旧 `source/render/rhi/rhiDevice.h` 虚接口已删除；`RHIDeviceVulkan` 已成为渲染侧唯一允许直接访问 `VulkanManager::GetInstance()` 的 Vulkan device boundary。
- 旧三角形/tutorial 渲染链 `render.*`、`renderProcess.*`、`swapchain.*`、`shaderModule.*`、`TriangleData.*` 已删除，运行入口只走当前 `RendererBackendVulkan` / `RenderSystem` / frame graph 路径。
- `ResourceRetireQueue` 已接入 world reload、render graph reload、light SSBO 扩容和 fence epoch。
- `tool/ue-lite-boundary-audit.ps1` 已固化关键静态边界。
- mesh runtime 已从 `SceneObject` wrapper 迁到 `WorldMeshObject`；mesh section 的纯数据通过 `MeshObjectBuildPlan` 汇入 `WorldBuildPlan`，`WorldBuilder` 不再读取 renderer cache mesh binding。
- `RendererResourceCache` 已移除 `SceneObject` wrapper 指针表和 mesh object binding 表，仍同时持有 world-local renderer resources、material/texture cache 和回滚快照。
- `RHIDeviceVulkan` buffer、image、image view、sampler、descriptor layout/pool/set、render pass 和 framebuffer 创建、分配、更新、销毁已推进到 opaque `RHIBufferHandle` / `RHIImageHandle` / `RHIImageViewHandle` / `RHISamplerHandle` / `RHIDescriptor*Handle` / `RHIRenderPassHandle` / `RHIFramebufferHandle`；这些历史命名的 handle 只负责 Vulkan 资源生命周期注册和安全释放，不再作为跨 API 路线推进，descriptor/createInfo 保持 Vulkan-native。

当前仍需继续演进的部分：

- `RenderGraph` 数据结构仍承载较多 Vulkan render pass / framebuffer 工作字段，但 graph image、pass descriptor、render pass 和 framebuffer 资源包已携带 lifecycle handles；`FrameGraphCompiler` 后续继续收紧只读 compiled plan 责任。
- asset 管线还没有统一 `AssetManager`；scene、mesh、material、texture、shader 的校验入口仍分散。
- `CommonFunction` 仍承担路径、JSON、数学、Vulkan helper 等混合职责。

## V1 完成定义

UE-Lite V1 完成时必须满足下面的硬门槛。

| 领域 | 完成定义 | 不接受状态 |
| --- | --- | --- |
| World 所有权 | `World` 持有 camera/light/environment/object runtime state；renderer 不持有 mutable gameplay 指针 | RT、pass runtime、draw executor、descriptor 代码回读 `World` / `SceneObject` |
| 加载事务 | 任意 world load 失败后旧 active world、renderer cache、pass material binding 保持可用 | 半切换、半加载、部分新资源污染 active cache |
| 快照边界 | 每帧渲染只消费 `WorldSnapshot` -> `RenderScene` -> `ResolvedRenderScene` | 渲染录制时为了 transform/material/light 回查 mutable scene |
| Renderer / Vulkan device boundary | 上层只调用 backend/device boundary 的意图接口；Vulkan 是唯一图形 API；Vulkan device、queue、swapchain 等全局 raw state 不向上泄漏；lifecycle handle 只表示资源身份和释放目标 | `GetDevice()`、`GetGpuMemoryProperties()`、`GetRhiDevice()` 逃逸口恢复，或新增 `RHIImageLayout` / `RHIDescriptorType` 这类 API-neutral 描述 |
| 资源生命周期 | world-local、graph-local、frame-local GPU 对象都通过 retire 或明确 idle 释放 | reload/resize 时直接销毁仍可能被 GPU 使用的资源 |
| 命令与测试 | 控制台/启动参数只投递命令，runtime tests 走用户同路径 | test hook 直接调用 coordinator、render system 或 render graph 内部 |
| 文档 | `architecture/` 记录当前合同，`plan/` 只保留未来路线 | 已完成合同只留在计划文档，当前实现没有正式说明 |

## 阶段 0：冻结边界基线

目标：把当前已经完成的边界作为不可回退合同。

任务：

- 保留并扩展 `tool/ue-lite-boundary-audit.ps1`，让它继续作为 hard cutover 的静态闸门。
- 明确 `UE-Lite` 只作为历史路线名和验收脚本名，C++ 新代码继续放入 `VL` namespace。
- 在新增系统时先判断归属：Engine、World、Asset、Renderer Frontend、Vulkan Backend、Device Boundary、Diagnostics。
- 对所有 reload/resize/graph reload 路径保持 command-driven 验收，不新增直连 test shortcut。

验收：

```powershell
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
```

出关标准：

- boundary audit 返回 0。
- `rg "SceneLoader|LightManager" source` 不出现旧类型或 include 回流。
- 新增文档不把未实现计划写进 `documents/architecture/` 或 `documents/rendering/`。

## 阶段 1：World Object 彻底脱离 renderer wrapper

目标：把 mesh runtime 从 `SceneObject` 迁到 world-owned object/component 数据，renderer 只接收 snapshot handles。

当前完成情况：

- 已新增 `WorldMeshObject`，World 持有 mesh draw 所需的 model、bounds、mesh/material/materialInstance logical keys。
- `RendererMeshLoader` 已停止创建 `SceneObject` wrapper，改为返回纯数据 `MeshObjectBuildPlan`。
- `RendererResourceCache` 已移除 `BindSceneObject`、`GetSceneObject`、`GetSceneObjects` 和 `sceneObjects` 回滚指纹。
- `WorldBuildPlan` 已携带 `meshObjectPlans`，`WorldBuilder` 不再 include `sceneObject.h`，也不再从 `RendererResourceCache` 读取 mesh binding；`SceneObject` wrapper 文件已删除，基础 `SceneNode` / `Camera` / `Light` 由 `sceneNode.h/.cpp` 承载。
- `WorldSnapshotBuilder` 已从 `World::GetMeshObjects()` 读取 transform、bounds、mesh/material keys。
- `tool/ue-lite-boundary-audit.ps1` 已新增 World mesh object cutover 和 renderer cache mesh binding cutover 规则。

剩余桥接：

- mesh section 展开仍发生在 renderer resource load 阶段，因为 bounds、renderable section key 和 material instance key 目前依赖 renderer mesh/material loader 的现有输出。后续阶段 2 应继续把 terrain、material、texture、shader reflection 的 plan/result 统一起来。

目标结构：

- `SceneAssetObjectPlan` 解析出 object name、transform、mesh asset key 和 material slot 请求。
- `MeshObjectBuildPlan` 承载 mesh section 展开后的 object name、model、bounds、mesh key、material key 和 material instance key。
- `WorldBuilder` 创建 world-owned `WorldMeshObject` record。
- renderer resource loader 只负责 mesh/material/texture/object GPU resource entry，不创建 gameplay wrapper。
- `WorldSnapshotBuilder` 从 world-owned object/component 复制 transform、bounds 和 logical resource handles。
- `RendererResourceCache` 不保存 mutable gameplay wrapper 或 mesh object binding 表。

建议执行顺序：

1. 已完成：新增轻量 `WorldMeshObject` 数据结构，承载现有 mesh object 的必要字段。
2. 已完成：`WorldBuilder` 不再从 `RendererResourceCache::GetSceneObjects()` 导入 wrapper。
3. 已完成：`WorldSnapshotBuilder` 从 world-owned mesh object 构建 `MeshDrawSnapshot`。
4. 已完成：`RendererMeshLoader` 停止创建 `SceneObject`，只注册 renderable/material 资源并返回纯数据 `MeshObjectBuildPlan`。
5. 已完成：`RendererResourceCache` 删除 `BindSceneObject`、`GetSceneObject`、`GetSceneObjects` 和 snapshot 内的 `sceneObjects`。
6. 已完成：`WorldBuildPlan` 携带 `meshObjectPlans`，`WorldBuilder` 不再读取 `RendererResourceCache` mesh binding。
7. 已完成：`RendererResourceCache` 删除 renderer mesh object binding 表。
8. 已完成：boundary audit 禁止 `RendererResourceCache` / `WorldBuilder` / snapshot path 持有 `SceneObject` wrapper，并禁止旧 mesh binding API 回流。

验收：

```powershell
rg -n "sceneObject\.h|std::shared_ptr<SceneObject>|sceneObjects|BindSceneObject\(|GetSceneObject\(|GetSceneObjects\(|AddSceneObject\(" source\render\resource source\world
rg -n "RendererMeshObjectBinding|meshObjectBindings|BindMeshObjectBinding|GetMeshObjectBinding|GetMeshObjectBindings" source tool
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
build\bin\main.exe --reloadstress scenes/SC_speedtree.json 20 --exit-after-tests
build\bin\main.exe --reloadfail-material --exit-after-tests
```

保留约束：

- 如果 `SceneObject` 仍被工具或非渲染路径使用，必须明确留在对应工具/数据目录或注释中说明用途。
- renderer draw path 不允许重新持有 `SceneObject*`。

## 阶段 2：Asset 构建计划统一化

目标：scene、mesh、terrain、material、texture、shader reflection 的校验都输出纯数据计划，resource 创建只在 renderer/resource 或 Vulkan backend/device boundary 阶段发生。

任务：

- 明确 `SceneAssetPlan` 是 scene JSON 的唯一校验输出。
- 为 mesh、terrain、material instance、texture asset 建立同风格 plan/result 类型。
- 已落地的 mesh section 桥接合同保持为：`RendererMeshLoader` 返回 `MeshObjectBuildPlan`，`RendererResourceLoadCoordinator` 汇总到 `RendererWorldResourceLoadResult`，`WorldTransitionCoordinator` 写入 `WorldBuildPlan::meshObjectPlans`，`WorldBuilder` 只消费该纯数据计划。
- 已落地的失败回滚验收保持为：`--reloadfail-material` 覆盖 material 参数失败，`--reloadfail-mesh` 覆盖 mesh/source 预检失败，`--reloadfail-texture` 覆盖 renderer resource load 阶段的 texture source 失败。
- 已落地的 `RuntimeError` code 分类保持为：`Scene.*`、`Mesh.*`、`Material.*`、`Texture.*`、`Shader.*`、`WorldTransition.*`；generated material/mesh/texture rollback 会断言 `Material.LoadFailed`、`Mesh.LoadFailed`、`Texture.LoadFailed`。
- 把 “source data is correct” 的约定写入对应 rendering contract，运行帧不重复做 defensive clamp。
- 将 `CommonFunction` 中与 asset path / JSON parse 强相关的 helper 逐步迁到 `platform/FileSystem` 或 asset util，不在本阶段做大重写。

目标边界：

| 层 | 输入 | 输出 | 禁止 |
| --- | --- | --- | --- |
| Scene validation | scene JSON | `SceneAssetPlan` | 创建 mesh/material/texture runtime object |
| Mesh validation | `SM_*.json` / terrain descriptor | mesh source plan | 上传 vertex/index buffer |
| Material validation | `M_*.json` / `MI_*.json` / reflection | effective material plan | 创建 descriptor set 或 pipeline |
| Texture validation | texture JSON | texture load plan | 创建 Vulkan image |
| Renderer resource load | asset plans | renderer cache entries | 修改 `World` active state |

验收：

```powershell
build\bin\main.exe --reloadfail scenes/DOES_NOT_EXIST.json --exit-after-tests
build\bin\main.exe --reloadfail-material --exit-after-tests
build\bin\main.exe --reloadfail-mesh --exit-after-tests
build\bin\main.exe --reloadfail-texture --exit-after-tests
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
```

完成信号：

- 坏 scene、坏 material、坏 texture、坏 mesh 都能通过 runtime command path 报告失败并完成回滚验证。
- generated material、mesh、texture 失败测试会同时验证错误 code 没有退回泛化的 world transition failure。
- 失败路径不改变 active world generation。
- 失败路径不改变 renderer resource cache / pass material binding 指纹。

## 阶段 3：Renderer frontend / frame graph 合同收口

目标：让 renderer 数据流固定为 `WorldSnapshot` -> `RenderScene` -> compiled graph/pass runtime -> backend command recording。

任务：

- `RendererFrontend` 只把 snapshot 和 render options 转成 `RenderScene`。
- `ResolvedRenderScene` 只解析 logical handles 到 backend resource entries，不保留 gameplay 指针。
- `PassRuntime` 只负责 pass 分派、barrier、render pass begin/end、post-process 特例。
- `RendererDrawExecutor` 继续拥有 object draw loop，所有 draw 所需数据来自 resolved draw packet。
- `FrameGraphCompiler` 输出只读 compiled pass/resource/barrier plan，运行帧不修改原始 graph config。
- render graph reload 使用 retire 模式，不要求全局 wait idle，除非明确处于 shutdown 或保守 resize 路径。

扩展 audit：

- 已固化：禁止 `source/render/pass/*` include `world/*`、`sceneObject.h`，或持有 `World*` / `SceneObject*`。
- 已固化：禁止 `source/render/backend/resolvedRenderScene.*` 持有 `SceneObject` / `World`。
- 已固化：禁止 `RenderSystem` 调用 renderer resource cache 的 object/resource lookup。
- 已固化：禁止 descriptor writer/context/plan 访问 `RenderSystem`、`World`、`SceneObject`，descriptor 更新只能通过显式 `RendererDescriptorContext` 和调用方输入。

验收：

```powershell
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
build\bin\main.exe --graphreloadstress 6 --exit-after-tests
build\bin\main.exe --framesmoke 120 --exit-after-tests
```

完成信号：

- reload graph 时旧 attachment、framebuffer、pass descriptor、render pass 进入 retire queue 并 drain。
- frame smoke 输出 avg/min/max frame time 和 avg FPS。
- Vulkan validation layer 无 object lifetime / layout transition 回归。

## 阶段 4：RendererBackend / Vulkan device boundary 资源句柄推进

目标：保留 Vulkan 学习项目的原生 Vulkan 表达，同时把 device/global state 访问和 GPU 资源生命周期收敛到 backend/device boundary。资源句柄用于 owner/retire/resize/reload 安全释放，不追求跨 API descriptor 或 createInfo 抽象；后续命名清理可以另开阶段处理，但 V1 不再推进 API-neutral backend abstraction。

当前状态：

- `RHIDeviceVulkan` 是直接的 Vulkan device boundary，不再有 `RHIDevice` 虚接口，也不向上暴露 raw device/queue/swapchain getter；raw device/memory/command-pool getter 已收进 private，backend 通过具体 frame/pipeline 意图接口调用。
- `RendererBackendVulkan` header 只 forward declare concrete device boundary，并以 private `std::unique_ptr<RHIDeviceVulkan>` 持有；完整 `rhiDeviceVulkan.h` 只在 backend implementation 中包含，不继续向上层传播 device boundary 细节。
- `RHIDeviceVulkan` buffer 创建、map、copy、destroy 已使用 opaque `RHIBufferHandle`，内部维护 handle -> Vulkan buffer/memory registry。
- `RHIDeviceVulkan` image 创建、layout transition、view 创建、buffer/image copy、mipmap、destroy 已使用 opaque `RHIImageHandle`，内部维护 handle -> Vulkan image/memory registry。
- `RHIDeviceVulkan` image view 和 sampler 创建/销毁已使用 opaque `RHIImageViewHandle` / `RHISamplerHandle`，内部维护 handle -> Vulkan imageView/sampler registry。
- `RHIDeviceVulkan` descriptor layout/pool/set 创建、分配、free、update 和 debug name 已使用 opaque `RHIDescriptorSetLayoutHandle` / `RHIDescriptorPoolHandle` / `RHIDescriptorSetHandle` / `RHIDescriptorWrite`，`RHIDescriptorWrite` 保持 Vulkan-native descriptor type/image info/buffer info，内部维护 handle -> Vulkan descriptor registry。
- `RHIDeviceVulkan` render pass 和 framebuffer 创建/销毁已使用 opaque `RHIRenderPassHandle` / `RHIFramebufferHandle`，内部维护 handle -> Vulkan renderPass/framebuffer registry。
- `RHIDeviceVulkan` frame sync raw collection getter 已收进 private；`RendererBackendVulkan` 只通过单帧/单 swapchain image 的 Vulkan-native sync resource 访问 fence、semaphore 和 command buffer，并在 device boundary 内保留索引错误上下文。queue/swapchain raw getter 也已收进 private，backend 通过 `SubmitToGraphicsQueue()` 和 `PresentSwapchainImage()` 表达 Vulkan submit/present 意图。
- Pipeline shader module、descriptor set layout、pipeline layout、pipeline cache 和 graphics/compute pipeline 创建失败已改为抛出带 operation、pipeline/shader 名称和 Vulkan result 的异常，不再只依赖 `assert(result == vk::Result::eSuccess)`。
- `Buffer` 资源包已保存 per-swapchain `RHIBufferHandle`，`RendererBackendVulkan::CreatePerSwapchainBufferSet()` / `DestroyBufferSet()` 直接使用 handle 管理生命周期；raw `vk::Buffer` 和 descriptor buffer info 保留给 command buffer update / descriptor 写入使用。
- `Texture` 资源包已保存 `RHIImageHandle` / `RHIImageViewHandle` / `RHISamplerHandle`，`DeviceTextureFactory::CreateResourceFromHostImage()` 返回 handle 化 texture resource，`Texture` 析构优先通过 handle 释放；raw image/view/sampler 和 descriptor image info 保留给现有 descriptor 写入使用。
- `RenderGraph` resource/pass 资源包已保存 image/view/sampler、descriptor layout/pool/set、render pass 和 framebuffer lifecycle handles，graph reload retire 与 resize immediate shutdown 都优先通过 handle 释放；raw 字段作为 Vulkan-native attachment、command buffer 和 descriptor 写入工作数据保留。
- `RendererObjectGpuResources` 已保存 object/shadow descriptor pool/set lifecycle handles，object resource shutdown 优先通过 descriptor pool handle 释放；raw descriptor sets 作为当前 draw bind 和 descriptor 写入工作数据保留。
- `RendererBackendVulkan` 对外保留 raw `vk::Buffer` / `vk::Image` / `vk::ImageView` / `vk::Sampler` / `vk::DescriptorSetLayout` / `vk::DescriptorPool` / `vk::DescriptorSet` / `vk::RenderPass` / `vk::Framebuffer` / `vk::DeviceMemory` 形式的 Vulkan-native helper，并在内部维护 raw -> resource handle 映射，服务现有 pipeline key、attachment 和 descriptor info 数据结构；`vk::WriteDescriptorSet` 输入只会把 destination set 映射成 handle，descriptor 内容保持 Vulkan-native。
- pipeline 直接创建的 descriptor set layout 由 backend 注册为 non-owning lifecycle handle，只用于 descriptor set 分配；原 pipeline owner 仍负责销毁。

目标：

- Vulkan device boundary public contract 使用 `RHIBufferHandle`、`RHIImageHandle`、`RHIImageViewHandle`、`RHISamplerHandle`、`RHIDescriptorSetLayoutHandle`、`RHIDescriptorPoolHandle`、`RHIDescriptorSetHandle`、`RHIRenderPassHandle`、`RHIFramebufferHandle` 等轻量句柄来表达所有权和释放目标。
- Vulkan device、queue、swapchain、fence/semaphore 等全局状态只在 `source/render/rhi/vulkan/` 和 backend Vulkan private implementation 内可见；Vulkan descriptor/createInfo 类型允许出现在 Vulkan backend/device boundary 合同中。
- `RendererBackendVulkan` 对上层暴露 Vulkan-native helper，例如 buffer/image/descriptor/render pass/framebuffer 创建与释放。
- resource owner 用 RAII 或显式 `Destroy*` 包装保证释放路径清楚。

建议执行顺序：

1. 已完成第一步：buffer/image/image view/sampler lifecycle handle 合同落地。
2. 已完成第二步：descriptor pool/layout/set 生命周期 handle 合同落地，descriptor 写入保持 Vulkan-native descriptor info。
3. 已完成第三步：render pass/framebuffer 生命周期 handle 合同落地，pipeline key 仍通过 backend Vulkan-native helper 持有 raw Vulkan handle。
4. 已完成第四步的一部分：per-swapchain `Buffer` 资源包携带 `RHIBufferHandle`，frame/object/material UBO/SSBO buffer set 生命周期优先走 handle。
5. 已完成第四步的一部分：`Texture` 资源包携带 image/view/sampler lifecycle handles，普通材质贴图和生成贴图的 `Texture` 析构优先走 handle。
6. 已完成第四步的一部分：`RenderGraph` resource/pass 资源包携带 image/view/sampler、descriptor、render pass 和 framebuffer lifecycle handles，graph reload retire 与 resize shutdown 优先走 handle。
7. 已完成第四步的一部分：`RendererObjectGpuResources` 携带 object/shadow descriptor pool/set lifecycle handles，object resource shutdown 优先走 descriptor pool handle。
8. 后续不再把 descriptor/render pass/framebuffer 参数改成非 Vulkan 描述；优先补生命周期验证、owner 顺序和命名清理。

验收：

```powershell
rg -n "vk::Device|vk::PhysicalDevice|GetDevice\(|GetGpuMemoryProperties\(|GetRhiDevice\(" source
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
build\bin\main.exe --resizestress 6 --exit-after-tests
build\bin\main.exe --framesmoke 120 --exit-after-tests
```

完成信号：

- `RHIDeviceVulkan` 不向上暴露 raw getter，旧 `RHIDevice` 虚接口不可回流，raw getter public contract 由 `ue-lite-boundary-audit.ps1` 固化。
- `RHIDeviceVulkan` buffer 创建、map、copy、destroy 不再使用 raw `vk::Buffer` / `vk::DeviceMemory` 作为 public contract，`ue-lite-boundary-audit.ps1` 固化该规则。
- `RHIDeviceVulkan` image 创建、transition、view 创建、copy、mipmap、destroy 不再使用 raw `vk::Image` / `vk::DeviceMemory` 作为 public contract，`ue-lite-boundary-audit.ps1` 固化该规则。
- `RHIDeviceVulkan` image view 和 sampler 创建/销毁不再使用 raw `vk::ImageView` / `vk::Sampler` 作为 public contract，`ue-lite-boundary-audit.ps1` 固化该规则。
- `RHIDeviceVulkan` descriptor layout/pool/set 创建、分配、free、update、debug name 不再使用 raw `vk::DescriptorSetLayout` / `vk::DescriptorPool` / `vk::DescriptorSet` 作为 public resource identity；descriptor write 保持 Vulkan-native `vk::DescriptorImageInfo` / `vk::DescriptorBufferInfo`，因为本项目当前只学习 Vulkan 后端。
- `RHIDeviceVulkan` render pass/framebuffer 创建、销毁不再使用 raw `vk::RenderPass` / `vk::Framebuffer` 作为 public contract，`ue-lite-boundary-audit.ps1` 固化该规则。
- `RenderGraph` resource/pass 资源包携带 lifecycle handles，`ue-lite-boundary-audit.ps1` 固化该规则。
- `RendererObjectGpuResources` descriptor 资源包携带 lifecycle handles，`ue-lite-boundary-audit.ps1` 固化该规则。
- 除 Vulkan backend/device boundary private 文件外，上层不依赖 swapchain queue/fence/semaphore/raw device。
- resize 和 frame smoke 通过。

## 阶段 5：资源生命周期与回滚完整覆盖

目标：所有会替换 GPU 资源的路径都有明确释放策略和自动化压力验证。

覆盖路径：

- world reload success：旧 world-local resources retire。
- world reload failure：旧 active world 和 renderer cache snapshot restore。
- material load failure：资源加载中途失败后内容级 rollback。
- light SSBO 扩容：旧 frame-local buffer retire。
- swapchain resize：尺寸相关 graph/frame resources 重建，不触发 world reload。
- render graph reload：旧 graph GPU objects retire。
- shutdown：等待 GPU idle 后按 owner 顺序释放。

任务：

- 给 `ResourceRetireQueue` 的 debug output 保持 owner generation、resource category、submitted epoch。
- runtime stress 结束前等待 retire drain，并确认曾观察到 pending 峰值。
- 已增加 `tool/ue-lite-final-validation.ps1` 作为长跑脚本，串行运行最终验收矩阵并把日志写入 `artifacts/ue-lite-validation/<timestamp>/`，覆盖 reload、失败回滚、light buffer retire、resize 和 graph reload 压力。
- 将资源 owner 顺序写入 architecture 当前合同：frame resources -> graph resources -> active world -> resource cache -> Vulkan backend/device boundary。

验收：

```powershell
build\bin\main.exe --reloadstress scenes/SC_speedtree.json 100 --exit-after-tests
build\bin\main.exe --reloadfail scenes/DOES_NOT_EXIST.json --exit-after-tests
build\bin\main.exe --reloadfail-material --exit-after-tests
build\bin\main.exe --lightstress 3 --exit-after-tests
build\bin\main.exe --resizestress 6 --exit-after-tests
build\bin\main.exe --graphreloadstress 6 --exit-after-tests
```

完成信号：

- 所有命令返回 0。
- 失败测试不会改变 active world handle。
- retire queue 最终 drain 到 0。
- validation layer 无 use-after-free、double destroy、layout transition regression。

## 阶段 6：Subsystem 与启动流定稿

目标：把当前启动流固定成 V1 合同，减少 `EngineLoop` 随功能增长继续变胖。

任务：

- 保持 `RuntimeConfig` 负责 project root、config JSON、render graph JSON、launch flags。
- `PlatformApplication` 负责 SDL/Vulkan loader-facing platform setup。
- `SubsystemCollection` 持有 Input、Console、Diagnostics、RuntimeClock、WorldManager 等跨 world subsystem。
- `EngineLoop` 只顺序初始化 subsystem、shader/material generation、renderer backend、pipeline factory、render graph、initial world、console commands。
- runtime commands 在 `RuntimeCommandExecutor` 内转成 coordinator / world mutation / render option / diagnostics 操作；全部 runtime test 状态机统一由 `RuntimeTestHooks` 持有，`EngineLoop` 仅提供帧时机和其本来就拥有的生命周期操作。
- 保留必要错误上下文：启动参数互斥报错、runtime test 冲突报错、rollback 指纹、frame smoke 时间统计和 retire queue 计数都属于验收诊断信息，不按冗余清理。
- 控制器只消费 `InputSubsystem` 和 active view target，不直接读 SDL。

验收：

```powershell
build\bin\main.exe --framesmoke 120 --exit-after-tests
build\bin\main.exe --reloadstress scenes/SC_speedtree.json 20 --exit-after-tests
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
```

完成信号：

- startup order 与 `AGENTS.md` 保持一致。
- 新增 runtime command 不绕过 `CommandBus`。
- window/input/controller 边界没有 SDL include 回流。

## 阶段 7：文档升格与计划清理

目标：实现完成后，把已落地合同移到正式文档，把计划文档保留为路线和历史上下文。

任务：

- 更新 `documents/architecture/vulkanlearn-architecture.html`：只描述当前 V1 合同和已验证命令。
- 如新增 world object/component 格式，更新 `documents/architecture/` 对应 ownership 和 snapshot 章节。
- 如新增 asset JSON 格式，更新 `documents/rendering/` 或新建 asset contract 文档。
- 更新 `documents/README.md`：已完成计划标记为 planning context，当前合同指向 architecture/rendering。
- 删除或合并不再指导实现的临时 notes，避免多个文档同时宣称 source of truth。

验收：

```powershell
rg -n "TODO|FIXME|临时|暂时|看后续|可能需要" documents source
powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1
```

完成信号：

- 当前实现合同只在 `architecture/` 或 `rendering/` 中作为正式说明。
- `plan/` 中只剩未来路线、迁移记录和已废弃方案上下文。
- `README.md` 不承载深层架构细节。

## 最终验收矩阵

UE-Lite V1 宣布完成前，至少执行以下矩阵。runtime 验证命令需要串行执行；启动阶段会写入共享的 `shader/spv/` 编译、反射和 debug 输出，并发进程可能造成非目标失败。

推荐入口：

```powershell
powershell -ExecutionPolicy Bypass -File tool\ue-lite-final-validation.ps1
```

| 验收域 | 命令 / 方法 | 通过标准 |
| --- | --- | --- |
| Build | `cmake --build build -j` | 编译成功 |
| Static boundary | `powershell -ExecutionPolicy Bypass -File tool/ue-lite-boundary-audit.ps1` | 返回 0 |
| Frame smoke | `build\bin\main.exe --framesmoke 120 --exit-after-tests` | 输出 frame stats，返回 0 |
| Environment update | `build\bin\main.exe --environmentstress 3 --exit-after-tests --no-dev-ui` | 增量代际、旧资源窗口、稳定 active 资源和 timestamp 样本均通过 |
| World reload | `build\bin\main.exe --reloadstress scenes/SC_speedtree.json 100 --exit-after-tests` | 无崩溃、无黑屏、retire drain |
| Bad scene rollback | `build\bin\main.exe --reloadfail scenes/DOES_NOT_EXIST.json --exit-after-tests` | 旧 active world 不变 |
| Bad material rollback | `build\bin\main.exe --reloadfail-material --exit-after-tests` | renderer cache/pass material 指纹不变 |
| Bad mesh rollback | `build\bin\main.exe --reloadfail-mesh --exit-after-tests` | mesh/source 预检失败后旧 active world 不变 |
| Bad texture rollback | `build\bin\main.exe --reloadfail-texture --exit-after-tests` | texture 加载失败后 renderer cache/pass material 指纹不变 |
| Light buffer retire | `build\bin\main.exe --lightstress 3 --exit-after-tests` | light SSBO 扩容后旧 buffer retire |
| Resize | `build\bin\main.exe --resizestress 6 --exit-after-tests` | 不触发 world reload，尺寸资源重建 |
| Graph reload | `build\bin\main.exe --graphreloadstress 6 --exit-after-tests` | 旧 graph GPU objects retire |
| Validation layer | 使用 Vulkan validation layer 跑 smoke + reload + resize | 无 lifetime/layout/synchronization 回归 |
| Docs | 检查 `documents/README.md` 和架构/rendering 文档 | 文档 source of truth 不冲突 |

## 禁止项

以下方向会破坏 UE-Lite 收尾目标，除非另有明确设计文档批准：

- 恢复 `SceneLoader` / `LightManager` 旧文件或旧 include。
- 恢复早期三角形/tutorial 渲染链：`render.*`、`renderProcess.*`、`swapchain.*`、`shaderModule.*` 或 `triangleData.*`。
- 为了兼容保留新旧双 runtime 执行路径。
- 在 renderer draw/pass/descriptor 路径保存 `World*`、`SceneObject*` 或 mutable component 指针。
- test hook 直接调用 coordinator/render system/render graph 内部接口绕过 command path。
- asset validation 阶段创建 Vulkan runtime 对象。
- `RHIDevice` 虚接口回流，或 backend facade 重新开放 raw device/queue/swapchain/fence getter。
- 把 Vulkan enum、createInfo、descriptor info 包装成 `RHIImageLayout`、`RHIDescriptorType` 等 API-neutral 类型。
- 在 shader runtime 热路径补与 JSON/source data correctness 重复的 defensive clamp。
- 把未来计划写进 `README.md` 或当前合同文档里伪装成已实现行为。

## 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| terrain/material/texture/shader reflection 继续统一 plan/result 时牵动 loader 边界 | 可能破坏现有 reload rollback | 每次只推进一个 asset 类型，并用对应 `--reloadfail-*` 验证旧 active world 和 renderer cache 指纹 |
| lifecycle handle owner 顺序继续收紧时覆盖面过大 | 容易一次改坏多个 Vulkan helper | 按 resource owner 分批，保留 Vulkan-native createInfo/descriptor info，不引入 API-neutral wrapper |
| reload/resize/graph reload 组合压力不足 | 单项通过但组合生命周期出错 | 增加长跑脚本，至少覆盖连续 reload 后 resize、resize 后 graph reload |
| 文档与代码进度不一致 | 后续 agent 按错误合同改代码 | 已完成内容升格到 architecture/rendering，计划文档保留未来项 |
| 过度抽象影响学习可读性 | 代码变成框架壳，难以学习 Vulkan | V1 只抽 owner/lifetime/boundary，不引入复杂 ECS 或多后端插件系统 |

## 后续维护顺序建议

当前 hard cutover 基线已完成。后续优化按以下顺序推进，减少反复改边界：

1. 继续扩展 boundary audit，冻结当前已经正确的依赖方向。
2. 补齐 terrain/material/texture/shader reflection 的 asset plan/result 和结构化错误。
3. 继续收口 renderer frontend/frame graph/pass/backend 数据流，减少 `RenderGraph` 中可移动到 compiled plan 的工作字段。
4. 按 owner 顺序强化 lifecycle handle 验证和释放顺序。
5. 扩展 runtime stress 矩阵和 validation-layer 长跑。
6. 把新增已完成合同升格到正式 architecture/rendering 文档。

UE-Lite 不再作为新的迁移计划使用；当前正式讨论默认称为 VulkanLearn V1。
