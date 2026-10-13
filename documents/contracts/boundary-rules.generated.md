# 架构边界规则索引（生成物，请勿手工编辑）

> 状态：**当前契约**（实现已落地）。本文件由 `tool/generate_boundary_rule_index.py` 从 `tool/architecture-boundary-audit.ps1` 生成；`tool/validate-docs.py` 会在生成物过期时判失败。改规则请改脚本再重新生成，不要编辑本文件。

脚本是**可执行的架构契约**：它定义代码不得违反什么。规则正文以脚本为唯一真相，本表只是索引——
它说明每条规则的名字、它做什么检查、以及它作用于哪些路径。

生成源 revision：`48970c01c95860722de2d28700bb6911768ad4f9`

| 规则名 | 检查类型 | 作用对象 |
| --- | --- | --- |
| Vulkan-native architecture wording | pattern must not match in a file | `documents/README.md`, `documents/contracts/vulkanlearn-architecture.html`, `documents/contracts/descriptor-imageinfo-management.md`, `documents/plan/architecture/ue-lite-completion-plan.md` |
| VL namespace naming in documents | pattern must not match in a file | `documents/README.md`, `documents/contracts/descriptor-imageinfo-management.md` |
| Removed scene/light singleton cutover | path must not exist | `source/sceneLoader.h`, `source/sceneLoader.cpp`, `source/lightManager.h`, `source/lightManager.cpp` |
| Removed scene/light singleton references | pattern must not match anywhere in source | `（全仓库源码树）` |
| Removed tutorial render path cutover | path must not exist | `source/render.h`, `source/render.cpp`, `source/renderProcess.h`, `source/renderProcess.cpp`, `source/swapchain.h`, `source/swapchain.cpp` 等 12 项 |
| Removed tutorial render path references | pattern must not match anywhere in source | `（全仓库源码树）` |
| Removed VulkanManager cached submit path | pattern must not match in a file | `source/VulkanManager.h`, `source/vulkanManager.cpp`, `source/pipeline/graphicsPipeline.h` |
| VulkanManager private platform dependency boundary | pattern must not match in a file | `source/VulkanManager.h` |
| VulkanManager historical naming cleanup | pattern must not match in a file | `source/VulkanManager.h`, `source/vulkanManager.cpp`, `source/render/rhi/vulkan/rhiDeviceVulkan.cpp` |
| CommonFunction depth helper cleanup | pattern must not match in a file | `source/commonFunction.h` |
| Source comment clarity | pattern must not match anywhere in source | `（全仓库源码树）` |
| Draw frame boundary | pattern must not match in a file | `（路径由脚本内条件生成）` |
| Render pass gameplay boundary | pattern must not match in a directory | `（全仓库源码树）` |
| Descriptor writer explicit context boundary | pattern must not match in a file | `source/render/backend/rendererDescriptorContext.h`, `source/render/backend/rendererDescriptorWriter.h`, `source/render/backend/rendererDescriptorWriter.cpp`, `source/render/backend/rendererDescriptorPlan.h`, `source/render/backend/rendererDescriptorPlan.cpp` |
| RenderSystem frame boundary | pattern must not match in a file | `source/renderSystem.cpp` |
| Snapshot previous model ownership | pattern must match in a file | `（路径由脚本内条件生成）` |
| Resolved draw resource ownership | pattern must match in a file | `（路径由脚本内条件生成）` |
| World mesh object cutover | pattern must not match in a file | `source/render/resource/rendererResourceCache.h`, `source/render/resource/rendererResourceCache.cpp`, `source/render/resource/rendererMeshLoader.h`, `source/render/resource/rendererMeshLoader.cpp`, `source/world/world.h`, `source/world/world.cpp` 等 8 项 |
| SceneObject wrapper file cutover | path must not exist | `source/sceneObject.h`, `source/sceneObject.cpp` |
| Renderer cache mesh binding cutover | pattern must not match anywhere in source | `（全仓库源码树）` |
| Mesh object build plan contract | pattern must match in a file | `（路径由脚本内条件生成）` |
| Renderer resource load result contract | pattern must match in a file | `（路径由脚本内条件生成）` |
| World build plan mesh object contract | pattern must match in a file | `（路径由脚本内条件生成）` |
| WorldBuilder mesh object plan consumption | pattern must match in a file | `（路径由脚本内条件生成）` |
| Runtime test hook command boundary | pattern must not match in a file | `source/engine/runtimeTestHooks.h`, `source/engine/runtimeTestHooks.cpp` |
| Redundant Vulkan device abstraction cutover | path must not exist | `source/render/rhi/rhiDevice.h` |
| Vulkan device boundary concrete-only contract | pattern must not match in a file | `source/render/rhi/vulkan/rhiDeviceVulkan.h` |
| Vulkan device boundary buffer lifecycle handle contract | pattern must not match in a file | `source/render/rhi/vulkan/rhiDeviceVulkan.h` |
| Vulkan device boundary buffer lifecycle handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| Buffer resource package lifecycle handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| Texture resource package lifecycle handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| Device texture factory lifecycle handle result | pattern must match in a file | `（路径由脚本内条件生成）` |
| Backend texture handle bridge contract | pattern must match in a file | `（路径由脚本内条件生成）` |
| RenderGraph resource package lifecycle handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| Object GPU resource descriptor handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| lifecycle handle definitions | pattern must match in a file | `（路径由脚本内条件生成）` |
| Vulkan device boundary image lifecycle handle contract | pattern must not match in a file | `source/render/rhi/vulkan/rhiDeviceVulkan.h` |
| Vulkan device boundary image lifecycle handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| Vulkan device boundary image view sampler lifecycle handle contract | pattern must not match in a file | `source/render/rhi/vulkan/rhiDeviceVulkan.h` |
| Vulkan device boundary image view sampler lifecycle handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| Vulkan device boundary descriptor lifecycle handle contract | pattern must not match in a file | `source/render/rhi/vulkan/rhiDeviceVulkan.h` |
| Vulkan-native descriptor write type | pattern must match in a file | `（路径由脚本内条件生成）` |
| Vulkan device boundary descriptor lifecycle handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| Vulkan-native descriptor write contract | pattern must not match in a file | `source/render/rhi/rhiResourceHandles.h`, `source/render/backend/rendererBackendVulkan.cpp`, `source/render/rhi/vulkan/rhiDeviceVulkan.h`, `source/render/rhi/vulkan/rhiDeviceVulkan.cpp` |
| Vulkan device boundary render pass framebuffer lifecycle handle contract | pattern must not match in a file | `source/render/rhi/vulkan/rhiDeviceVulkan.h` |
| Vulkan device boundary render pass framebuffer lifecycle handle adoption | pattern must match in a file | `（路径由脚本内条件生成）` |
| RendererBackend device boundary escape hatch | pattern must not match in a file | `source/render/backend/rendererBackendVulkan.h` |
| RendererBackend hides concrete device boundary | pattern must not match in a file | `source/render/backend/rendererBackendVulkan.h` |
| RendererBackend device boundary forward declaration | pattern must match in a file | `（路径由脚本内条件生成）` |
| Automated runtime validation entry | pattern must match in a file | `（路径由脚本内条件生成）` |
| Launch runtime tests use CommandBus | pattern must not match in a file | `source/engine/launchOptions.cpp` |
| Runtime test state stays out of EngineLoop | pattern must not match in a file | `source/engine/engineLoop.cpp` |
| Swapchain resize production lifecycle | pattern must match in a file | `（路径由脚本内条件生成）` |
| Render graph reload production lifecycle | pattern must match in a file | `（路径由脚本内条件生成）` |
| Render graph resource retire ownership | pattern must match in a file | `（路径由脚本内条件生成）` |
| World reload failure state machine validation | pattern must match in a file | `（路径由脚本内条件生成）` |
| World reload failure fixture validation | pattern must match in a file | `（路径由脚本内条件生成）` |
| World reload failure command result validation | pattern must match in a file | `（路径由脚本内条件生成）` |
| LoadWorld path resolve runtime error contract | pattern must match in a file | `（路径由脚本内条件生成）` |
| World loader runtime error code contract | pattern must match in a file | `（路径由脚本内条件生成）` |
| Renderer resource load runtime error code contract | pattern must match in a file | `（路径由脚本内条件生成）` |
| Pipeline Vulkan creation diagnostics | pattern must not match in a file | `source/pipeline/computePipeline.cpp`, `source/pipeline/graphicsPipeline.cpp`, `source/pipeline/graphicsPipelineBuilder.cpp`, `source/pipeline/pipelineLayoutBuilder.cpp` |
| Pipeline Vulkan creation diagnostic context | pattern must match in a file | `（路径由脚本内条件生成）` |
| World graph retire queue validation | pattern must match in a file | `（路径由脚本内条件生成）` |
| Architecture final validation script contract | pattern must match in a file | `（路径由脚本内条件生成）` |

共 65 条规则。
