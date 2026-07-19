#pragma once

#include <string>

#include "render/backend/rendererObjectGpuResources.h"

class MaterialInstance;

namespace VL
{

enum class MaterialShadowCasterKind;
struct RendererDescriptorContext;
class RendererBackendVulkan;

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
