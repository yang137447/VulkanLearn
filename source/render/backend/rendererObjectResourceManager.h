#pragma once

#include <string>

#include "render/backend/rendererObjectGpuResources.h"

class MaterialInstance;

namespace VL
{

enum class MaterialShadowCasterKind;
struct RendererDescriptorContext;
class RendererBackendVulkan;

// 运行时替换材质贴图后，只重写当前已 acquire 的 swapchain image 的对象描述符集。
void UpdateObjectDescriptorSetsForImage(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    MaterialInstance& materialInstance,
    RendererObjectGpuResources& resources,
    uint32_t imageIndex);

// Creates and destroys the per-object GPU resource package used by the current
// draw path. RendererObjectResourceEntry owns the package lifetime; this
// manager performs Vulkan allocation and descriptor writes for that entry.
class RendererObjectResourceManager
{
public:
    void InitializeObjectResources(
        RendererBackendVulkan& rendererBackend,
        const RendererDescriptorContext& descriptorContext,
        const std::string& objectName,
        MaterialInstance& materialInstance,
        MaterialShadowCasterKind shadowCasterKind,
        RendererObjectGpuResources& resources) const;

    void ShutdownObjectResources(
        RendererBackendVulkan* rendererBackend,
        RendererObjectGpuResources& resources) const;
};

} // namespace VL
