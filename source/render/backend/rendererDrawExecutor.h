#pragma once

#include <memory>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/backend/rendererObjectGpuResources.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/frontend/renderScene.h"

class MaterialInstance;
class PipelineBase;
struct Renderpass;

namespace VL
{

// Upload callbacks needed by the transitional draw executor. The concrete
// implementation still lives in RenderSystem while frame resources and object
// descriptors finish moving fully behind the backend boundary.
class RendererDrawUploadServices
{
public:
    virtual ~RendererDrawUploadServices() = default;

    virtual void UpdateMaterialInstanceUBOForPass(
        const std::shared_ptr<MaterialInstance>& materialInstance) = 0;
    virtual void UpdateObjectUBOForPass(
        RendererObjectGpuResources& objectResources,
        const RenderDrawPacket& drawPacket) = 0;
    virtual void UploadLightsForPass(
        uint32_t swapChainImageIndex,
        const std::vector<LightSnapshot>& lights) = 0;
};

struct RendererDrawContext
{
    vk::CommandBuffer& commandBuffer;
    const Renderpass& renderPass;
    uint32_t swapChainImageIndex = 0;
    const RenderScene& renderScene;
    const ResolvedRenderScene& resolvedRenderScene;
    RendererDrawUploadServices& services;
};

// Backend-side draw packet execution. It consumes resolved renderable handles
// and backend-owned object GPU resources directly.
class RendererDrawExecutor
{
public:
    bool PipelineUsesDescriptorSet(const PipelineBase& pipeline, uint32_t setIndex) const;
    void DrawShadowScene(const PipelineBase& shadowPipeline, RendererDrawContext& context) const;
    void DrawGeometryScene(RendererDrawContext& context) const;
};

} // namespace VL
